#!/usr/bin/env python3
"""Capturador de puerto serie con canal de comandos (el que se uso para la evidencia).

Lee el puerto a 115200, escribe cada linea en un log con la marca de tiempo del HOST
(segundos desde que arranco el capturador) y deja mandar comandos a la placa sin cerrar
el puerto: lo que se escriba en el canal se envia a la placa y se anota con ">>>".
Una linea que empiece por "#MARK" solo se anota en el log (para marcar inicios/fines de tanda).

El canal de comandos depende del sistema, pero el LOG es identico en los dos:
  - POSIX: una FIFO (`os.mkfifo`), como siempre.
  - Windows: un fichero normal al que se APENDEAN lineas; el capturador recuerda por
    donde iba y envia solo lo nuevo. Windows no tiene FIFOs.

Requisitos: python3 y pyserial (pip install pyserial); permisos sobre el puerto
(en Linux, grupo `dialout`).

Uso (una terminal por placa):
    python3 captura_serie.py /dev/ttyUSB1 evidencia/B.log /tmp/cmd_B      # Linux/macOS
    python captura_serie.py COM7 evidencia/B.log cmd_B.txt                # Windows

y desde otra terminal:
    echo "r"                    > /tmp/cmd_B     # imprimir umbrales y contadores
    echo "p170"                 > /tmp/cmd_B     # fijar punto_raya_ms = 170
    echo "#MARK INICIO TANDA 1" > /tmp/cmd_B     # solo marca en el log

    Add-Content cmd_B.txt "r"                    # lo mismo en PowerShell
    Add-Content cmd_B.txt "#MARK INICIO TANDA 1"

En Windows el fichero de comandos se TRUNCA al arrancar, para no reenviar los comandos
de una sesion anterior.

NOTA: la captura serie de las dos placas mostro en algunas lineas bloques de 64 bytes 0xFF
que pisan el principio de la linea (causa no encontrada). Ver INFORME.md.
"""
import os
import stat
import sys
import time

import serial

MAX_BUF_CANAL = 4096   # una linea suelta de comando no llega ni a 100 bytes


class CanalFifo:
    """POSIX: una FIFO abierta en no bloqueante."""

    def __init__(self, ruta):
        if not os.path.exists(ruta):
            os.mkfifo(ruta)
        elif not stat.S_ISFIFO(os.stat(ruta).st_mode):
            # Un fichero normal NO sirve de canal aunque os.open() lo abra igual:
            #  1. la primera lectura reenviaria a la placa TODO lo que hubiera
            #     dentro, o sea los comandos de la sesion anterior (la rama de
            #     Windows trunca el fichero justo por este motivo);
            #  2. el uso documentado es `echo "r" > ruta`, que TRUNCA; el
            #     descriptor se queda pasado del final del fichero y el canal
            #     enmudece para siempre sin avisar.
            # Mejor parar y que el operador decida que hacer con ese fichero.
            sys.exit(f"{ruta} ya existe y NO es una FIFO. Borralo o usa otra ruta.")
        self.fd = os.open(ruta, os.O_RDWR | os.O_NONBLOCK)
        self.buf = b""

    def leer(self):
        try:
            self.buf += os.read(self.fd, 256)
        except BlockingIOError:
            pass
        if len(self.buf) > MAX_BUF_CANAL:   # escritor que nunca cierra la linea
            self.buf = b""
        out = []
        while b"\n" in self.buf:
            cmd, self.buf = self.buf.split(b"\n", 1)
            out.append(cmd)
        return out


class CanalFichero:
    """Windows: fichero normal; se lee solo lo nuevo desde la ultima vuelta."""

    def __init__(self, ruta):
        self.ruta = ruta
        with open(ruta, "wb"):  # truncar: lo de una sesion anterior no debe reenviarse
            pass
        self.leido = 0

    def leer(self):
        try:
            tam = os.path.getsize(self.ruta)
        except OSError:
            return []
        if tam < self.leido:  # alguien lo vacio: volver a empezar
            self.leido = 0
        if tam <= self.leido:
            return []
        with open(self.ruta, "rb") as c:
            c.seek(self.leido)
            datos = c.read()
        # Solo se consumen lineas COMPLETAS; a un escritor a medias se le espera.
        corte = datos.rfind(b"\n")
        if corte < 0:
            return []
        self.leido += corte + 1
        return [c.replace(b"\r", b"") for c in datos[:corte].split(b"\n")]


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    puerto, salida, canal_ruta = argv[1:4]
    canal = CanalFifo(canal_ruta) if hasattr(os, "mkfifo") else CanalFichero(canal_ruta)

    try:
        puerto_abierto = serial.Serial(puerto, 115200, timeout=0.2)
    except serial.SerialException as e:
        sys.exit(f"no puedo abrir {puerto}: {e}")

    with puerto_abierto as s, open(salida, "wb", buffering=0) as f:
        t0 = time.time()
        ts = lambda: f"{time.time() - t0:9.3f} ".encode()
        try:
            while True:
                linea = s.readline()
                if linea:
                    f.write(ts() + linea)
                for cmd in canal.leer():
                    cmd = cmd.strip()
                    if cmd.startswith(b"#MARK"):
                        f.write(ts() + cmd + b"\n")
                    elif cmd:
                        s.write(cmd + b"\n")
                        f.write(ts() + b">>> " + cmd + b"\n")
        except serial.SerialException as e:
            # El puerto se fue a media tanda (cable, reset de la placa, driver).
            # Queda anotado DENTRO del log: la evidencia explica por que se corto
            # en vez de acabar sin mas en mitad de una linea.
            f.write(ts() + b"#MARK CAPTURA INTERRUMPIDA: "
                    + str(e).encode("utf-8", "replace") + b"\n")
            sys.exit(f"puerto perdido a media captura: {e}")


if __name__ == "__main__":
    try:
        main(sys.argv)
    except KeyboardInterrupt:
        pass
