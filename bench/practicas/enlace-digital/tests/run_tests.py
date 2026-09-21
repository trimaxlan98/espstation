#!/usr/bin/env python3
"""Test de deriva del sketch N3 (bench/practicas/enlace-digital/n3_bytes).

Por que existe: el enlace de dos hilos vive en TRES implementaciones
independientes (este sketch Arduino, `esps_dio` en C y `dio_link.py` en el
simulador del gateway) y el unico seguro contra que se separen es que las tres
pasen los mismos vectores y den las mismas cifras. Las otras dos tienen tests
en el repo; este es el del sketch.

Como funciona: compila con g++ `host_n3.cpp`, que hace `#include` del `.ino`
REAL (nunca una copia) sobre un mock minimo del core Arduino, y contrasta lo
que el sketch calcula con (a) SPEC-LINK.md, leido del propio fichero, y (b)
`LinkMonitor` de `gateway/espstation_gateway/transports/sim/dio_link.py`.

Sin hardware, salida determinista. Lo que NO prueba: latencia de ISR, jitter,
setup/hold reales del cable. Eso sigue siendo del banco.

Uso (raiz del repo):
    gateway/.venv/bin/python bench/practicas/enlace-digital/tests/run_tests.py
    ... --ino RUTA          usar otro .ino (lo usa --mutation-check)
    ... --mutation-check    demostrar que el test detecta deriva (mutaciones)
    ... --keep DIR          conservar el ejecutable y los flujos en DIR
"""
from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
PRACTICA = HERE.parent
REPO = PRACTICA.parents[2]
DEFAULT_INO = PRACTICA / "n3_bytes" / "n3_bytes.ino"
SPEC = PRACTICA / "SPEC-LINK.md"

sys.path.insert(0, str(REPO / "gateway"))
from espstation_gateway.transports.sim import dio_link as dl  # noqa: E402

SPEED_LIST = [1000, 5000, 10000, 50000, 100000, 200000]  # la lista de la practica (README)
TOTAL_FRAMES = 1000


# -- mini framework -------------------------------------------------------------

@dataclass
class Suite:
    quiet: bool = False
    failures: list[str] = field(default_factory=list)
    checks: int = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks += 1
        if not ok:
            self.failures.append(name)
        if not self.quiet:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not ok else ""))
        return ok

    def section(self, title: str) -> None:
        if not self.quiet:
            print(title)

    def info(self, text: str) -> None:
        if not self.quiet:
            print(f"        {text}")


class Harness:
    def __init__(self, exe: Path) -> None:
        self.exe = exe

    def run(self, *args: str, timeout: int = 300) -> dict[str, str]:
        """Ejecuta una orden y devuelve las parejas clave=valor de stdout."""
        r = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError(f"host_n3 {' '.join(args)} -> rc={r.returncode}\n{r.stderr[-2000:]}")
        return self.parse(r.stdout)

    @staticmethod
    def parse(stdout: str) -> dict[str, str]:
        out: dict[str, str] = {}
        for line in stdout.splitlines():
            if line.startswith("report="):
                out["report"] = line[len("report="):]
                continue
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    out[k] = v
        return out

    def raw(self, *args: str) -> str:
        r = subprocess.run([str(self.exe), *args], capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            raise RuntimeError(f"host_n3 {' '.join(args)} -> rc={r.returncode}\n{r.stderr[-2000:]}")
        return r.stdout


def compile_harness(ino: Path, workdir: Path) -> Path:
    exe = workdir / "host_n3"
    cxx = os.environ.get("CXX", "g++")
    cmd = [
        cxx, "-std=c++17", "-O2", "-Wall", "-Wextra",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
        "-I", str(HERE / "mock"),
        f'-DN3_INO="{ino}"',
        str(HERE / "host_n3.cpp"), "-o", str(exe),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("no compila el harness:\n" + r.stderr[-4000:])
    if r.stderr.strip():
        # advertencias del sketch en el host: visibles, no fatales
        sys.stderr.write(r.stderr)
    return exe


# -- SPEC-LINK.md: la fuente unica de los vectores y de las cifras del barrido -----

def _section(text: str, start: str, end: str) -> str:
    a = text.index(start)
    return text[a:text.index(end, a)]


def parse_spec() -> dict:
    text = SPEC.read_text(encoding="utf-8")
    vec = _section(text, "## Vectores dorados", "## Canales NDB")

    crc: list[tuple[bytes, int]] = []
    frames: list[tuple[str, bytes, bytes]] = []  # (caso, payload, trama)
    for line in vec.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 2 and re.fullmatch(r"0x[0-9A-Fa-f]{2}", cells[1].strip("`")):
            entry = cells[0]
            if "(ASCII)" in entry:
                data = re.search(r'"([^"]*)"', entry).group(1).encode("ascii")  # type: ignore[union-attr]
            else:
                data = bytes.fromhex(entry.strip("`").replace(" ", ""))
            crc.append((data, int(cells[1].strip("`"), 16)))
        elif len(cells) == 3:
            m_p = re.match(r"`([0-9a-f]+)`", cells[1])
            m_t = re.fullmatch(r"`(aa[0-9a-f]+)`", cells[2])
            if m_p and m_t:
                frames.append((cells[0].strip("`"), bytes.fromhex(m_p.group(1)), bytes.fromhex(m_t.group(1))))

    sweep = _section(text, "### Debilidad conocida del formato", "## Contabilidad")
    num = r"([\d\s  ]+?)"
    m_len = re.search(r"\|\s*`LEN`\s*\|\s*" + num + r"\s*\|\s*\*{0,2}(\d+)\*{0,2}", sweep)
    m_body = re.search(r"\|\s*`PAYLOAD`\s*\+\s*`CRC`\s*\|\s*" + num + r"\s*\|\s*\*{0,2}(\d+)\*{0,2}", sweep)
    if not (m_len and m_body):
        raise RuntimeError("no encuentro la tabla del barrido de bit-flips en SPEC-LINK.md")

    def n(s: str) -> int:
        return int(re.sub(r"\D", "", s))

    return {
        "crc": crc,
        "frames": frames,
        "sweep": {
            "len_flips": n(m_len.group(1)), "len_ok": int(m_len.group(2)),
            "body_flips": n(m_body.group(1)), "body_ok": int(m_body.group(2)),
        },
    }


# -- (a) vectores dorados ------------------------------------------------------------

def test_vectors(s: Suite, h: Harness, spec: dict) -> None:
    s.section("(a) vectores dorados de SPEC-LINK.md")
    s.check("SPEC: 3 vectores CRC y 6 tramas leidos", len(spec["crc"]) == 3 and len(spec["frames"]) == 6,
            f"crc={len(spec['crc'])} frames={len(spec['frames'])}")
    for data, want in spec["crc"]:
        got = int(h.run("crc", data.hex())["crc"], 16)
        s.check(f"CRC-8 de {data!r} == 0x{want:02X}", got == want and dl.crc8(data) == want, f"sketch=0x{got:02X}")
    for case, payload, trama in spec["frames"]:
        got = h.run("frame", payload.hex())["frame"]
        s.check(f"trama {case}: armarTrama", got == trama.hex() == dl.build_frame(payload).hex(), got)
        m = re.fullmatch(r"seq=(\d+)", case)
        if m:
            g = h.run("gen", m.group(1))
            s.check(f"trama {case}: generarPayload", g["payload"] == payload.hex() and g["frame"] == trama.hex(),
                    f"{g}")
        r = h.raw("feed", trama.hex()).split()
        s.check(f"trama {case}: la FSM del receptor la entrega OK",
                r[0] == "results=1" and r[1] == "len_err=0" and r[2] == f"ok:{len(payload)}:{payload.hex()}", " ".join(r))

    # el generador completo contra dio_link.py (independiente del SPEC)
    lines = h.raw("genall").splitlines()
    bad = [ln for ln in lines
           if (lambda q: q[1] != dl.testframe_payload(int(q[0])).hex() or q[2] != dl.build_frame(dl.testframe_payload(int(q[0]))).hex())(ln.split())]
    s.check("generador seq 0..999 == dio_link.testframe_payload", len(lines) == 1000 and not bad, f"{len(bad)} distintas")


# -- (b) barrido de bit-flips ----------------------------------------------------------

def test_sweep(s: Suite, h: Harness, spec: dict) -> None:
    s.section("(b) barrido de bit-flips, seq 0..999 (+36 bytes de linea inactiva)")
    r = h.run("sweep")
    sw = spec["sweep"]
    s.check(f"volteos en LEN == {sw['len_flips']}", int(r["len_flips"]) == sw["len_flips"], r["len_flips"])
    s.check(f"volteos en PAYLOAD/CRC == {sw['body_flips']}", int(r["body_flips"]) == sw["body_flips"], r["body_flips"])
    s.check(f"FRAME_OK espurios por LEN == {sw['len_ok']} (exacto)",
            int(r["len_ok_total"]) == sw["len_ok"] == int(r["len_flips_with_ok"]) == int(r["len_first_ok"]),
            f"total={r['len_ok_total']} volteos_con_ok={r['len_flips_with_ok']} primero_ok={r['len_first_ok']}")
    s.check(f"FRAME_OK espurios por PAYLOAD/CRC == {sw['body_ok']} (exacto)",
            int(r["body_ok_total"]) == sw["body_ok"] and int(r["body_flips_with_ok"]) == 0 and int(r["body_first_ok"]) == 0,
            f"total={r['body_ok_total']}")


# -- (c) tramas pegadas -----------------------------------------------------------------

def test_glued(s: Suite, h: Harness) -> None:
    s.section("(c) tramas pegadas: sin falso sincronismo con CRC en nibble bajo 0xA")
    r = h.run("glued")
    # el numero de tramas con nibble 0xA lo calcula Python de forma independiente:
    want = sum(1 for q in range(999) if dl.build_frame(dl.testframe_payload(q))[-1] & 0x0F == 0x0A)
    s.check("hay tramas con CRC en nibble 0xA (el caso no es vacuo)", int(r["nibble_a_frames"]) == want > 0,
            f"sketch={r['nibble_a_frames']} python={want}")
    s.info(f"{want} tramas seq 0..998 terminan en nibble bajo 0xA")
    s.check("cada una de ellas + la siguiente pegada: 2 OK, sin len_err", int(r["pair_fail"]) == 0, r["pair_fail"])
    s.check("1000 tramas seguidas sin hueco: 1000 OK, 0 malas, 0 len_err",
            (int(r["run_ok"]), int(r["run_bad"]), int(r["run_len_err"])) == (1000, 0, 0),
            f"ok={r['run_ok']} bad={r['run_bad']} len_err={r['run_len_err']}")


# -- (d) enlace TX -> RX por el registro de bits --------------------------------------

def test_link(s: Suite, h: Harness, work: Path) -> None:
    s.section("(d) enlace TX -> RX de extremo a extremo (registro GPIO simulado)")
    lst = h.run("speedlist")
    s.check(f"lista de velocidades == {SPEED_LIST}", [int(x) for x in lst["speeds"].split(",")] == SPEED_LIST,
            lst["speeds"])
    s.check("TOTAL_TRAMAS == 1000", int(lst["total_tramas"]) == TOTAL_FRAMES)

    want_lines = [
        "".join(str(b) for b in dl.frame_to_bits(dl.build_frame(dl.testframe_payload(q)))) for q in range(TOTAL_FRAMES)
    ]
    want_bits = sum((2 + len(dl.testframe_payload(q))) * 8 for q in range(TOTAL_FRAMES))

    for hz in SPEED_LIST:
        bits_file = work / f"tx_{hz}.bits"
        r = h.run("link", str(hz), str(bits_file))
        tag = f"{hz} bit/s"
        got_lines = bits_file.read_text().split()
        s.check(f"{tag}: el TX pone en el cable exactamente las tramas del SPEC (1000 tramas)",
                got_lines == want_lines, f"{len(got_lines)} tramas")
        s.check(f"{tag}: RX 1000 OK, 0 crc_err, 0 len_err, 0 overruns",
                (int(r["ok"]), int(r["crc_err"]), int(r["len_err"]), int(r["overruns"])) == (TOTAL_FRAMES, 0, 0, 0), str(r))
        s.check(f"{tag}: bit_errors == 0 y BER == 0",
                int(r["bit_errors"]) == 0 and r["report"].split(",")[5] == "0.000e+00", r["report"])
        s.check(f"{tag}: bits_rx == {want_bits} y siguiente esperada == 1000, perdidas == 0",
                int(r["bits_rx"]) == want_bits and int(r["esperado"]) == TOTAL_FRAMES and r["report"].split(",")[6] == "0",
                r["report"])
        nominal = 240_000_000 / hz
        s.check(f"{tag}: periodo de reloj medio == nominal ({nominal:.0f} ciclos, +-0,5 %)",
                abs(float(r["period_mean"]) - nominal) <= 0.005 * nominal, r["period_mean"])
        medio = nominal / 2
        # step = medio/8: el dato debe estar estable ~medio periodo antes y despues del flanco
        s.check(f"{tag}: setup y hold del dato >= medio periodo",
                float(r["min_setup"]) >= 0.85 * medio and float(r["min_hold"]) >= 0.85 * medio,
                f"setup={r['min_setup']} hold={r['min_hold']} medio={medio:.0f}")
        s.check(f"{tag}: hueco entre tramas >= 16 periodos",
                float(r["min_gap_periods"]) >= 16.0, r["min_gap_periods"])
        s.check(f"{tag}: al terminar DATA=0 y CLK=0",
                r["final_data"] == "0" and r["final_clk"] == "0" and r["tx_seq"] == str(TOTAL_FRAMES))

    # arranque del transmisor: latch a 0 antes de habilitar el driver
    r = h.run("setuporder")
    s.check("setup() del TX: latch de DATA y CLK a 0 ANTES de pinMode(OUTPUT), y en reposo",
            (r["data_latch_first"], r["clk_latch_first"], r["latch_a_cero"]) == ("1", "1", "1"), str(r))

    # comandos serie '+', '-' y 'b<N>'
    r_raw = h.raw("speeds").splitlines()
    got = [int(re.search(r"rate0?=(\d+)", ln).group(1)) for ln in r_raw]  # type: ignore[union-attr]
    rate = 1000
    want = [rate]
    for c in ["+"] * 6 + ["-"] * 7 + ["b12345", "+", "-", "b50", "b600000"]:
        if c == "+":
            rate = next((v for v in SPEED_LIST if v > rate), SPEED_LIST[-1])
        elif c == "-":
            rate = next((v for v in reversed(SPEED_LIST) if v < rate), SPEED_LIST[0])
        else:
            n = int(c[1:])
            rate = n if 100 <= n <= 500000 else rate
        want.append(rate)
    s.check("comandos '+', '-', 'b<N>' recorren la lista y validan el rango", got == want, f"{got}")


# -- (e) contra-prueba de deriva: sketch vs LinkMonitor ----------------------------------

def make_stream(rng: random.Random, n_frames: int, p_flip: float, p_drop_frame: float, p_drop_bit: float) -> str:
    out: list[str] = []
    for seq in range(n_frames):
        if rng.random() < p_drop_frame:
            continue  # trama perdida entera
        for b in dl.frame_to_bits(dl.build_frame(dl.testframe_payload(seq))):
            if rng.random() < p_drop_bit:
                continue  # pulso de reloj perdido
            if rng.random() < p_flip:
                b ^= 1
            out.append("1" if b else "0")
    return "".join(out)


# (nombre, tramas, p_flip, p_drop_trama, p_drop_bit, semilla): los 5 escenarios de la
# primera verificacion + 6 flujos cortos con flips al 0,2 %, todo con semilla fija.
SCENARIOS: list[tuple[str, int, float, float, float, int]] = [
    ("limpio", 1000, 0.0, 0.0, 0.0, 1),
    ("flips 0,1 %", 1000, 0.001, 0.0, 0.0, 2),
    ("flips 0,5 % + 5 % tramas perdidas", 1000, 0.005, 0.05, 0.0, 3),
    ("flips 0,2 % + 2 % tramas + 0,02 % pulsos perdidos", 1000, 0.002, 0.02, 0.0002, 4),
    ("flips 2 % + 10 % tramas + 0,1 % pulsos perdidos", 1000, 0.02, 0.10, 0.001, 5),
] + [(f"flujo corto {i} (60 tramas, flips 0,2 %)", 60, 0.002, 0.0, 0.0, 100 + i) for i in range(1, 7)]


def monitor_counters(bits: str) -> tuple[dict[str, int], str, int]:
    mon = dl.LinkMonitor()
    mon.feed_bits(1 if c == "1" else 0 for c in bits)
    st = mon.stats
    lost = max(0, TOTAL_FRAMES - st.frames_ok - st.frames_crc_err)
    csv = f"{st.frames_ok},{st.frames_crc_err},{st.frames_len_err},{st.bits_rx},{st.bit_errors},{st.ber:.3e},{lost}"
    ctr = {"ok": st.frames_ok, "crc_err": st.frames_crc_err, "len_err": st.frames_len_err,
           "bits_rx": st.bits_rx, "bit_errors": st.bit_errors}
    return ctr, csv, mon.expected


def test_drift(s: Suite, h: Harness, work: Path) -> None:
    s.section("(e) contra-prueba de deriva: sketch vs LinkMonitor (dio_link.py), contadores EXACTOS")
    for name, n, pf, pd, pb, seed in SCENARIOS:
        bits = make_stream(random.Random(seed), n, pf, pd, pb)
        path = work / f"stream_{seed}.bits"
        path.write_text(bits)
        r = h.run("stream", str(path))
        ctr, csv, expected = monitor_counters(bits)
        same_ctr = all(int(r[k]) == v for k, v in ctr.items())
        s.check(f"{name}: 7 contadores idénticos", same_ctr and r["report"] == csv and int(r["esperado"]) == expected,
                f"sketch={r['report']} esperado={r['esperado']}  |  dio_link={csv} esperado={expected}")
        s.info(f"{name}: {csv}")


# -- ejecucion -------------------------------------------------------------------------

def run_suite(ino: Path, work: Path, quiet: bool = False) -> Suite:
    s = Suite(quiet=quiet)
    exe = compile_harness(ino, work)
    h = Harness(exe)
    spec = parse_spec()
    test_vectors(s, h, spec)
    test_sweep(s, h, spec)
    test_glued(s, h)
    test_link(s, h, work)
    test_drift(s, h, work)
    return s


# Mutaciones para --mutation-check: (nombre, [(viejo, nuevo)], ...). Cada `viejo`
# debe aparecer EXACTAMENTE una vez en el .ino; si no, la mutacion queda obsoleta
# y el propio check falla (no se puede pasar en silencio).
MUTATIONS: list[tuple[str, list[tuple[str, str]]]] = [
    ("polinomio CRC 0x07 -> 0x1D", [("(uint8_t)((crc << 1) ^ 0x07)", "(uint8_t)((crc << 1) ^ 0x1D)")]),
    ("semilla del generador 0xC0FFEE -> 0xC0FFEF", [("#define SEED 0xC0FFEEu", "#define SEED 0xC0FFEFu")]),
    ("bit_errors: relleno 0xFF en vez de ceros (regla de longitud distinta)",
     [("uint8_t esperado[MAX_LEN] = {0};", "uint8_t esperado[MAX_LEN]; for (uint8_t q = 0; q < MAX_LEN; q++) esperado[q] = 0xFF;")]),
    ("sin resincronizacion del indice esperado",
     [("idx = k + ((t.payload[0] - k) & 0xFFu);", "idx = k;")]),
    ("LEN maximo 32 -> 31 en la FSM", [("byte > MAX_LEN)", "byte > MAX_LEN - 1)")]),
    ("registro de desplazamiento sin poner a cero por byte (falso sincronismo)",
     [("  g_fsm.registro = 0;\n  g_fsm.nbits = 0;\n}", "  g_fsm.nbits = 0;\n}"),
      ("  uint8_t byte = g_fsm.registro;\n  g_fsm.registro = 0;", "  uint8_t byte = g_fsm.registro;")]),
    ("TX envia LSB primero", [("enviarBit((trama[i] >> b) & 1, plazo)", "enviarBit((trama[i] >> (7 - b)) & 1, plazo)")]),
    ("hueco entre tramas 16 -> 8 periodos", [("#define HUECO_PERIODOS 16", "#define HUECO_PERIODOS 8")]),
    ("la ISR ignora el dato (lee siempre 0)",
     [("rxAlimentarBit((GPIO.in & MASK_RX_DATA) ? 1 : 0);", "rxAlimentarBit(0);")]),
    ("setup() del TX: pinMode(OUTPUT) antes de poner el latch a 0 (orden antiguo)",
     [("    GPIO.out_w1tc = MASK_TX_DATA | MASK_TX_CLK;   // reposo: DATA=0, CLK=0\n    pinMode(TX_DATA, OUTPUT);\n    pinMode(TX_CLK, OUTPUT);\n",
       "    pinMode(TX_DATA, OUTPUT);\n    pinMode(TX_CLK, OUTPUT);\n    GPIO.out_w1tc = MASK_TX_DATA | MASK_TX_CLK;   // reposo: DATA=0, CLK=0\n")]),
    ("CRC no cubre LEN", [("uint8_t crc = crc8Byte(0, len);\n  for (uint8_t i = 0; i < len; i++) {\n    out[2 + i]",
                           "uint8_t crc = 0;\n  for (uint8_t i = 0; i < len; i++) {\n    out[2 + i]")]),
]


def mutation_check(base_ino: Path) -> int:
    text = base_ino.read_text(encoding="utf-8")
    undetected = 0
    for name, edits in MUTATIONS:
        mutated = text
        for old, new in edits:
            n = mutated.count(old)
            if n != 1:
                print(f"OBSOLETA  {name}: el fragmento aparece {n} veces (debe ser 1): {old!r}")
                undetected += 1
                break
            mutated = mutated.replace(old, new)
        else:
            with tempfile.TemporaryDirectory(prefix="n3mut_") as td:
                w = Path(td)
                ino = w / "n3_bytes.ino"
                ino.write_text(mutated, encoding="utf-8")
                try:
                    s = run_suite(ino, w, quiet=True)
                except RuntimeError as e:
                    print(f"DETECTADA (falla al ejecutar)  {name}: {str(e).splitlines()[0][:100]}")
                    continue
                if s.failures:
                    print(f"DETECTADA  {name}  -> {len(s.failures)} checks fallan, p. ej. '{s.failures[0]}'")
                else:
                    print(f"NO DETECTADA  {name}")
                    undetected += 1
    print(f"\n{len(MUTATIONS) - undetected}/{len(MUTATIONS)} mutaciones detectadas")
    return 1 if undetected else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ino", type=Path, default=DEFAULT_INO)
    ap.add_argument("--mutation-check", action="store_true")
    ap.add_argument("--keep", type=Path, default=None)
    args = ap.parse_args()
    ino = args.ino.resolve()

    if args.mutation_check:
        return mutation_check(ino)

    print(f"sketch bajo prueba: {ino.relative_to(REPO) if ino.is_relative_to(REPO) else ino}")
    if args.keep:
        args.keep.mkdir(parents=True, exist_ok=True)
        s = run_suite(ino, args.keep)
    else:
        with tempfile.TemporaryDirectory(prefix="n3test_") as td:
            s = run_suite(ino, Path(td))
    print()
    if s.failures:
        print(f"FALLA: {len(s.failures)} de {s.checks} comprobaciones")
        for f in s.failures:
            print(f"  - {f}")
        return 1
    print(f"OK: {s.checks} comprobaciones")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
