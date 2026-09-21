/*
 * N1 - RECEPTOR: espejo de estado de 1 bit (ESP32 WROOM, esp32:esp32:esp32)
 *
 * Que hace: lee RX_DATA, enciende su LED con el nivel recibido y registra por
 * serie cada cambio con su marca de tiempo. Marca ANOMALIA cuando el intervalo
 * entre cambios se sale de 500 +/- 50 ms (el emisor conmuta cada 500 ms) y cada
 * 5 s imprime un resumen.
 *
 * CABLEADO (tres cables):
 *      EMISOR GPIO26 --330R--> RECEPTOR GPIO25 (RX_DATA)
 *      EMISOR GND ------------ RECEPTOR GND      <-- OBLIGATORIO
 *      LED testigo: GPIO4 -> 330 ohm -> LED -> GND
 *
 * ===========================================================================
 *  POR QUE INPUT_PULLDOWN Y NO INPUT
 * ===========================================================================
 *  Un pin en INPUT "sin resistencia" esta FLOTANTE: no vale 0, no vale 1, vale
 *  lo que la interferencia y las capacidades parasitas quieran (el dedo, el
 *  cable, la red electrica). Un pin flotante NO es un 0.
 *  Ningun delay() lo arregla: esperar mas tiempo solo te da mas tiempo de
 *  leer ruido. La solucion es darle un nivel de reposo definido con una
 *  resistencia interna (~45 kohm) hacia GND: INPUT_PULLDOWN. Asi, si el cable
 *  se desconecta, el pin queda en 0 ESTABLE, y el emisor puede manejar la linea
 *  a 1 sin problema (la resistencia es muy debil frente a un GPIO).
 *
 *  COMO PROBARLO: con el enlace funcionando, DESCONECTA el cable de senal
 *  (GPIO26 -> GPIO25) dejando el GND conectado. El receptor debe quedarse en
 *  nivel 0 estable (sin lineas de cambio en el CSV, LED apagado). Si en el
 *  sketch original (INPUT) haces lo mismo, veras cambios aleatorios.
 *
 * ===========================================================================
 *  GND COMUN
 * ===========================================================================
 *  Sin GND compartido las tensiones de las dos placas no tienen referencia
 *  comun y la lectura no es fiable, aunque "a veces" parezca funcionar.
 *
 *  GPIO12 esta PROHIBIDO (pin de strapping, puede impedir el arranque).
 *
 * Serie: 115200 baudios. Salida CSV: t_ms,nivel_rx,dt_ms[,ANOMALIA]
 * Las lineas que empiezan con '#' son resumenes/comentarios.
 */

#include <Arduino.h>

#define RX_DATA 25        // entrada de datos, viene de TX_DATA (GPIO26) del emisor
#define LED_PIN 4         // 2 = LED on-board

#define PERIODO_ESPERADO_MS 500
#define TOLERANCIA_MS 50
#define PERIODO_RESUMEN_MS 5000

static uint8_t nivel_prev = 0;
static uint32_t t_cambio_prev = 0;
static bool hay_cambio_prev = false;   // el primer cambio no tiene "anterior"
static uint32_t cambios = 0;
static uint32_t anomalias = 0;
static uint32_t t_resumen = 0;

void setup() {
  Serial.begin(115200);
  pinMode(RX_DATA, INPUT_PULLDOWN);   // ver comentario destacado arriba
  pinMode(LED_PIN, OUTPUT);
  nivel_prev = digitalRead(RX_DATA);
  digitalWrite(LED_PIN, nivel_prev);
  Serial.println("t_ms,nivel_rx,dt_ms");
  t_resumen = millis();
}

void loop() {
  // Sondeo continuo: el loop vacio pasa por aqui miles de veces por segundo,
  // sobra resolucion para un cambio cada 500 ms (millis() tiene 1 ms).
  uint8_t nivel = digitalRead(RX_DATA);
  if (nivel != nivel_prev) {
    uint32_t t = millis();          // sellar el tiempo ANTES de imprimir
    nivel_prev = nivel;
    digitalWrite(LED_PIN, nivel);
    cambios++;

    Serial.print(t);
    Serial.print(',');
    Serial.print(nivel);
    Serial.print(',');
    if (hay_cambio_prev) {
      uint32_t dt = t - t_cambio_prev;
      Serial.print(dt);
      if (dt < PERIODO_ESPERADO_MS - TOLERANCIA_MS ||
          dt > PERIODO_ESPERADO_MS + TOLERANCIA_MS) {
        anomalias++;
        Serial.print(",ANOMALIA");
      }
    } else {
      Serial.print(0);   // primer cambio: no hay intervalo previo que medir
    }
    Serial.println();
    t_cambio_prev = t;
    hay_cambio_prev = true;
  }

  if (millis() - t_resumen >= PERIODO_RESUMEN_MS) {
    t_resumen += PERIODO_RESUMEN_MS;
    Serial.print("# resumen t_ms=");
    Serial.print(millis());
    Serial.print(" cambios=");
    Serial.print(cambios);
    Serial.print(" anomalias=");
    Serial.print(anomalias);
    Serial.print(" nivel=");
    Serial.println(nivel_prev);
  }
}
