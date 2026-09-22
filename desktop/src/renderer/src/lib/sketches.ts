/**
 * The Arduino sketches of the bench practices, bundled into the app.
 *
 * WHY BUNDLED AND NOT READ FROM DISK. The packaged app is installed on
 * machines that do not have this repository — that is the whole point of
 * shipping an installer. Reading `bench/practicas/...` at runtime would work
 * on the development machine and show an empty list everywhere else, which is
 * the worst of the two outcomes because it only fails where nobody is
 * looking. `?raw` puts the text in the bundle at build time, so what the app
 * hands out is exactly the file that was in the tree when it was built.
 *
 * AND WHY NOT A COPY CHECKED INTO desktop/. A second copy of a sketch is a
 * second thing to keep in step with the hardware, and it would drift the
 * first time someone fixed a timing bug on the bench. There is one file, in
 * bench/, and this module points at it.
 */
import transceptorSrc from '@sketches/morse-duplex/transceptor/transceptor.ino?raw'
import transmisorSrc from '@sketches/clave-morse/transmisor/transmisor.ino?raw'
import receptorSrc from '@sketches/clave-morse/receptor/receptor.ino?raw'

export interface SketchPin {
  pin: string
  role: string
}

export interface Sketch {
  id: string
  /** The file name Arduino expects; the folder must carry the same stem. */
  file: string
  /** Where it lives in the repository, for anyone who has the repository. */
  path: string
  practice: string
  title: string
  summary: string
  /** Which board(s) this one goes on. */
  boards: string
  pins: SketchPin[]
  source: string
}

export const SKETCHES: Sketch[] = [
  {
    id: 'transceptor',
    file: 'transceptor.ino',
    path: 'bench/practicas/morse-duplex/transceptor/transceptor.ino',
    practice: 'morse-duplex',
    title: 'Transceptor full duplex',
    summary:
      'One sketch, flashed on BOTH boards. Each one keys its own line and decodes the ' +
      'other one at the same time, with independent thresholds per direction. This is the ' +
      'sketch the Morse section of this app was built around.',
    boards: 'Both boards, identical binary',
    pins: [
      { pin: 'GPIO13', role: 'KEY — the operator’s key, INPUT_PULLDOWN, other end to 3V3' },
      { pin: 'GPIO26', role: 'TX_DATA — out, mirrors the debounced key, through 330 Ω' },
      { pin: 'GPIO25', role: 'RX_DATA — in, from the other board’s GPIO26' },
      { pin: 'GPIO4', role: 'LED TX — own key' },
      { pin: 'GPIO16', role: 'LED RX — incoming level (WROOM only; it is PSRAM on WROVER)' }
    ],
    source: transceptorSrc
  },
  {
    id: 'transmisor',
    file: 'transmisor.ino',
    path: 'bench/practicas/clave-morse/transmisor/transmisor.ino',
    practice: 'clave-morse',
    title: 'Transmisor (board A)',
    summary:
      'The earlier one-way practice: A reads the key, debounces it and mirrors the level ' +
      'onto the wire. Nothing is decoded here — A only sends.',
    boards: 'Board A',
    pins: [
      { pin: 'GPIO13', role: 'KEY — the key, INPUT_PULLDOWN' },
      { pin: 'GPIO26', role: 'TX_DATA — out, through 330 Ω, to B’s GPIO25' },
      { pin: 'GPIO4', role: 'LED — the key level' }
    ],
    source: transmisorSrc
  },
  {
    id: 'receptor',
    file: 'receptor.ino',
    path: 'bench/practicas/clave-morse/receptor/receptor.ino',
    practice: 'clave-morse',
    title: 'Receptor (board B)',
    summary:
      'The other half of the one-way practice: B timestamps every edge in an interrupt and ' +
      'decodes dots, dashes, letters and words in the loop.',
    boards: 'Board B',
    pins: [
      { pin: 'GPIO25', role: 'RX_DATA — in, from A’s GPIO26' },
      { pin: 'GPIO4', role: 'LED — the raw received level' }
    ],
    source: receptorSrc
  }
]

/**
 * What you have to install before any of these will compile.
 *
 * The honest answer is "nothing beyond the ESP32 core". Every one of these
 * sketches includes `<Arduino.h>`, and two of them `"soc/gpio_struct.h"`,
 * which is part of ESP-IDF and therefore part of the core — there is no
 * third-party library to add, no Library Manager step, and nothing to pin to
 * a version. Checked against the `#include` lines of the three files, not
 * assumed.
 */
export const REQUIREMENTS = {
  libraries: 'None. No third-party library is used — do not open the Library Manager.',
  core: 'esp32 by Espressif Systems (Boards Manager). 2.x and 3.x both compile these.',
  boardsUrl: 'https://espressif.github.io/arduino-esp32/package_esp32_index.json',
  board: 'ESP32 Dev Module (or whatever your board actually is)',
  baud: '115200 — the Serial Monitor must match or the output is mojibake',
  wiring:
    'Common GND between the two boards, and 330 Ω in series with every signal wire. ' +
    'GPIO12 is forbidden on these boards: a high level on it at boot selects 1.8 V flash.'
} as const

/** What the "save" button proposes, and what Arduino insists on. */
export function folderFor(sketch: Sketch): string {
  return sketch.file.replace(/\.ino$/, '')
}
