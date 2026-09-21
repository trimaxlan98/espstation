#!/usr/bin/env python3
"""Capturador de puerto serie con canal de comandos (el que se uso para la evidencia).

Lee el puerto a 115200, escribe cada linea en un log con la marca de tiempo del HOST
(segundos desde que arranco el capturador) y deja mandar comandos a la placa sin cerrar
el puerto: lo que se escriba en la FIFO se envia a la placa y se anota con ">>>".
Una linea que empiece por "#MARK" solo se anota en el log (para marcar inicios/fines de tanda).

Requisitos: python3 y pyserial (pip install pyserial); permisos sobre el puerto
(en Linux, grupo `dialout`).

Uso (una terminal por placa):
    python3 captura_serie.py /dev/ttyUSB1 evidencia/B.log /tmp/cmd_B
y desde otra terminal:
    echo "r"                  > /tmp/cmd_B      # imprimir umbrales y contadores
    echo "p170"               > /tmp/cmd_B      # fijar punto_raya_ms = 170
    echo "#MARK INICIO TANDA 1" > /tmp/cmd_B    # solo marca en el log

NOTA: la captura serie de las dos placas mostro en algunas lineas bloques de 64 bytes 0xFF
que pisan el principio de la linea (causa no encontrada). Ver INFORME.md.
"""
import os
import sys
import time

import serial

if len(sys.argv) != 4:
    sys.exit(__doc__)
puerto, salida, fifo = sys.argv[1:4]
if not os.path.exists(fifo):
    os.mkfifo(fifo)
fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
buf = b""
with serial.Serial(puerto, 115200, timeout=0.2) as s, open(salida, "wb", buffering=0) as f:
    t0 = time.time()
    ts = lambda: f"{time.time() - t0:9.3f} ".encode()
    while True:
        linea = s.readline()
        if linea:
            f.write(ts() + linea)
        try:
            datos = os.read(fd, 256)
        except BlockingIOError:
            datos = b""
        buf += datos
        while b"\n" in buf:
            cmd, buf = buf.split(b"\n", 1)
            if cmd.startswith(b"#MARK"):
                f.write(ts() + cmd + b"\n")
            elif cmd:
                s.write(cmd + b"\n")
                f.write(ts() + b">>> " + cmd + b"\n")
