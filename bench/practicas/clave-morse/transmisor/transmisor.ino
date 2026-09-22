/*
 * CLAVE MORSE - TRANSMISOR (placa A) (ESP32 WROOM, esp32:esp32:esp32)
 *
 * Que hace: lee una llave tactil en KEY_PIN y espeja su nivel EN VIVO hacia la
 * placa B por TX_DATA. No decodifica nada: la placa A solo es la llave. La
 * decodificacion Morse la hace el receptor (placa B).
 *
 * CABLEADO (el de N1 mas la llave):
 *      A GPIO26 (TX_DATA) --330R--> B GPIO25 (RX_DATA)
 *      A GND ------------------------ B GND            <-- OBLIGATORIO
 *      LLAVE: punta de jumper pelada en A GPIO13 (KEY_PIN)
 *             otra punta de jumper pelada a 3V3
 *             (opcional: 330 ohm en serie con la llave)
 *      LED testigo: GPIO4 -> 330 ohm -> LED -> GND
 *
 * ===========================================================================
 *  LA LLAVE ES METAL CONTRA METAL, NO EL DEDO
 * ===========================================================================
 *  Las dos puntas peladas se presionan una contra otra: los dedos solo dan la
 *  presion mecanica y el contacto electrico es metal-metal (~0 ohm), como un
 *  pulsador. Si en cambio cierras el circuito con la piel, el dedo es una
 *  resistencia variable (1 kohm mojado a >500 kohm seco) en un divisor contra el
 *  pull-down interno (~45 kohm): el nivel cae en la zona indefinida y cambia con
 *  la humedad de la mano.
 *
 * ANTIRREBOTE: el contacto metal-metal rebota unos ms al cerrar y al abrir. Un
 * cambio de lectura solo se acepta cuando se mantiene estable debounce_ms
 * (15 ms por defecto). Cada flanco sale retrasado el mismo tiempo, asi que las
 * duraciones que ve el receptor no cambian.
 *
 * Sin interrupciones ni delay(): el loop() vuelve a leer en microsegundos y la
 * temporizacion humana es de milisegundos, el sondeo sobra.
 *
 * GPIO12 esta PROHIBIDO (pin de strapping). Contrato: SPEC-MORSE.md.
 *
 * Serie: 115200 baudios. Salida: t_ms,nivel_tx por cada flanco aceptado.
 * Las lineas que empiezan con '#' son resumenes/comentarios.
 * Comandos (linea terminada en \n): d<ms> antirrebote, c poner contadores a
 * cero, r imprimir estado.
 */

#include <Arduino.h>

#define KEY_PIN 13        // llave tactil, INPUT_PULLDOWN; el otro extremo a 3V3
#define TX_DATA 26        // salida hacia RX_DATA (GPIO25) de la placa B
#define LED_PIN 4         // 2 = LED on-board

#define PERIODO_RESUMEN_MS 5000
#define DEBOUNCE_POR_DEFECTO_MS 15
#define DEBOUNCE_MAX_MS 200

static uint32_t debounce_ms = DEBOUNCE_POR_DEFECTO_MS;
static uint8_t estable = 0;        // nivel ya aceptado (el que sale por TX_DATA)
static uint8_t candidato = 0;      // ultima lectura cruda del pin
static uint32_t t_candidato = 0;   // desde cuando la lectura vale 'candidato'
static uint32_t crudos = 0;        // cambios de lectura ANTES del filtro
static uint32_t aceptados = 0;     // flancos que pasaron el filtro
static uint32_t crudos_resumen = 0;
static uint32_t aceptados_resumen = 0;
static uint32_t t_resumen = 0;

static char linea[16];
static uint8_t n_linea = 0;

static void imprimirEstado() {
  Serial.print("# umbrales debounce_ms=");
  Serial.println(debounce_ms);
  Serial.print("# contadores crudos=");
  Serial.print(crudos);
  Serial.print(" aceptados=");
  Serial.println(aceptados);
}

static void ejecutarComando(const char *cmd) {
  if (cmd[0] == 'r') {
    imprimirEstado();
  } else if (cmd[0] == 'c') {
    crudos = 0;
    aceptados = 0;
    Serial.println("# contadores a cero");
  } else if (cmd[0] == 'd') {
    long v = atol(cmd + 1);
    if (v < 0 || v > DEBOUNCE_MAX_MS) {
      Serial.println("# rechazado: d<ms> admite 0..200");
    } else {
      debounce_ms = (uint32_t)v;
      Serial.print("# debounce_ms=");
      Serial.println(debounce_ms);
    }
  }
}

static void leerSerie() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (n_linea > 0) {
        linea[n_linea] = '\0';
        ejecutarComando(linea);
        n_linea = 0;
      }
    } else if (n_linea < sizeof(linea) - 1) {
      linea[n_linea++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(KEY_PIN, INPUT_PULLDOWN);   // llave abierta = 0 definido, nunca flotante
  pinMode(TX_DATA, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(TX_DATA, LOW);
  digitalWrite(LED_PIN, LOW);
  estable = candidato = digitalRead(KEY_PIN);
  if (estable) {                      // arranco con la llave cerrada: reflejarlo
    digitalWrite(TX_DATA, HIGH);
    digitalWrite(LED_PIN, HIGH);
  }
  Serial.println("# transmisor morse listo");
  Serial.println("t_ms,nivel_tx");
  t_candidato = millis();
  t_resumen = millis();
}

void loop() {
  uint8_t lectura = digitalRead(KEY_PIN);
  uint32_t ahora = millis();

  if (lectura != candidato) {
    // Cambio de lectura (real o rebote): reinicia la cuenta de estabilidad.
    candidato = lectura;
    t_candidato = ahora;
    crudos++;
  } else if (candidato != estable && ahora - t_candidato >= debounce_ms) {
    // Estable el tiempo suficiente: es un flanco de verdad. Primero al cable.
    estable = candidato;
    digitalWrite(TX_DATA, estable);
    digitalWrite(LED_PIN, estable);
    aceptados++;
    Serial.print(ahora);
    Serial.print(',');
    Serial.println(estable);
  }

  if (ahora - t_resumen >= PERIODO_RESUMEN_MS) {
    t_resumen += PERIODO_RESUMEN_MS;
    if (crudos != crudos_resumen || aceptados != aceptados_resumen) {
      crudos_resumen = crudos;
      aceptados_resumen = aceptados;
      Serial.print("# resumen t_ms=");
      Serial.print(ahora);
      Serial.print(" crudos=");
      Serial.print(crudos);
      Serial.print(" aceptados=");
      Serial.print(aceptados);
      Serial.print(" nivel=");
      Serial.println(estable);
    }
  }

  leerSerie();
}
