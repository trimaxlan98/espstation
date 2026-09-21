#!/usr/bin/env python3
"""Compara TRES decodificaciones de los mismos pulsos:

  1. lo que imprimio la placa REAL en el banco (evidencia/*.log, via visor/datos.json),
  2. lo que imprime receptor/receptor.ino compilado en el host (tests/host_replay.cpp),
  3. lo que decodifica el port JavaScript del visor (visor/decodificador.js, requiere node).

Ademas compara 2 y 3 sobre 400 secuencias aleatorias (umbrales y tiempos alrededor de los limites).
Prueba que el host y el visor reproducen a la placa sobre estos datos. NO prueba la temporizacion
real del microcontrolador. Uso:  python3 bench/practicas/clave-morse/tests/replay_evidencia.py
"""
import json, random, shutil, subprocess, sys, tempfile
from pathlib import Path

AQUI = Path(__file__).resolve().parent
RAIZ = AQUI.parent


def compilar(d):
    exe = Path(d) / "host_replay"
    cmd = ["g++", "-std=c++17", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-I", str(AQUI / "mock"),
           f'-DRECEPTOR_INO="{RAIZ / "receptor" / "receptor.ino"}"', str(AQUI / "host_replay.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); sys.exit("no compila host_replay.cpp")
    return exe


def cpp(exe, um, pulsos):
    ent = f"{um['p']} {um['l']} {um['w']} {um['d']}\n" + "".join(f"{x['sil'] or 0} {x['dur']}\n" for x in pulsos)
    r = subprocess.run([str(exe)], input=ent, capture_output=True, text=True)
    if r.returncode:
        sys.exit("host_replay fallo: " + r.stderr)
    return [l for l in r.stdout.split("\n") if l and not l.startswith("#")]


def nodejs(casos):
    r = subprocess.run(["node", str(RAIZ / "visor" / "decodificador_cli.js")], input=json.dumps({"casos": casos}),
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("node fallo: " + r.stderr)
    return json.loads(r.stdout)


def main():
    fallos = 0
    tandas = json.loads((RAIZ / "visor" / "datos.json").read_text(encoding="utf-8"))
    hay_node = shutil.which("node") is not None
    if not hay_node:
        print("AVISO: node no esta instalado; se omite la comparacion del port JS del visor")
    with tempfile.TemporaryDirectory() as d:
        exe = compilar(d)
        casos = [dict(umbrales=t["umbrales"], pulsos=t["pulsos"]) for t in tandas]
        js = nodejs(casos) if hay_node else [None] * len(tandas)
        print("=== evidencia real: placa vs sketch en host vs port JS del visor")
        for t, salida_js in zip(tandas, js):
            placa = [e["v"] for e in t["placa"]]
            host = cpp(exe, t["umbrales"], t["pulsos"])
            ok_host = host == placa
            ok_js = salida_js is None or salida_js == placa
            print(f"tanda {t['id']}: {len(placa)} eventos | sketch en host == placa: {ok_host} | "
                  f"port JS == placa: {'omitido' if salida_js is None else ok_js}")
            if not ok_host:
                fallos += 1
                for i, (a, b) in enumerate(zip(placa, host)):
                    if a != b: print(f"   primera diferencia en el evento {i}: placa={a!r} host={b!r}"); break
                if len(placa) != len(host): print(f"   longitudes: placa={len(placa)} host={len(host)}")
            if not ok_js: fallos += 1
        if hay_node:
            rnd = random.Random(20260921)
            aleatorios = []
            for _ in range(400):
                l = rnd.randint(300, 1500); w = rnd.randint(l + 100, 4000); p = rnd.randint(50, 400); dd = rnd.randint(5, 40)
                pulsos = []
                for i in range(rnd.randint(1, 15)):
                    dur = rnd.choice([rnd.randint(1, 60), p - 1, p, p + 1, dd - 1, dd, rnd.randint(30, 900)])
                    sil = rnd.choice([rnd.randint(50, 600), l - 1, l, l + 1, w - 1, w, w + 1, rnd.randint(50, 5000)])
                    pulsos.append(dict(sil=None if i == 0 else max(1, sil), dur=max(1, dur)))
                aleatorios.append(dict(umbrales=dict(p=p, l=l, w=w, d=dd), pulsos=pulsos))
            js2 = nodejs(aleatorios)
            dif = 0
            for c, sj in zip(aleatorios, js2):
                if cpp(exe, c["umbrales"], c["pulsos"]) != sj:
                    dif += 1
            print(f"=== 400 secuencias aleatorias (limites incluidos): sketch en host vs port JS, {dif} diferencias")
            fallos += dif > 0
    print("RESULTADO:", "FALLA" if fallos else "TODO OK")
    return 1 if fallos else 0


if __name__ == "__main__":
    sys.exit(main())
