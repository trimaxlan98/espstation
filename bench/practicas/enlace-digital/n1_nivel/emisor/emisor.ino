/*
 * N1 - EMISOR: espejo de estado de 1 bit (ESP32 WROOM, esp32:esp32:esp32)
 *
 * Que hace: conmuta la salida TX_DATA cada 500 ms (onda cuadrada de 1 Hz),
 * refleja ese mismo nivel en un LED propio y lo imprime por serie.
 * El receptor (ver ../receptor/receptor.ino) debe reproducir el nivel.
 *
 * CABLEADO (tres cables, no dos):
 *
 *      EMISOR                      RECEPTOR
 *      GPIO26 (TX_DATA) --330R--> GPIO25 (RX_DATA)
 *      GND --------------------------- GND        <-- OBLIGATORIO
 *
 *   LED testigo: GPIO4 -> 330 ohm -> LED -> GND   (en cada placa)
 *
 * GND COMUN: un nivel logico es una TENSION medida contra una referencia.
 * Si las dos placas no comparten GND, el receptor no tiene contra que medir
 * "3.3 V" y lo que lea sera ruido. Sin el cable de GND el enlace "funciona a
 * veces" (por las capacidades parasitas), que es peor que no funcionar.
 *
 * GPIO12 esta PROHIBIDO en este banco: es un pin de strapping (MTDI) que
 * selecciona la tension de la flash al arrancar. Un nivel alto ahi en el reset
 * puede dejar la placa sin arrancar.
 *
 * Serie: 115200 baudios, salida CSV: t_ms,nivel_tx
 */

#include <Arduino.h>

#define TX_DATA 26        // salida de datos hacia RX_DATA (GPIO25) de la otra placa
#define LED_PIN 4         // 2 = LED on-board
#define MEDIO_PERIODO_MS 500

static uint8_t nivel = 0;
static uint32_t proximo_ms = 0;

void setup() {
  Serial.begin(115200);
  pinMode(TX_DATA, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(TX_DATA, LOW);
  digitalWrite(LED_PIN, LOW);
  Serial.println("t_ms,nivel_tx");
  proximo_ms = millis() + MEDIO_PERIODO_MS;
}

void loop() {
  // Se compara contra un instante absoluto y se le SUMA el periodo: si este
  // ciclo se retrasa unos ms, el siguiente lo compensa y no se acumula deriva
  // (con delay(500) el periodo real seria 500 ms + tiempo de impresion).
  if ((int32_t)(millis() - proximo_ms) >= 0) {
    proximo_ms += MEDIO_PERIODO_MS;
    nivel ^= 1;
    digitalWrite(TX_DATA, nivel);
    digitalWrite(LED_PIN, nivel);
    Serial.print(millis());
    Serial.print(',');
    Serial.println(nivel);
  }
}
