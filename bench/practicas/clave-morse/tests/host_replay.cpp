// Reproduce una secuencia de pulsos a traves del receptor.ino REAL (mock del core, tiempo virtual
// de 1 ms por vuelta de loop()) e imprime lo que B imprimiria por serie (sin lineas '#').
// Entrada (stdin): "p l w d" y luego una linea "silencio_ms dur_ms" por pulso (el silencio del 1o se ignora).
#include "Arduino.h"
#include RECEPTOR_INO
#include <stdio.h>
static uint32_t T = 1000000;  // us
static void flanco(int nivel) { g_vt_us = T; GPIO.in = nivel ? (1UL << 25) : 0; g_isr(); loop(); }
static void avanzar(long ms) { for (long i = 0; i < ms; i++) { T += 1000; g_vt_us = T; loop(); } }
int main() {
  long p, l, w, d;
  if (scanf("%ld %ld %ld %ld", &p, &l, &w, &d) != 4) return 2;
  setup();
  Serial.out.clear();
  char b[64];
  snprintf(b, sizeof b, "w60000\nl%ld\nw%ld\np%ld\nd%ld\n", l, w, p, d);
  Serial.in = b; loop(); Serial.out.clear();
  long sil, dur; bool primero = true;
  while (scanf("%ld %ld", &sil, &dur) == 2) {
    if (!primero) avanzar(sil);
    primero = false;
    flanco(1); avanzar(dur); flanco(0);
  }
  avanzar(70000);  // cierra la letra y la palabra pendientes
  fputs(Serial.out.c_str(), stdout);
  return 0;
}
