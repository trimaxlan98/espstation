#!/usr/bin/env python3
"""Puente serie -> navegador: muestra el CIRCUITO REAL en el visor, en tiempo real.

Un HTML no puede leer el puerto serie (Firefox no tiene Web Serial), asi que este script:
  1. lee los dos puertos (A = llave/transmisor, B = receptor/decodificador) a 115200,
  2. interpreta cada linea que imprimen los sketches (ver SPEC-MORSE.md),
  3. sirve `visor/` por HTTP y le manda los eventos al navegador por SSE (`/eventos`).

Uso (con las placas conectadas y el sketch de cada una subido):
    python3 herramientas/puente_serie.py --a /dev/ttyUSB0 --b /dev/ttyUSB1
    # y abrir  http://127.0.0.1:8765/#vivo

Sin hardware (repite una tanda REAL de la evidencia; las esperas de mas de 5 s se comprimen a 1,5 s):
    python3 herramientas/puente_serie.py --demo 3

Visor Y evidencia a la vez (un puerto serie solo lo abre UN proceso, asi que el visor y
captura_serie.py son excluyentes; con esto el puente hace las dos cosas). El log sale en
el mismo formato que captura_serie.py:
    python3 herramientas/puente_serie.py --a /dev/ttyUSB0 --b /dev/ttyUSB1 \
        --log-a evidencia/sesion_A.log --log-b evidencia/sesion_B.log --marcas /tmp/marcas
    echo "#MARK INICIO TANDA 1" >> /tmp/marcas     # entra en los DOS logs

Practica hermana morse-duplex (las dos placas con transceptor.ino, sin roles, lineas
prefijadas RX/TX). `--duplex` cambia el interprete y la lista blanca de comandos, y sirve
el visor de esa practica:
    python3 herramientas/puente_serie.py --duplex --a COM5 --b COM7 \
        --log-a ../morse-duplex/evidencia/t_A.log --log-b ../morse-duplex/evidencia/t_B.log

Reproducir una tanda ya grabada (cualquier par de logs de captura_serie.py), util para
revisar una sesion o para probar el visor sin placas:
    python3 herramientas/puente_serie.py --duplex --velocidad 4 \
        --reproducir-a ../morse-duplex/evidencia/t_A.log \
        --reproducir-b ../morse-duplex/evidencia/t_B.log

Requisitos: python3; pyserial solo para los puertos reales (pip install pyserial); en Linux,
permiso sobre el puerto (grupo `dialout`). Solo escucha en 127.0.0.1.

Seguridad: solo se aceptan comandos de la lista blanca del SPEC (r, c, v, p/l/w/d<ms>) y solo desde
el propio visor servido por este puente (mismo origen). La lectura (`/eventos`, `/estado`) es
de solo lectura y admite CORS para abrir el visor desde un archivo local.
"""
import argparse
import collections
import os
import json
import queue
import re
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent

# ------------------------------------------------------------------ interpretacion de lineas
RE_KV = re.compile(r"(\w+)=(\d+)")
RE_A_FLANCO = re.compile(r"^(\d+),([01])$")
RE_B_LETRA = re.compile(r"^\[letra: (.)\] \[bin: ([01]{8})\]$")
RE_B_DESC = re.compile(r"^\[letra: \?\] \[morse: ([.\-]+)(\+?)\]$")
RE_B_PULSO = re.compile(r"^# pulso_ms=(\d+)( filtrado)?$")
RE_B_SIL = re.compile(r"^# silencio_ms=(\d+)$")
COMANDO_OK = re.compile(r"^(?:[rcv]|[plwd]\d{1,5})$")

# --- Formato DUPLEX (../morse-duplex/transceptor/transceptor.ino) -------------
# Ahi no hay roles: las dos placas imprimen lo mismo y cada linea de simbolo lleva
# el sentido delante ("RX " lo que llega del otro, "TX " el eco de la propia llave).
# Se deja aparte de parsear() a proposito: el interprete de clave-morse no se toca.
RE_D_SIMB = re.compile(r"^(RX|TX) ([.\-])$")
RE_D_LETRA = re.compile(r"^(RX|TX) \[letra: (.)\] \[bin: ([01]{8})\]$")
RE_D_DESC = re.compile(r"^(RX|TX) \[letra: \?\] \[morse: ([.\-]+)(\+?)\]$")
RE_D_PALAB = re.compile(r"^(RX|TX) \[palabra\]$")
RE_D_PULSO = re.compile(r"^# (RX|TX) pulso_ms=(\d+)( filtrado)?$")
RE_D_SIL = re.compile(r"^# (RX|TX) silencio_ms=(\d+)$")
RE_D_FLANCO = re.compile(r"^# TX flanco t_ms=(\d+) nivel=([01])$")
RE_D_ETIQ = re.compile(r"^# (umbrales|contadores) (RX|TX) ")
COMANDO_DUPLEX_OK = re.compile(r"^(?:[rcve]|k\d{1,5}|t?[plwd]\d{1,5})$")


def limpiar(linea: bytes):
    """Quita los bloques 0xFF de basura de captura y los CR/LF. Devuelve (texto, tenia_basura)."""
    basura = b"\xff" in linea
    txt = linea.replace(b"\xff", b"").decode("latin1").replace("\r", "").replace("\n", "")
    return txt, basura


def parsear(src: str, texto: str, basura: bool = False):
    """Devuelve un dict de evento (sin marca de tiempo) o None si la linea esta vacia."""
    texto = texto.strip()
    if not texto:
        return {"tipo": "basura"} if basura else None
    if any(ord(c) < 32 or ord(c) > 126 for c in texto):
        return {"tipo": "basura", "v": len(texto)}
    if src == "A":
        m = RE_A_FLANCO.match(texto)
        if m:
            return {"tipo": "flanco", "t_ms": int(m.group(1)), "nivel": int(m.group(2))}
        if texto.startswith("# resumen"):
            return dict({"tipo": "resumen"}, **{k: int(v) for k, v in RE_KV.findall(texto)})
        if texto.startswith("# contadores"):
            return dict({"tipo": "contadores"}, **{k: int(v) for k, v in RE_KV.findall(texto)})
        if texto.startswith("# umbrales"):
            return dict({"tipo": "umbrales"}, **{k: int(v) for k, v in RE_KV.findall(texto)})
    else:
        if texto in (".", "-"):
            return {"tipo": "simbolo", "v": texto}
        if texto == "[palabra]":
            return {"tipo": "palabra"}
        m = RE_B_LETRA.match(texto)
        if m:
            return {"tipo": "letra", "letra": m.group(1), "bin": m.group(2)}
        m = RE_B_DESC.match(texto)
        if m:
            return {"tipo": "letra", "letra": "?", "bin": None, "morse": m.group(1), "desbordado": bool(m.group(2))}
        m = RE_B_PULSO.match(texto)
        if m:
            return {"tipo": "pulso", "ms": int(m.group(1)), "filtrado": bool(m.group(2))}
        m = RE_B_SIL.match(texto)
        if m:
            return {"tipo": "silencio", "ms": int(m.group(1))}
        if texto.startswith("# contadores"):
            return dict({"tipo": "contadores"}, **{k: int(v) for k, v in RE_KV.findall(texto)})
        if texto.startswith("# umbrales"):
            return dict({"tipo": "umbrales"}, **{k: int(v) for k, v in RE_KV.findall(texto)})
    return {"tipo": "texto", "v": texto}


def parsear_duplex(src: str, texto: str, basura: bool = False):
    """Como parsear(), pero para el transceptor duplex. Cada evento lleva `sentido`
    ("RX" = la mano del otro operador, "TX" = el eco de la mia). `src` no decide
    nada aqui: las dos placas hablan igual."""
    texto = texto.strip()
    if not texto:
        return {"tipo": "basura"} if basura else None
    if any(ord(c) < 32 or ord(c) > 126 for c in texto):
        return {"tipo": "basura", "v": len(texto)}
    m = RE_D_SIMB.match(texto)
    if m:
        return {"tipo": "simbolo", "sentido": m.group(1), "v": m.group(2)}
    m = RE_D_LETRA.match(texto)          # antes que RE_D_DESC: esta exige [bin:
    if m:
        return {"tipo": "letra", "sentido": m.group(1), "letra": m.group(2), "bin": m.group(3)}
    m = RE_D_DESC.match(texto)
    if m:
        return {"tipo": "letra", "sentido": m.group(1), "letra": "?", "bin": None,
                "morse": m.group(2), "desbordado": bool(m.group(3))}
    m = RE_D_PALAB.match(texto)
    if m:
        return {"tipo": "palabra", "sentido": m.group(1)}
    m = RE_D_PULSO.match(texto)
    if m:
        return {"tipo": "pulso", "sentido": m.group(1), "ms": int(m.group(2)),
                "filtrado": bool(m.group(3))}
    m = RE_D_SIL.match(texto)
    if m:
        return {"tipo": "silencio", "sentido": m.group(1), "ms": int(m.group(2))}
    m = RE_D_FLANCO.match(texto)
    if m:
        return {"tipo": "flanco", "sentido": "TX", "t_ms": int(m.group(1)), "nivel": int(m.group(2))}
    m = RE_D_ETIQ.match(texto)           # exige la etiqueta: "# contadores a cero" no cuela
    if m:
        return dict({"tipo": m.group(1), "sentido": m.group(2)},
                    **{k: int(v) for k, v in RE_KV.findall(texto)})
    for prefijo, tipo in (("# llave ", "llave"), ("# modo ", "modo"), ("# resumen", "resumen")):
        if texto.startswith(prefijo):
            return dict({"tipo": tipo}, **{k: int(v) for k, v in RE_KV.findall(texto)})
    return {"tipo": "texto", "v": texto}


# ------------------------------------------------------------------ guarda de tasa
class GuardiaTasa:
    """Red de seguridad: los sketches imprimen como mucho unas decenas de lineas por segundo. Si de un puerto
    llegan mas (p. ej. datos viejos repetidos por el driver USB-serie al abrir un puerto ya usado), se descartan
    y se avisa; asi el visor no muestra cientos de letras o palabras falsas."""

    def __init__(self, max_por_s=40):
        self.max = max_por_s
        self.ts = collections.deque()
        self.descartadas = 0

    def admitir(self, t):
        while self.ts and t - self.ts[0] > 1.0:
            self.ts.popleft()
        if len(self.ts) >= self.max:
            self.descartadas += 1
            return False
        self.ts.append(t)
        return True

    def tomar_descartadas(self):
        n, self.descartadas = self.descartadas, 0
        return n


# ------------------------------------------------------------------ bus de eventos
class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.subs = []
        self.hist = collections.deque(maxlen=800)
        self.n = 0
        self.estado = {"modo": "vivo", "A": "sin puerto", "B": "sin puerto", "puertos": {}}

    def publicar(self, src, ev, t=None):
        ev = dict(ev)
        ev["src"] = src
        ev["t"] = t if t is not None else time.time()
        with self.lock:
            self.n += 1
            ev["n"] = self.n
            self.hist.append(ev)
            for q in self.subs:
                try:
                    q.put_nowait(ev)
                except queue.Full:
                    pass
        return ev

    def suscribir(self):
        q = queue.Queue(maxsize=5000)
        with self.lock:
            self.subs.append(q)
            return q, list(self.hist)

    def baja(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def poner_estado(self, **kw):
        with self.lock:
            self.estado.update(kw)
        self.publicar("puente", {"tipo": "estado", **{k: v for k, v in self.estado.items()}})


# ------------------------------------------------------------------ lectura de los puertos reales
def vaciar_hasta_silencio(s, silencio=0.4, maximo=8.0):
    """Al abrir un puerto ya usado llega, durante un rato, una avalancha de lineas VIEJAS repetidas (medido en el banco;
    causa no comprobada, apunta al driver USB-serie). Las placas no imprimen nada en reposo, asi que se descarta todo
    hasta que el puerto lleva `silencio` s callado (o `maximo` s). Devuelve los bytes descartados."""
    t0 = ultimo = time.time()
    total = 0
    s.reset_input_buffer()
    while time.time() - t0 < maximo:
        d = s.read(4096)
        if d:
            total += len(d)
            ultimo = time.time()
        elif time.time() - ultimo >= silencio:
            break
    s.reset_input_buffer()
    return total


def hilo_puerto(bus, src, puerto, cola_cmd, parar, ruta_log=None, ruta_marcas=None,
                parser=None):
    parser = parser or parsear
    try:
        import serial
    except ImportError:
        bus.poner_estado(**{src: "falta pyserial (pip install pyserial)"})
        return

    # Log de evidencia opcional, en el MISMO formato que captura_serie.py. Hace falta
    # porque un puerto serie solo lo abre un proceso: sin esto, ver el visor en vivo y
    # capturar la sesion son excluyentes.
    f_log = open(ruta_log, "ab", buffering=0) if ruta_log else None
    t0_log = time.time()
    marcas_leido = [0]

    def anota(prefijo, datos):
        if f_log:
            f_log.write(f"{time.time() - t0_log:9.3f} ".encode() + prefijo + datos)

    def mirar_marcas():
        # Cada hilo lleva su propio offset, asi la marca entra en los DOS logs.
        if not (f_log and ruta_marcas):
            return
        try:
            tam = os.path.getsize(ruta_marcas)
        except OSError:
            return
        if tam <= marcas_leido[0]:
            return
        with open(ruta_marcas, "rb") as m:
            m.seek(marcas_leido[0])
            datos = m.read()
        corte = datos.rfind(b"\n")
        if corte < 0:
            return
        marcas_leido[0] += corte + 1
        for linea_m in datos[:corte].split(b"\n"):
            linea_m = linea_m.strip().replace(b"\r", b"")
            if linea_m:
                anota(b"", linea_m + b"\n")

    while not parar.is_set():
        try:
            with serial.Serial(puerto, 115200, timeout=0.1) as s:
                bus.estado["puertos"][src] = puerto
                bus.poner_estado(**{src: "calentando (descartando datos viejos del puerto)"})
                t_cal = time.time()
                descartados = vaciar_hasta_silencio(s)
                bus.poner_estado(**{src: "conectado"})
                if descartados > 256:
                    bus.publicar(src, {"tipo": "aviso", "v": f"{descartados} bytes viejos descartados al abrir el puerto ({time.time() - t_cal:.1f} s)"})
                s.write(b"r\n")  # pide umbrales y contadores (solo lectura) para poblar el visor
                guardia = GuardiaTasa()
                vio_umbrales, intentos, t_r = False, 1, time.time()
                while not parar.is_set():
                    # La linea de umbrales de la respuesta a `r` a veces llega mordida por la basura de captura: se repite (solo lectura).
                    if not vio_umbrales and intentos < 3 and time.time() - t_r > 1.5:
                        s.write(b"r\n"); intentos += 1; t_r = time.time()
                    linea = s.readline()
                    mirar_marcas()
                    if linea:
                        # Al log ANTES de la guardia de tasa: la guardia protege al
                        # navegador, pero la evidencia tiene que ser completa.
                        anota(b"", linea)
                        if not guardia.admitir(time.time()):
                            continue
                        n_desc = guardia.tomar_descartadas()
                        if n_desc:
                            bus.publicar(src, {"tipo": "aviso", "v": f"{n_desc} lineas descartadas por exceso de tasa (rafaga anomala del puerto serie)"})
                        txt, basura = limpiar(linea)
                        ev = parser(src, txt, basura)
                        if ev:
                            ev["raw"] = txt.strip()
                            vio_umbrales = vio_umbrales or ev["tipo"] == "umbrales"
                            bus.publicar(src, ev)
                    while True:
                        try:
                            cmd = cola_cmd.get_nowait()
                        except queue.Empty:
                            break
                        s.write(cmd.encode("ascii") + b"\n")
                        anota(b">>> ", cmd.encode("ascii") + b"\n")
                        bus.publicar(src, {"tipo": "comando", "v": cmd})
        except (OSError, Exception) as e:  # puerto ausente, sin permiso, desenchufado...
            bus.poner_estado(**{src: f"sin conexion ({type(e).__name__}: {e})"[:120]})
            parar.wait(2.0)


# ------------------------------------------------------------------ modo demo: repite una tanda real
def cargar_demo(n):
    evid = RAIZ / "evidencia"
    def leer(nombre):
        ini, fin, out = None, None, []
        for cruda in (evid / nombre).read_bytes().split(b"\n"):
            txt = cruda.replace(b"\xff", b"").decode("latin1").replace("\r", "")
            m = re.match(r"^\s*(\d+\.\d+) ?(.*)$", txt)
            if not m:
                continue
            t, resto = float(m.group(1)), m.group(2)
            if resto.startswith("#MARK"):
                if "INICIO" in resto and ini is None:
                    ini = t
                elif ini is not None and ("FIN" in resto or "TERMINADA" in resto):
                    fin = t
                    break
                continue
            if ini is None or resto.startswith(">>>"):
                continue
            out.append((t - ini, resto))
        return out
    # Las trazas A y B se alinean por su marca INICIO (se escribieron a la vez, con <0,2 s de diferencia).
    ev = [(t, "A", l) for t, l in leer(f"tanda{n}_A.log")] + [(t, "B", l) for t, l in leer(f"tanda{n}_B.log")]
    ev.sort(key=lambda x: x[0])
    # Las esperas largas (p. ej. los ~96 s antes del primer toque de la tanda 3) se comprimen a 1,5 s: solo cambia el
    # ritmo de reproduccion; los datos de cada linea (t_ms del pin, duraciones) no se tocan.
    out, t_prev, t_acum = [], 0.0, 0.0
    for t, src, linea in ev:
        hueco = t - t_prev
        t_acum += 1.5 if hueco > 5.0 else hueco
        t_prev = t
        out.append((t_acum, src, linea))
    return out


def hilo_demo(bus, n, velocidad, bucle, parar):
    eventos = cargar_demo(n)
    bus.estado["puertos"] = {"A": f"evidencia/tanda{n}_A.log", "B": f"evidencia/tanda{n}_B.log"}
    bus.poner_estado(modo=f"demo: tanda {n} (reproduce evidencia real, x{velocidad:g})", A="demo", B="demo")
    while not parar.is_set():
        t0 = time.time()
        for t, src, linea in eventos:
            espera = t / velocidad - (time.time() - t0)
            if espera > 0 and parar.wait(espera):
                return
            ev = parsear(src, linea)
            if ev:
                ev["raw"] = linea.strip()
                bus.publicar(src, ev)
        if not bucle:
            bus.publicar("puente", {"tipo": "fin_demo"})
            return
        parar.wait(3.0)


# ----------------------------------------- reproduccion de logs de captura_serie.py
def cargar_reproduccion(rutas):
    """Lee logs en el formato de captura_serie.py y devuelve [(t_rel, src, linea)].

    Los dos logs se alinean por su `#MARK INICIO` si lo tienen (se escriben a la
    vez, con menos de 1 s de diferencia); si no, por su primera linea. Los `>>>`
    (comandos enviados) no se reproducen: no son salida de la placa.
    """
    todo = []
    for src, ruta in rutas.items():
        if not ruta:
            continue
        ini, lineas = None, []
        for cruda in Path(ruta).read_bytes().split(b"\n"):
            txt = cruda.replace(b"\xff", b"").decode("latin1").replace("\r", "")
            m = re.match(r"^\s*(\d+\.\d+) ?(.*)$", txt)
            if not m:
                continue
            t, resto = float(m.group(1)), m.group(2)
            if resto.startswith("#MARK"):
                if "INICIO" in resto and ini is None:
                    ini = t
                continue
            if resto.startswith(">>>") or not resto:
                continue
            lineas.append((t, resto))
        if not lineas:
            continue
        origen = ini if ini is not None else lineas[0][0]
        todo += [(t - origen, src, l) for t, l in lineas]
    if not todo:
        return []
    todo.sort(key=lambda x: x[0])
    desfase = todo[0][0]                       # la cabecera va antes del INICIO: no se pierde
    # Las esperas largas se comprimen a 1,5 s: cambia el ritmo de reproduccion,
    # no los datos de cada linea (t_ms del pin, duraciones medidas).
    out, t_prev, t_acum = [], 0.0, 0.0
    for t, src, linea in todo:
        hueco = (t - desfase) - t_prev
        t_acum += 1.5 if hueco > 5.0 else max(hueco, 0.0)
        t_prev = t - desfase
        out.append((t_acum, src, linea))
    return out


def hilo_reproducir(bus, rutas, velocidad, bucle, parar, parser):
    eventos = cargar_reproduccion(rutas)
    bus.estado["puertos"] = {k: str(v) for k, v in rutas.items() if v}
    bus.poner_estado(modo=f"reproduccion de evidencia (x{velocidad:g})",
                     **{k: "reproduccion" for k, v in rutas.items() if v})
    if not eventos:
        bus.publicar("puente", {"tipo": "aviso", "v": "los logs indicados no tienen lineas legibles"})
        return
    while not parar.is_set():
        t0 = time.time()
        for t, src, linea in eventos:
            espera = t / velocidad - (time.time() - t0)
            if espera > 0 and parar.wait(espera):
                return
            ev = parser(src, linea)
            if ev:
                ev["raw"] = linea.strip()
                bus.publicar(src, ev)
        if not bucle:
            bus.publicar("puente", {"tipo": "fin_demo"})
            return
        parar.wait(3.0)


# ------------------------------------------------------------------ HTTP
def fabricar_handler(bus, colas, dir_visor, puerto_http, duplex=False):
    tipos = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
             ".json": "application/json", ".svg": "image/svg+xml"}
    origenes_ok = {f"http://127.0.0.1:{puerto_http}", f"http://localhost:{puerto_http}"}

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _cors_lectura(self):
            self.send_header("Access-Control-Allow-Origin", "*")

        def do_GET(self):
            ruta = self.path.split("?")[0]
            if ruta == "/eventos":
                return self._sse()
            if ruta == "/estado":
                cuerpo = json.dumps(bus.estado).encode()
                self.send_response(200); self.send_header("Content-Type", "application/json")
                self._cors_lectura(); self.send_header("Content-Length", str(len(cuerpo))); self.end_headers()
                self.wfile.write(cuerpo); return
            nombre = "index.html" if ruta in ("/", "") else ruta.lstrip("/")
            f = (dir_visor / nombre).resolve()
            if dir_visor.resolve() not in f.parents or f.suffix not in tipos or not f.is_file():
                self.send_error(HTTPStatus.NOT_FOUND); return
            datos = f.read_bytes()
            self.send_response(200); self.send_header("Content-Type", tipos[f.suffix])
            self.send_header("Content-Length", str(len(datos))); self.end_headers(); self.wfile.write(datos)

        def _sse(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-cache")
            self._cors_lectura(); self.end_headers()
            q, hist = bus.suscribir()
            try:
                for ev in hist:
                    self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
                self.wfile.write(b"data: " + json.dumps({"src": "puente", "tipo": "estado", "t": time.time(), **bus.estado}).encode() + b"\n\n")
                self.wfile.flush()
                while True:
                    try:
                        ev = q.get(timeout=15)
                        self.wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                bus.baja(q)

        def do_POST(self):
            if self.path != "/comando":
                self.send_error(HTTPStatus.NOT_FOUND); return
            origen = self.headers.get("Origin")
            if origen is not None and origen not in origenes_ok:  # solo el visor servido por este puente
                self.send_error(HTTPStatus.FORBIDDEN, "origen no permitido"); return
            try:
                n = int(self.headers.get("Content-Length", "0"))
                datos = json.loads(self.rfile.read(min(n, 1024)))
                placa, cmd = str(datos["placa"]).upper(), str(datos["cmd"]).strip()
            except Exception:
                self.send_error(HTTPStatus.BAD_REQUEST); return
            # En duplex las DOS placas decodifican, asi que las dos aceptan p/l/w/v;
            # en clave-morse la placa A es solo la llave y no tiene umbrales.
            patron = COMANDO_DUPLEX_OK if duplex else COMANDO_OK
            if placa not in colas or not patron.match(cmd) or (
                    not duplex and placa == "A" and cmd[0] in "plwv"):
                self.send_error(HTTPStatus.BAD_REQUEST, "comando no permitido"); return
            if bus.estado["modo"] != "vivo":
                self.send_error(HTTPStatus.CONFLICT, "modo demo: no hay placas"); return
            colas[placa].put(cmd)
            cuerpo = b'{"ok":true}'
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(cuerpo))); self.end_headers(); self.wfile.write(cuerpo)
    return H


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", help="puerto de la placa A (llave), p. ej. /dev/ttyUSB0")
    ap.add_argument("--b", help="puerto de la placa B (receptor), p. ej. /dev/ttyUSB1")
    ap.add_argument("--demo", type=int, choices=[1, 2, 3, 4], help="repite la tanda N de la evidencia en vez de leer puertos")
    ap.add_argument("--velocidad", type=float, default=1.0, help="factor de velocidad de la demo (1 = tiempo real)")
    ap.add_argument("--bucle", action="store_true", help="repite la demo en bucle")
    ap.add_argument("--log-a", help="fichero de evidencia de la placa A (formato de captura_serie.py)")
    ap.add_argument("--log-b", help="fichero de evidencia de la placa B (formato de captura_serie.py)")
    ap.add_argument("--marcas", help="fichero vigilado: cada linea nueva se anota en los DOS logs (para los #MARK)")
    ap.add_argument("--port", type=int, default=8765, help="puerto HTTP en 127.0.0.1 (0 = uno libre)")
    ap.add_argument("--duplex", action="store_true",
                    help="las placas llevan morse-duplex/transceptor (sin roles, lineas RX/TX)")
    ap.add_argument("--visor", help="directorio del visor a servir (por defecto, el de la practica)")
    ap.add_argument("--reproducir-a", help="reproduce este log de captura_serie.py como si fuera la placa A")
    ap.add_argument("--reproducir-b", help="idem para la placa B")
    args = ap.parse_args(argv)
    reproducir = bool(args.reproducir_a or args.reproducir_b)
    if not args.demo and not reproducir and not (args.a or args.b):
        ap.error("indica --a y/o --b (puertos reales), --demo N, o --reproducir-a/--reproducir-b")

    parser = parsear_duplex if args.duplex else parsear
    if args.visor:
        dir_visor = Path(args.visor)
    elif args.duplex:
        dir_visor = RAIZ.parent / "morse-duplex" / "visor"
    else:
        dir_visor = RAIZ / "visor"
    if not (dir_visor / "index.html").is_file():
        ap.error(f"no encuentro el visor en {dir_visor}")

    bus, parar = Bus(), threading.Event()
    colas = {"A": queue.Queue(), "B": queue.Queue()}
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), BaseHTTPRequestHandler)
    puerto_http = srv.server_address[1]
    srv.RequestHandlerClass = fabricar_handler(bus, colas, dir_visor, puerto_http, args.duplex)
    srv.daemon_threads = True
    hilos = []
    if args.demo:
        hilos.append(threading.Thread(target=hilo_demo, args=(bus, args.demo, args.velocidad, args.bucle, parar), daemon=True))
    elif reproducir:
        rutas = {"A": args.reproducir_a, "B": args.reproducir_b}
        hilos.append(threading.Thread(target=hilo_reproducir,
                                      args=(bus, rutas, args.velocidad, args.bucle, parar, parser),
                                      daemon=True))
    else:
        rutas_log = {"A": args.log_a, "B": args.log_b}
        for src, p in (("A", args.a), ("B", args.b)):
            if p:
                hilos.append(threading.Thread(
                    target=hilo_puerto,
                    args=(bus, src, p, colas[src], parar, rutas_log[src], args.marcas, parser),
                    daemon=True))
    for h in hilos:
        h.start()
    print(f"Puente listo: http://127.0.0.1:{puerto_http}/#vivo  (Ctrl+C para salir)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        parar.set()
        srv.server_close()


if __name__ == "__main__":
    main()
