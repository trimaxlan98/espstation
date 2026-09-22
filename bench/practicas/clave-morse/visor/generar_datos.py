#!/usr/bin/env python3
"""Convierte los logs reales de `evidencia/` en `datos.js` / `datos.json` para el visor.

Solo lee los logs de la placa B (con verbose activo: `# silencio_ms`, `# pulso_ms`, los
simbolos `.`/`-`, las letras y `[palabra]`) y comprueba que el numero de pulsos leidos
coincide con los contadores que la propia placa imprimio (`puntos` + `rayas`). Si no
coincide, se detiene: no se inventa ni se rellena ningun dato.

Los bloques de 64 bytes 0xFF que la captura serie mete en algunas lineas se descartan;
si lo que queda de la linea sigue siendo un evento valido, se conserva.

Uso:  python3 bench/practicas/clave-morse/visor/generar_datos.py
"""
import json
import re
import sys
from pathlib import Path

AQUI = Path(__file__).resolve().parent
EVID = AQUI.parent / "evidencia"
SVG = AQUI.parent / "docs" / "circuito.svg"

# Metadatos de cada tanda: SOLO lo que se registro durante la sesion (README, `## Resultados`).
# `contadores` son los que imprimio la placa con `r` (o el resumen periodico donde el `r`
# salio corrupto); `toques` es lo que declaro el operador (None = no lo sabe / no lo dio).
TANDAS = [
    dict(id=1, archivo="tanda1_B.log", ini="INICIO SOS x3", fin="TANDA 1 TERMINADA",
         umbrales=dict(p=300, l=700, w=1800, d=15), d_A=15, toques=None,
         toques_nota="el operador no sabe cuantos toques dio",
         contadores=dict(A_crudos=150, A_aceptados=62, B_flancos_crudos=62, puntos=25, rayas=6,
                         letras=5, desconocidas=2, niveles_repetidos=0),
         sos_correctos="0 de 3",
         nota="Umbrales de arranque. Seis de las doce rayas midieron 210-276 ms y con p=300 se leyeron como puntos; "
              "tres pausas entre letras (396-586 ms) fueron menores que l=700 y las letras se fundieron."),
    dict(id=2, archivo="tanda2_B.log", ini="INICIO TANDA 2", fin="FIN TANDA 2",
         umbrales=dict(p=140, l=700, w=1800, d=15), d_A=15, toques=27, toques_nota=None,
         contadores=dict(A_crudos=298, A_aceptados=56, B_flancos_crudos=56, puntos=19, rayas=9,
                         letras=14, desconocidas=0, niveles_repetidos=0),
         sos_correctos="0 de 3",
         nota="p=140 corrigio el punto/raya, pero las pausas entre rayas (802-852 ms) superaron l=700 y la O se partio en M+T. "
              "Un toque se partio en dos pulsos (hueco de 19 ms > antirrebote de 15 ms): 27 toques dieron 28 pulsos y una S salio H."),
    dict(id=3, archivo="tanda3_B.log", ini="INICIO TANDA 3", fin="FIN TANDA 3",
         umbrales=dict(p=170, l=930, w=3000, d=15), d_A=25, toques=27, toques_nota=None,
         contadores=dict(A_crudos=134, A_aceptados=54, B_flancos_crudos=55, puntos=18, rayas=9,
                         letras=10, desconocidas=0, niveles_repetidos=1),
         sos_correctos="2 de 3 (letras); 1 de 3 con [palabra]",
         nota="Umbrales calibrados con las tandas 1-2 (evaluacion). SOS #2 sin [palabra]: hueco de 2467 ms < w=3000. "
              "SOS #3: pausa de 1037 ms entre rayas > l=930, la O salio T+M. 27 toques dieron 27 pulsos."),
    dict(id=4, archivo="tanda4_B.log", ini="INICIO TANDA 4", fin="FIN TANDA 4",
         umbrales=dict(p=170, l=930, w=3000, d=15), d_A=25, toques=None,
         toques_nota="el operador admite 'un par de errores' y no dio el conteo",
         contadores=dict(A_crudos=90, A_aceptados=48, B_flancos_crudos=48, puntos=11, rayas=13,
                         letras=9, desconocidas=0, niveles_repetidos=0),
         sos_correctos="0 de 3",
         nota="Umbrales congelados (repeticion de la evaluacion). No se reproduce el 2 de 3 de la tanda 3: "
              "una O partida por una pausa entre rayas > l, un SOS al que le falta la primera S y uno que salio O O U."),
]

RE_TS = re.compile(r"^\s*\d+\.\d+ ?")
RE_SIL = re.compile(r"^# silencio_ms=(\d+)$")
RE_PUL = re.compile(r"^# pulso_ms=(\d+)( filtrado)?$")
RE_LET = re.compile(r"^\[letra: (.)\] \[(bin: [01]{8}|morse: [.\-]+\+?)\]$")


def limpiar(linea: bytes):
    """Quita la marca de tiempo del host y los bytes 0xFF de basura. Devuelve (texto, tenia_basura)."""
    basura = b"\xff" in linea
    linea = linea.replace(b"\xff", b"")
    txt = linea.decode("latin1").replace("\r", "")
    txt = RE_TS.sub("", txt, count=1).strip()
    return txt, basura


def leer_tanda(t):
    datos = (EVID / t["archivo"]).read_bytes().split(b"\n")
    dentro = False
    salida, pulsos = [], []
    sil = None
    basura_n = perdidas = 0
    for cruda in datos:
        txt, basura = limpiar(cruda)
        if t["ini"] in txt and txt.startswith("#MARK"):
            dentro = True
            continue
        if not dentro:
            continue
        if t["fin"] in txt and txt.startswith("#MARK"):
            break
        if not txt or txt.startswith("#MARK") or txt.startswith(">>>"):
            continue
        if basura:
            basura_n += 1
        m = RE_SIL.match(txt)
        if m:
            sil = int(m.group(1))
            continue
        m = RE_PUL.match(txt)
        if m:
            if m.group(2):
                sys.exit(f"tanda {t['id']}: hay un pulso filtrado; el visor no lo modela con datos reales")
            pulsos.append(dict(sil=sil, dur=int(m.group(1))))
            sil = None
            continue
        if txt in (".", "-"):
            salida.append(dict(k="sim", v=txt))
            continue
        if txt == "[palabra]":
            salida.append(dict(k="pal", v=txt))
            continue
        m = RE_LET.match(txt)
        if m:
            salida.append(dict(k="let", v=txt))
            continue
        if txt.startswith("# umbrales") or txt.startswith("# contadores"):
            continue
        perdidas += 1  # linea ilegible o no reconocida: se cuenta, no se descarta en silencio
    return pulsos, salida, basura_n, perdidas


def main():
    salida = []
    for t in TANDAS:
        pulsos, board, basura_n, perdidas = leer_tanda(t)
        c = t["contadores"]
        esperado = c["puntos"] + c["rayas"]
        if len(pulsos) != esperado:
            sys.exit(f"tanda {t['id']}: {len(pulsos)} pulsos leidos del log frente a {esperado} en los "
                     f"contadores de la placa: el log perdio datos; no se genera el visor")
        # el primer silencio es el que hubo ANTES de empezar a teclear: no es parte de la tanda
        primer = pulsos[0]["sil"]
        pulsos[0]["sil"] = None
        salida.append(dict(id=t["id"], umbrales=t["umbrales"], d_A=t["d_A"], toques=t["toques"],
                           toques_nota=t["toques_nota"], contadores=c, sos_correctos=t["sos_correctos"],
                           nota=t["nota"], pulsos=pulsos, placa=board,
                           lineas_con_basura=basura_n, lineas_no_reconocidas=perdidas,
                           silencio_previo_ms=primer, archivo=t["archivo"]))
        print(f"tanda {t['id']}: {len(pulsos)} pulsos (= puntos+rayas de la placa), "
              f"{len(board)} eventos de salida de la placa, {basura_n} lineas con basura, "
              f"{perdidas} no reconocidas")

    js = "// GENERADO por generar_datos.py a partir de evidencia/*.log. No editar a mano.\n"
    js += "const TANDAS = " + json.dumps(salida, ensure_ascii=False, indent=1) + ";\n"
    js += "const CIRCUITO_SVG = " + json.dumps(SVG.read_text(encoding="utf-8") if SVG.exists() else "") + ";\n"
    (AQUI / "datos.js").write_text(js, encoding="utf-8")
    (AQUI / "datos.json").write_text(json.dumps(salida, ensure_ascii=False, indent=1), encoding="utf-8")
    print("escritos datos.js y datos.json")


if __name__ == "__main__":
    main()
