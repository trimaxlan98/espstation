#!/usr/bin/env python3
"""Pruebas de host del transceptor Morse duplex (sin hardware).

Compila con g++ (ASan/UBSan) el .ino REAL sobre un mock minimo del core Arduino
(tests/mock/) y comprueba lo que esta practica anade sobre ../../clave-morse:
los dos sentidos conviven en la misma placa sin mezclarse, con umbrales y
contadores independientes, y el antirrebote de la llave no altera las duraciones.
Ademas valida la tabla A-Z/0-9 contra un diccionario Morse independiente.

Prueba la LOGICA, no la temporizacion real de las placas (latencia de la ISR,
rebote fisico, pulso humano) ni nada del enlace entre dos ordenadores: eso se
mide en el banco (README.md, "Resultados").

Forma parte de `make check` (objetivo `bench-test`).
Uso suelto: python3 bench/practicas/morse-duplex/tests/run_tests.py
"""
import subprocess, sys, tempfile
from pathlib import Path

AQUI = Path(__file__).resolve().parent
RAIZ = AQUI.parent
# Morse internacional, escrito aparte del sketch para no compartir un error.
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
        exe = Path(d) / "host_duplex"
        compilar("host_duplex.cpp", RAIZ / "transceptor" / "transceptor.ino",
                 "TRANSCEPTOR_INO", exe)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(f"=== transceptor\n{r.stdout}{r.stderr}")
        fallos += r.returncode != 0
        r = subprocess.run([str(exe), "tabla"], input="\n".join(MORSE.values()) + "\n",
                           capture_output=True, text=True)
        salida = dict(l.split("|", 1) for l in r.stdout.strip().splitlines())
        mal = [c for c, cod in MORSE.items()
               if salida.get(cod) != f"[letra: {c}] [bin: {ord(c):08b}]"]
        print(f"=== tabla: {len(MORSE)} entradas (A-Z, 0-9) contra diccionario "
              f"independiente, {len(mal)} fallos {mal}")
        fallos += bool(mal)
    print("RESULTADO:", "FALLA" if fallos else "TODO OK")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
