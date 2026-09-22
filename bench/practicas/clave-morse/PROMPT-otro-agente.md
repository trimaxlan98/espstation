# Prompt para un agente de Claude Code en OTRA computadora

> **Cómo usarlo.** En la otra computadora, abre Claude Code en una carpeta vacía y escribe **solo esta línea**:
>
> ```
> Clona https://github.com/trimaxlan98/espstation.git, cambia a la rama feat/clave-morse, lee
> bench/practicas/clave-morse/PROMPT-otro-agente.md y ejecútalo paso a paso.
> ```
>
> Ese agente **no tiene el contexto de la sesión donde se hizo la práctica**: este archivo y `INFORME.md` son todo lo que necesita.
> El texto de abajo está escrito para ese agente.

---

## 0. Qué es y cuál es tu trabajo

En `bench/practicas/clave-morse/` hay una práctica terminada de electrónica/firmware: **una llave Morse táctil entre dos ESP32**.
La placa **A** lee una llave (dos puntas de jumper peladas) y copia su nivel por un cable a la placa **B**, que mide pulsos, decodifica
Morse y lo imprime como texto y como byte ASCII en binario. Hay una interfaz web (`visor/index.html`) y un puente serie→navegador
(`herramientas/puente_serie.py`) para verla **en tiempo real con las placas reales**.

**Tu trabajo es comprobar que la práctica funciona en ESTA computadora**, sin cambiar nada de su contenido, y **reportar con honestidad**.
No es tu trabajo mejorarla, refactorizarla ni «arreglar» resultados.

Autoría de la práctica: Yuritzi Elena Ordaz Huerta, Maestría en Tecnología Avanzada, UPIITA-IPN.

## 1. Reglas (no las rompas)

1. **No modifiques** `transmisor/`, `receptor/`, `tests/`, `visor/` ni `evidencia/`. Si algo falla, **repórtalo tal cual**; no cambies el criterio ni el código para que pase.
2. **No hagas commit ni push** salvo que la persona te lo pida explícitamente. Este repo tiene un `CLAUDE.md` con más reglas: léelo, pero **esta práctica es autónoma** y no toca el protocolo, el firmware base, el gateway ni el desktop.
3. **Cualquier acción física o irreversible se confirma antes con la persona:** subir sketches a las placas (`arduino-cli upload`), cablear, desenchufar. Pídele que cablee ella y espera su confirmación.
4. **No inventes ni redondees cifras.** Todo número de tu reporte sale de una salida que corriste y pegaste. Lo que no puedas ejecutar se marca **NO VERIFICADO** y dices por qué.
5. **No uses GPIO12**, no unas dos salidas entre sí, no sustituyas el cable por WiFi/BLE.
6. Si algo bloquea (permisos del puerto, herramienta que falta), **para y dilo**; no lo rodees con `sudo`.

## 2. Preparar el entorno

Todo se probó **solo en Linux**. Windows/macOS no se probaron: los puertos se llaman distinto (`COM3`, `/dev/cu.usbserial-*`) y no existe el grupo `dialout`; dilo si aplica.

```bash
git clone https://github.com/trimaxlan98/espstation.git      # si aún no está
cd espstation && git checkout feat/clave-morse
cd bench/practicas/clave-morse

python3 --version && g++ --version | head -1                  # necesarios para las pruebas sin hardware
node --version || echo "node no instalado (opcional: solo para comparar el visor)"
python3 -c "import serial; print('pyserial', serial.__version__)" || pip install pyserial   # solo para placas reales
```

Solo si va a haber placas reales:

```bash
arduino-cli version                                            # se probó con 1.5.1
arduino-cli core list | grep esp32:esp32 || arduino-cli core install esp32:esp32   # ~1 GB; se probó con 3.3.11
id | grep dialout || echo "FALTA el grupo dialout: para y dile a la persona que ejecute  sudo usermod -aG dialout \$USER  y vuelva a iniciar sesión"
```

## 3. Verificación SIN hardware (haz esto siempre primero)

Desde `bench/practicas/clave-morse/`:

```bash
python3 tests/run_tests.py        # esperado: "18 pruebas, 0 fallos", "7 pruebas, 0 fallos", tabla 36 entradas 0 fallos, RESULTADO: TODO OK
python3 tests/replay_evidencia.py # esperado: las 4 tandas "== placa: True" y "400 secuencias aleatorias ... 0 diferencias", TODO OK (sin node se omite la parte del visor)
python3 tests/test_puente.py      # esperado: "24 pruebas, 0 fallos"
python3 visor/generar_datos.py    # esperado: 4 tandas con sus pulsos == puntos+rayas de la placa; regenera visor/datos.json
git status --short                # esperado: vacío. Si datos.json/datos.js cambiaron, repórtalo (deberían ser idénticos)
```

Pega la salida real de cada uno. Si alguno falla, **para ahí y reporta** (qué comando, salida completa, qué versión de g++/python/node tienes).

## 4. Ver el visor y la demo (tampoco necesita placas)

```bash
python3 herramientas/puente_serie.py --demo 3    # imprime: Puente listo: http://127.0.0.1:8765/#vivo
```

Pídele a la persona que abra esa dirección (o ábrela tú si tienes navegador): debe verse la pestaña **2 · En vivo** con
`puente: conectado`, un letrero **DEMO**, y en unos segundos la traducción `SOS SOS…` con el byte de cada letra y la señal. Cuando termines,
para el puente (Ctrl+C). También se puede abrir `visor/index.html` directamente (doble clic) para las demás pestañas: la 4 muestra las
4 tandas reales y la 5 el circuito con la justificación de cada cable (**el GND común** en especial).

## 5. Con las dos placas ESP32 (solo si la persona las tiene)

Lee antes **`INFORME.md` secciones 4 (conexiones y por qué) y 6 (replicar)** y `docs/circuito.svg`/`.png`.

1. **Pídele a la persona que cablee** (tú NO puedes). Dale un diagrama ASCII corto y espera su confirmación explícita:
   ```
   A.GPIO26 ──[330 Ω]──► B.GPIO25        señal (un solo sentido)
   A.GND ────────────── B.GND            GND COMÚN (obligatorio)
   A.3V3 → punta pelada ┐  (se presionan una contra otra: metal con metal)
   A.GPIO13 ← punta pelada ┘             llave
   GPIO4 → 330 Ω → LED → GND en cada placa (testigo)
   ```
2. Identifica los puertos: `arduino-cli board list` (deben salir **dos**). Las placas son idénticas: **A = la que tiene la llave**. Confirma con la persona cuál es cuál; si la numeración cambia al reconectar, reasigna.
3. **Antes de subir, pide confirmación** (esto reemplaza lo que tengan las placas). Luego:
   ```bash
   FQBN=esp32:esp32:esp32
   arduino-cli compile --fqbn $FQBN --warnings all transmisor    # esperado: 273256 bytes, sin avisos
   arduino-cli compile --fqbn $FQBN --warnings all receptor      # esperado: 277240 bytes, sin avisos
   arduino-cli upload  --fqbn $FQBN -p <PUERTO_A> transmisor
   arduino-cli upload  --fqbn $FQBN -p <PUERTO_B> receptor
   ```
4. Arranca el puente con las placas reales y pide a la persona que abra la dirección que imprime:
   ```bash
   python3 herramientas/puente_serie.py --a <PUERTO_A> --b <PUERTO_B>
   ```
   **Solo un programa puede abrir cada puerto:** cierra cualquier monitor serie antes.
5. Pídele que **presione la llave**. Debe verse en directo: el nivel de la llave (0/1 con su LED), el trazo de la señal, el símbolo que recibe B y,
   tras una pausa, la letra con su byte. **Pregúntale qué ve** y repórtalo con sus palabras; tú no ves la pantalla ni el circuito.
   Criterio de que el montaje va bien: al presionar, el LED de A y el de B se encienden a la vez.
6. Los umbrales de la placa B (`p/l/w`) son **de un operador concreto** y se pierden al reiniciar la placa; si decodifica mal, es esperable (ver `INFORME.md` §8): calibra desde la pestaña En vivo o con `v` + `p/l/w`, **dilo en el reporte** y no lo presentes como fallo del circuito.

## 6. Qué debes saber (para no asustarte ni engañar)

- **El resultado de la práctica es parcial y así está documentado:** el montaje y la transmisión funcionan; la decodificación de Morse tecleado a mano es **frágil** (2 de 6 SOS con umbrales congelados, un operador). No lo «arregles» ni lo maquilles.
- **Hay una ráfaga de datos viejos repetidos al abrir un puerto ya usado** (causa no comprobada; probablemente driver USB-serie). El puente la descarta al conectar. Si ves «bytes viejos descartados» es normal.
- **Basura de 64 bytes `0xFF`** en algunas líneas del serie: conocida, no afecta a la decodificación.
- `.zip` de entrega y PDF: `INFORME.pdf` está en la carpeta; el `.zip` no está en el repo.

## 7. Reporte final (formato)

1. **Entorno:** SO, versiones (python, g++, node, arduino-cli, core esp32, pyserial), navegador.
2. **Cada comando que corriste y su salida real** (sección 3 completa). Resultado: pasa / falla, con la salida.
3. **Demo:** ¿se vio la pestaña En vivo en DEMO? (lo que dijo la persona).
4. **Con placas (si aplica):** puertos, qué se subió, qué vio la persona al presionar la llave, contadores si los pediste (`r`).
5. **Lo que NO pudiste verificar y por qué.**
6. **Diferencias con lo esperado**, con la salida que las prueba. Si todo coincidió, dilo sin adornos.
7. **Ningún archivo modificado** (`git status --short`); si algo cambió, cuál y por qué.
