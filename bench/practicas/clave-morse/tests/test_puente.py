#!/usr/bin/env python3
"""Pruebas del puente serie -> navegador (herramientas/puente_serie.py), sin hardware.

1. Interprete de lineas: con lineas REALES de la evidencia (incluida basura 0xFF).
2. Modo demo de punta a punta: arranca el puente, se suscribe a /eventos (SSE) y comprueba que lo
   que llega por SSE de la placa B es EXACTAMENTE lo que imprimio la placa real (datos.json), y que
   los flancos de A coinciden con los contadores de la placa.
3. Comandos: la lista blanca y el rechazo en modo demo.
4. Canal de comandos de herramientas/captura_serie.py, las DOS ramas. La rama
   POSIX (CanalFifo) solo se ejercita entera donde hay os.mkfifo; en Windows se
   comprueba al menos que rechaza un fichero que no es una FIFO.

Uso: python3 bench/practicas/clave-morse/tests/test_puente.py
"""
import http.client, json, os, re, subprocess, sys, tempfile, time, urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RAIZ / "herramientas"))
import puente_serie as P  # noqa: E402
import captura_serie as C  # noqa: E402

fallos = pruebas = 0
def check(nombre, ok, extra=""):
    global fallos, pruebas
    pruebas += 1
    if not ok:
        fallos += 1
    print(("ok:    " if ok else "FALLO: ") + nombre + (f"  [{extra}]" if extra and not ok else ""))

# ---- 1. interprete de lineas (formato real de los sketches)
check("A: flanco aceptado", P.parsear("A", "1258145,1") == {"tipo": "flanco", "t_ms": 1258145, "nivel": 1})
check("A: resumen", P.parsear("A", "# resumen t_ms=1260010 crudos=90 aceptados=48 nivel=0") == {"tipo": "resumen", "t_ms": 1260010, "crudos": 90, "aceptados": 48, "nivel": 0})
check("B: simbolos", P.parsear("B", ".") == {"tipo": "simbolo", "v": "."} and P.parsear("B", "-")["v"] == "-")
check("B: letra + byte", P.parsear("B", "[letra: S] [bin: 01010011]") == {"tipo": "letra", "letra": "S", "bin": "01010011"})
check("B: letra desconocida", P.parsear("B", "[letra: ?] [morse: ...---..+]") == {"tipo": "letra", "letra": "?", "bin": None, "morse": "...---..", "desbordado": True})
check("B: palabra", P.parsear("B", "[palabra]") == {"tipo": "palabra"})
check("B: pulso y silencio", P.parsear("B", "# pulso_ms=93")["ms"] == 93 and P.parsear("B", "# silencio_ms=367")["ms"] == 367)
u = P.parsear("B", "# umbrales punto_raya_ms=170 letra_ms=930 palabra_ms=3000 debounce_ms=15 verbose=1")
check("B: umbrales", u == {"tipo": "umbrales", "punto_raya_ms": 170, "letra_ms": 930, "palabra_ms": 3000, "debounce_ms": 15, "verbose": 1})
t, b = P.limpiar(b"\xff" * 64 + b"1258145,1\r\n")
check("basura 0xFF al inicio: se descarta y se conserva el dato", b and P.parsear("A", t, b) == {"tipo": "flanco", "t_ms": 1258145, "nivel": 1})
t, b = P.limpiar(b"\xff" * 64 + b"ra_ms=700 palabra_ms=1800\r\n")
check("linea pisada por basura: no se inventa nada (queda como texto)", P.parsear("B", t, b)["tipo"] == "texto")
check("bytes de control -> basura", P.parsear("A", "\x00\x00\x80")["tipo"] == "basura")

# ---- 2. demo de punta a punta
tandas = json.loads((RAIZ / "visor" / "datos.json").read_text(encoding="utf-8"))
def demo(n):
    pr = subprocess.Popen([sys.executable, str(RAIZ / "herramientas" / "puente_serie.py"), "--demo", str(n), "--velocidad", "60", "--port", "0"],
                          stdout=subprocess.PIPE, text=True)
    try:
        m = re.search(r"127\.0\.0\.1:(\d+)", pr.stdout.readline())
        port = int(m.group(1))
        evs, t0 = [], time.time()
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/eventos", timeout=30) as r:
            while time.time() - t0 < 40:
                l = r.readline().decode()
                if l.startswith("data: "):
                    e = json.loads(l[6:]); evs.append(e)
                    if e.get("tipo") == "fin_demo":
                        break
        # comando en modo demo: debe rechazarse (409)
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        c.request("POST", "/comando", body=json.dumps({"placa": "B", "cmd": "r"}), headers={"Content-Type": "application/json"})
        rc = c.getresponse().status
        c2 = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        c2.request("POST", "/comando", body=json.dumps({"placa": "B", "cmd": "r"}), headers={"Origin": "http://evil.example"})
        rc2 = c2.getresponse().status
        return evs, rc, rc2
    finally:
        pr.terminate(); pr.wait(5)

def texto_b(e):
    if e["tipo"] == "simbolo": return e["v"]
    if e["tipo"] == "palabra": return "[palabra]"
    if e["tipo"] == "letra":
        return f"[letra: {e['letra']}] [bin: {e['bin']}]" if e["letra"] != "?" else f"[letra: ?] [morse: {e['morse']}{'+' if e.get('desbordado') else ''}]"

for n in (3, 4):
    evs, rc, rc2 = demo(n)
    t = tandas[n - 1]
    b = [texto_b(e) for e in evs if e.get("src") == "B" and texto_b(e) is not None]
    esperado = [x["v"] for x in t["placa"]]
    check(f"demo tanda {n}: la salida de B por SSE == lo que imprimio la placa real ({len(esperado)} eventos)", b == esperado, f"{len(b)} recibidos")
    fl = [e for e in evs if e.get("src") == "A" and e["tipo"] == "flanco"]
    check(f"demo tanda {n}: flancos de A por SSE == contador de la placa ({t['contadores']['A_aceptados']})", len(fl) == t["contadores"]["A_aceptados"], f"{len(fl)} recibidos")
    check(f"demo tanda {n}: el estado anuncia modo demo", any(e.get("tipo") == "estado" and str(e.get("modo", "")).startswith("demo") for e in evs))
    check(f"demo tanda {n}: POST /comando rechazado en demo (409) y con origen ajeno (403)", rc == 409 and rc2 == 403, f"{rc},{rc2}")

# ---- 2b. guarda de tasa (red de seguridad frente a rafagas anomalas del puerto)
g = P.GuardiaTasa(max_por_s=40)
ok_lento = all(g.admitir(i * 0.1) for i in range(100))           # 10 lineas/s: todo pasa
check("guarda de tasa: 10 lineas/s pasan todas", ok_lento and g.tomar_descartadas() == 0)
g = P.GuardiaTasa(max_por_s=40)
adm = sum(g.admitir(0.5 + i * 0.0001) for i in range(5000))       # 5000 lineas en 0,5 s: rafaga
check("guarda de tasa: una rafaga de 5000 lineas en 0,5 s se recorta a <= 40", adm <= 40 and g.tomar_descartadas() == 5000 - adm)
g = P.GuardiaTasa(max_por_s=40)
for i in range(200): g.admitir(0.0 + i * 0.001)
check("guarda de tasa: tras la rafaga vuelve a admitir cuando la tasa es normal", g.admitir(5.0) is True)

# ---- 3. lista blanca de comandos
ok = ["r", "c", "v", "p170", "l930", "w3000", "d25"]; mal = ["reset", "p", "p-1", "p1000000", "x", "r;ls", "d 25", "../etc"]
check("lista blanca acepta los comandos del SPEC", all(P.COMANDO_OK.match(x) for x in ok))
check("lista blanca rechaza el resto", not any(P.COMANDO_OK.match(x) for x in mal))

# ---- 4. canal de comandos de captura_serie.py
# Lo que entra por aqui se ESCRIBE EN LA PLACA: un canal que reenvia lo que no
# debe cambia umbrales sin que nadie lo pida y contamina la tanda entera.
tmp = Path(tempfile.mkdtemp())

# 4a. CanalFichero (rama Windows)
ruta = tmp / "cmd.txt"
ruta.write_bytes(b"p170\nd0\n")                       # sobras de otra sesion
cf = C.CanalFichero(str(ruta))
check("CanalFichero trunca al arrancar: no reenvia la sesion anterior", cf.leer() == [])
ruta.write_bytes(b"r\n#MARK INICIO\r\np170")          # la ultima linea, a medias
check("CanalFichero entrega solo lineas completas y quita el CR",
      cf.leer() == [b"r", b"#MARK INICIO"])
check("CanalFichero espera a un escritor a medias", cf.leer() == [])
with open(ruta, "ab") as fh:
    fh.write(b"\n")
check("CanalFichero entrega la linea en cuanto se cierra", cf.leer() == [b"p170"])
ruta.write_bytes(b"c\n")                              # alguien lo vacio
check("CanalFichero detecta el truncado y reempieza", cf.leer() == [b"c"])

# 4b. Guarda de la rama POSIX: un fichero normal NO es un canal. Sin ella
#     os.open() lo abre igual y la primera lectura le manda a la placa todo lo
#     que hubiera dentro. Esta comprobacion corre tambien en Windows.
suelto = tmp / "no_es_fifo"
suelto.write_bytes(b"p170\n")
try:
    C.CanalFifo(str(suelto))
    aviso = None
except SystemExit as e:
    aviso = str(e)
check("CanalFifo rechaza un fichero normal en vez de reenviar su contenido",
      aviso is not None and "FIFO" in aviso, repr(aviso))

# 4c. La FIFO de verdad: solo donde existe os.mkfifo.
if hasattr(os, "mkfifo"):
    fifo = tmp / "cmd_fifo"
    cfi = C.CanalFifo(str(fifo))
    check("CanalFifo sin datos no bloquea ni devuelve nada", cfi.leer() == [])
    fd = os.open(str(fifo), os.O_WRONLY | os.O_NONBLOCK)
    os.write(fd, b"r\np1")
    os.close(fd)
    check("CanalFifo entrega la linea completa y se guarda el resto", cfi.leer() == [b"r"])
    fd = os.open(str(fifo), os.O_WRONLY | os.O_NONBLOCK)
    os.write(fd, b"70\n")
    os.close(fd)
    check("CanalFifo recompone una linea partida entre dos lecturas", cfi.leer() == [b"p170"])
    check("CanalFifo sigue vivo tras cerrarse todos los escritores", cfi.leer() == [])
    cfi.buf = b"x" * (C.MAX_BUF_CANAL + 1)
    cfi.leer()
    check("CanalFifo no acumula sin limite si nadie cierra la linea", cfi.buf == b"")
else:
    print("nota:  CanalFifo (rama POSIX) no se puede ejercitar aqui: no hay os.mkfifo")

# 4d. La LOGICA de CanalFifo.leer() (troceo, recomposicion entre dos lecturas,
#     EAGAIN y tope de buffer) se puede ejercitar en CUALQUIER sistema con un
#     os.read de mentira. Lo que no se puede probar fuera de POSIX es la FIFO
#     de verdad; esto cubre al menos el codigo que se refactorizo a ciegas.
canal = object.__new__(C.CanalFifo)          # sin __init__: no hay FIFO que abrir
canal.fd, canal.buf = -1, b""
guion = [b"r\np1", BlockingIOError(), b"70\n#MARK X\n"]


def falso_read(fd, n):
    if not guion:
        raise BlockingIOError()
    d = guion.pop(0)
    if isinstance(d, Exception):
        raise d
    return d


_read_real, C.os.read = C.os.read, falso_read
try:
    check("CanalFifo.leer: entrega la linea completa y guarda el trozo suelto",
          canal.leer() == [b"r"] and canal.buf == b"p1")
    check("CanalFifo.leer: sin datos (EAGAIN) no devuelve nada ni pierde el trozo",
          canal.leer() == [] and canal.buf == b"p1")
    check("CanalFifo.leer: recompone la linea partida y entrega las dos",
          canal.leer() == [b"p170", b"#MARK X"])
    canal.buf = b"x" * (C.MAX_BUF_CANAL + 1)
    canal.leer()
    check("CanalFifo.leer: no acumula sin limite si nadie cierra la linea", canal.buf == b"")
finally:
    C.os.read = _read_real
print(f"\n{pruebas} pruebas, {fallos} fallos")
sys.exit(1 if fallos else 0)
