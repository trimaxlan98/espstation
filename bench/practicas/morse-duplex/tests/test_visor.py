#!/usr/bin/env python3
"""Pruebas del puente en modo duplex y del visor de esta practica, sin hardware.

1. Interprete `parsear_duplex`: con lineas REALES de evidencia/ (incluida basura).
2. Lista blanca de comandos del duplex (k, e y los del eco con 't' delante).
3. De punta a punta: arranca el puente con `--duplex --reproducir-a/-b` sobre la
   evidencia de la tanda congelada, se suscribe a /eventos (SSE) y comprueba que
   las letras que llegan por SSE son EXACTAMENTE las que imprimieron las placas.
4. Que el visor de esta practica se sirve y que /comando se rechaza en reproduccion.

Uso: python3 bench/practicas/morse-duplex/tests/test_visor.py
"""
import http.client
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent          # morse-duplex/
CLAVE = RAIZ.parent / "clave-morse"                    # de ahi sale el puente
sys.path.insert(0, str(CLAVE / "herramientas"))
import puente_serie as P  # noqa: E402

PUENTE = CLAVE / "herramientas" / "puente_serie.py"
LOG_A = RAIZ / "evidencia" / "tanda_congelada_A.log"
LOG_B = RAIZ / "evidencia" / "tanda_congelada_B.log"

fallos = pruebas = 0


def check(nombre, ok, extra=""):
    global fallos, pruebas
    pruebas += 1
    if not ok:
        fallos += 1
    print(("ok:    " if ok else "FALLO: ") + nombre + (f"  [{extra}]" if extra and not ok else ""))


# ---- 1. interprete de lineas del transceptor -------------------------------
d = P.parsear_duplex
check("simbolo con sentido", d("A", "RX .") == {"tipo": "simbolo", "sentido": "RX", "v": "."}
      and d("A", "TX -")["sentido"] == "TX")
check("letra + byte", d("B", "RX [letra: S] [bin: 01010011]")
      == {"tipo": "letra", "sentido": "RX", "letra": "S", "bin": "01010011"})
check("letra desconocida y desbordada", d("A", "TX [letra: ?] [morse: ...---..+]")
      == {"tipo": "letra", "sentido": "TX", "letra": "?", "bin": None,
          "morse": "...---..", "desbordado": True})
check("palabra con sentido", d("A", "RX [palabra]") == {"tipo": "palabra", "sentido": "RX"})
check("pulso", d("A", "# RX pulso_ms=180") == {"tipo": "pulso", "sentido": "RX", "ms": 180, "filtrado": False})
check("pulso filtrado", d("A", "# TX pulso_ms=14 filtrado")["filtrado"] is True)
check("silencio", d("B", "# RX silencio_ms=872") == {"tipo": "silencio", "sentido": "RX", "ms": 872})
check("flanco de la llave", d("A", "# TX flanco t_ms=50511 nivel=1")
      == {"tipo": "flanco", "sentido": "TX", "t_ms": 50511, "nivel": 1})
u = d("A", "# umbrales RX punto_raya_ms=300 letra_ms=1200 palabra_ms=1800 debounce_ms=15")
check("umbrales por sentido", u["tipo"] == "umbrales" and u["sentido"] == "RX"
      and u["letra_ms"] == 1200 and u["punto_raya_ms"] == 300)
c = d("A", "# contadores RX puntos=8 rayas=7 letras=0 desconocidas=1 filtrados=0 "
           "niveles_repetidos=0 flancos_crudos=30 desbordes_buffer=0")
check("contadores por sentido", c["sentido"] == "RX" and c["puntos"] == 8
      and c["desbordes_buffer"] == 0)
check("contadores sin etiqueta NO se confunden con los de un sentido",
      d("A", "# contadores a cero")["tipo"] == "texto")
check("llave", d("A", "# llave debounce_ms=15 crudos=264 aceptados=26")
      == {"tipo": "llave", "debounce_ms": 15, "crudos": 264, "aceptados": 26})
check("modo", d("A", "# modo eco_local=1 verbose=1")
      == {"tipo": "modo", "eco_local": 1, "verbose": 1})
check("resumen", d("A", "# resumen t_ms=55044 llave_crudos=40 llave_aceptados=12 "
                        "rx_flancos_crudos=0 nivel_tx=0 nivel_rx=0")["llave_aceptados"] == 12)
t, b = P.limpiar(b"\xff" * 64 + b"RX [letra: O] [bin: 01001111]\r\n")
check("basura 0xFF al inicio: se descarta y se conserva el dato",
      b and d("B", t, b) == {"tipo": "letra", "sentido": "RX", "letra": "O", "bin": "01001111"})
t, b = P.limpiar(b"\xff" * 64 + b"tra_ms=700 palabra_ms=1800\r\n")
check("linea pisada por basura: no se inventa nada", d("B", t, b)["tipo"] == "texto")
check("el interprete de clave-morse NO cambia",
      P.parsear("A", "1258145,1") == {"tipo": "flanco", "t_ms": 1258145, "nivel": 1}
      and P.parsear("B", "[palabra]") == {"tipo": "palabra"})

# ---- 2. lista blanca del duplex -------------------------------------------
ok = ["r", "c", "v", "e", "k40", "p300", "l600", "w1800", "d40",
      "tp300", "tl600", "tw1800", "td40"]
mal = ["reset", "p", "t", "tt300", "p-1", "p1000000", "x", "r;ls", "d 25", "../etc", "k"]
check("acepta los comandos del transceptor", all(P.COMANDO_DUPLEX_OK.match(x) for x in ok),
      str([x for x in ok if not P.COMANDO_DUPLEX_OK.match(x)]))
check("rechaza el resto", not any(P.COMANDO_DUPLEX_OK.match(x) for x in mal),
      str([x for x in mal if P.COMANDO_DUPLEX_OK.match(x)]))

# ---- 3 y 4. de punta a punta sobre la evidencia real ----------------------
def esperado_del_log(ruta, sentido):
    """Las letras que imprimio la placa, en orden, saltandose la cabecera previa
    al #MARK INICIO (que es lo que hace la reproduccion)."""
    out, visto_inicio = [], False
    for linea in ruta.read_text(encoding="utf-8", errors="replace").splitlines():
        t = linea[10:].strip() if len(linea) > 10 else linea.strip()
        if t.startswith("#MARK") and "INICIO" in t:
            visto_inicio = True
            continue
        if not visto_inicio or t.startswith(">>>"):
            continue
        m = re.match(r"^" + sentido + r" \[letra: (.)\]", t)
        if m:
            out.append(m.group(1))
    return out


def reproducir():
    pr = subprocess.Popen(
        [sys.executable, str(PUENTE), "--duplex", "--velocidad", "40", "--port", "0",
         "--reproducir-a", str(LOG_A), "--reproducir-b", str(LOG_B)],
        stdout=subprocess.PIPE, text=True)
    try:
        m = re.search(r"127\.0\.0\.1:(\d+)", pr.stdout.readline())
        port = int(m.group(1))
        evs, t0 = [], time.time()
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/eventos", timeout=60) as r:
            while time.time() - t0 < 70:
                l = r.readline().decode()
                if l.startswith("data: "):
                    e = json.loads(l[6:])
                    evs.append(e)
                    if e.get("tipo") == "fin_demo":
                        break
        # el visor de ESTA practica se sirve en /
        pag = urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=5).read().decode()
        # en reproduccion no hay placas: /comando debe dar 409
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        c.request("POST", "/comando", body=json.dumps({"placa": "A", "cmd": "p300"}),
                  headers={"Content-Type": "application/json"})
        rc = c.getresponse().status
        return evs, pag, rc
    finally:
        pr.terminate()
        pr.wait(5)


if not (LOG_A.is_file() and LOG_B.is_file()):
    check("hay evidencia de la tanda congelada para reproducir", False,
          f"faltan {LOG_A.name} / {LOG_B.name}")
else:
    evs, pag, rc = reproducir()
    check("el visor servido es el del duplex", "Morse dúplex" in pag and "selFlujo" in pag)
    check("POST /comando rechazado en reproduccion (409)", rc == 409, str(rc))
    for src, ruta in (("A", LOG_A), ("B", LOG_B)):
        for sentido in ("RX", "TX"):
            esp = esperado_del_log(ruta, sentido)
            got = [e["letra"] for e in evs
                   if e.get("src") == src and e.get("tipo") == "letra"
                   and e.get("sentido") == sentido]
            check(f"SSE {src}·{sentido}: las letras == lo que imprimio la placa ({len(esp)})",
                  got == esp, f"esperado {esp} recibido {got}")
    ns = sum(1 for e in evs if e.get("tipo") == "silencio")
    np_ = sum(1 for e in evs if e.get("tipo") == "pulso")
    check("llegan pulsos y silencios con sentido (los que dibuja la cadencia)",
          ns > 10 and np_ > 10, f"{np_} pulsos, {ns} silencios")
    check("llegan umbrales de los DOS sentidos de las DOS placas",
          len({(e["src"], e["sentido"]) for e in evs
               if e.get("tipo") == "umbrales" and e.get("sentido")}) == 4)

print(f"\n{pruebas} pruebas, {fallos} fallos")
sys.exit(1 if fallos else 0)
