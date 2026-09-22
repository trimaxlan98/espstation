#!/usr/bin/env python3
"""Compila y ejecuta los tests de host del firmware. Linux, macOS y Windows.

Hace exactamente lo mismo que `make test` -- los mismos ficheros, las mismas
banderas, el mismo binario -- pero sin necesitar `make`, que en Windows no
viene de serie. El Makefile sigue siendo la referencia y CI lo usa; esto
existe para que un contribuyente en Windows pueda correr la misma puerta sin
instalar un entorno POSIX entero.

Requisitos: un compilador C11 con ASan/UBSan.
  - Linux/macOS:  gcc o clang, normalmente ya instalados.
  - Windows:      **no vale el clang de LLVM.LLVM**, que apunta a MSVC y pide
                  las cabeceras de Visual Studio. Hace falta LLVM-MinGW:
                      winget install MartinStorsjo.LLVM-MinGW.UCRT
                  y su `bin` en el PATH (ahi esta tambien la DLL de ASan).

Uso:
    python3 firmware/test/host/run_tests.py
    python3 firmware/test/host/run_tests.py --cc clang
    python3 firmware/test/host/run_tests.py --keep     # conserva el binario
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

AQUI = Path(__file__).resolve().parent
COMPONENTES = AQUI.parent.parent / "components"

PROTO = COMPONENTES / "esps_proto"
DIO = COMPONENTES / "esps_dio"
MORSE = COMPONENTES / "esps_morse"

# Los mismos de SRC en el Makefile. Si anades uno alli, anadelo aqui: la
# duplicacion es deliberada (un Makefile no se puede leer de forma fiable
# desde Python) y el test de coherencia de abajo la vigila.
FUENTES = [
    PROTO / "src" / "crc16.c", PROTO / "src" / "cobs.c", PROTO / "src" / "enlp.c",
    DIO / "src" / "crc8.c", DIO / "src" / "frame.c", DIO / "src" / "stats.c",
    DIO / "src" / "testgen.c", DIO / "src" / "pins.c", DIO / "src" / "policy.c",
    MORSE / "src" / "table.c", MORSE / "src" / "decode.c", MORSE / "src" / "key.c",
]
PRUEBAS = [
    "test_crc16.c", "test_cobs.c", "test_enlp.c",
    "test_dio_crc8.c", "test_dio_frame.c", "test_dio_stats.c",
    "test_dio_testgen.c", "test_dio_pins.c", "test_dio_policy.c",
    "test_morse_table.c", "test_morse_decode.c", "test_morse_key.c",
    "main.c",
]
INCLUDES = [PROTO / "include", DIO / "include", MORSE / "include"]

BANDERAS = ["-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-g"]


def elegir_compilador(preferido: str | None) -> str:
    for cc in ([preferido] if preferido else []) + ["gcc", "clang", "cc"]:
        if cc and shutil.which(cc):
            return cc
    sys.exit(
        "no encuentro un compilador C (gcc/clang). En Windows: "
        "winget install MartinStorsjo.LLVM-MinGW.UCRT y su bin en el PATH."
    )


def comprobar_coherencia_con_el_makefile() -> list[str]:
    """Avisa si el Makefile y esta lista se han separado.

    No falla la ejecucion: el Makefile es la referencia y puede llevar cosas
    que aqui no apliquen, pero un fichero que este en uno y no en el otro es
    casi siempre un olvido, y descubrirlo al mergear es tarde.
    """
    mk = AQUI / "Makefile"
    if not mk.is_file():
        return []
    texto = mk.read_text(encoding="utf-8")
    faltan = [f.name for f in FUENTES if f.name not in texto]
    faltan += [p for p in PRUEBAS if p not in texto]
    return faltan


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", help="compilador a usar (por defecto gcc, luego clang)")
    ap.add_argument("--keep", action="store_true", help="no borrar el binario")
    args = ap.parse_args()

    cc = elegir_compilador(args.cc)
    print(f"compilador: {cc}")

    faltan = comprobar_coherencia_con_el_makefile()
    if faltan:
        print(f"AVISO: en el Makefile no aparecen {faltan} — ¿se han separado?")

    for f in FUENTES:
        if not f.is_file():
            sys.exit(f"falta la fuente {f}")

    destino = Path(tempfile.mkdtemp(prefix="esps-host-"))
    binario = destino / ("esps_proto_tests.exe" if os.name == "nt" else "esps_proto_tests")
    cmd = [cc, *BANDERAS]
    for inc in INCLUDES:
        cmd += ["-I", str(inc)]
    cmd += ["-o", str(binario)]
    cmd += [str(f) for f in FUENTES]
    cmd += [str(AQUI / p) for p in PRUEBAS]

    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        return 1

    entorno = dict(os.environ)
    # esps_proto, esps_dio y esps_morse tienen prohibido asignar memoria, asi
    # que LeakSanitizer no anade cobertura y ademas no arranca bajo sandboxes
    # basados en ptrace. ASan y UBSan siguen activos, que es a lo que viene
    # esta puerta.
    entorno["ASAN_OPTIONS"] = entorno.get("ASAN_OPTIONS", "detect_leaks=0")

    r = subprocess.run([str(binario)], env=entorno, text=True)
    if not args.keep:
        shutil.rmtree(destino, ignore_errors=True)
    else:
        print(f"binario en {binario}")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
