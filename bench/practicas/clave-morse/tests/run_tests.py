#!/usr/bin/env python3
"""Pruebas de host de la clave Morse (sin hardware).

Compila con g++ (ASan/UBSan) los .ino REALES sobre un mock minimo del core
Arduino (tests/mock/) y comprueba: decodificacion de SOS, tabla A-Z/0-9 contra un
diccionario Morse independiente, umbrales, filtro de ruido, buffer de flancos y
antirrebote del transmisor.

Prueba la LOGICA, no la temporizacion real de las placas (latencia de la ISR,
rebote fisico, pulso humano): eso se mide en el banco (README, "Resultados").
Forma parte de `make check` (objetivo `bench-test`).
Uso suelto: python3 bench/practicas/clave-morse/tests/run_tests.py
"""
import subprocess, sys, tempfile
from pathlib import Path

AQUI = Path(__file__).resolve().parent
RAIZ = AQUI.parent
# Morse internacional, escrito aparte de los sketches para no compartir un error.
MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.',
         'H': '....', 'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.',
         'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-', 'U': '..-',
         'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..',
         '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....',
         '6': '-....', '7': '--...', '8': '---..', '9': '----.'}


def compilar(fuente, ino, macro, salida):
    cmd = ["g++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=address,undefined",
           "-I", str(AQUI / "mock"), f'-D{macro}="{ino}"', str(AQUI / fuente), "-o", str(salida)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(f"no compila {fuente}")


def main():
    fallos = 0
    with tempfile.TemporaryDirectory() as d:
        rx, tx = Path(d) / "host_rx", Path(d) / "host_tx"
        compilar("host_rx.cpp", RAIZ / "receptor" / "receptor.ino", "RECEPTOR_INO", rx)
        compilar("host_tx.cpp", RAIZ / "transmisor" / "transmisor.ino", "TRANSMISOR_INO", tx)
        for nombre, exe in (("receptor", rx), ("transmisor", tx)):
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            print(f"=== {nombre}\n{r.stdout}{r.stderr}")
            fallos += r.returncode != 0
        r = subprocess.run([str(rx), "tabla"], input="\n".join(MORSE.values()) + "\n",
                           capture_output=True, text=True)
        salida = dict(l.split("|", 1) for l in r.stdout.strip().splitlines())
        mal = [c for c, cod in MORSE.items()
               if salida.get(cod) != f"[letra: {c}] [bin: {ord(c):08b}]"]
        print(f"=== tabla: {len(MORSE)} entradas (A-Z, 0-9) contra diccionario independiente, {len(mal)} fallos {mal}")
        fallos += bool(mal)
    print("RESULTADO:", "FALLA" if fallos else "TODO OK")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
