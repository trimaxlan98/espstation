// Prueba en el host de la logica de transmisor.ino (mock del core; NO mide temporizacion real).
#include "Arduino.h"
#include TRANSMISOR_INO
#include <stdio.h>
static int fallos = 0, pruebas = 0;
static void check(const char *n, bool ok) { pruebas++; if (!ok) { fallos++; printf("FALLO: %s\n", n); } else printf("ok:    %s\n", n); }
// Avanza el tiempo ms a ms; en cada ms el pin vale lo que diga el guion; 3 vueltas de loop() por ms.
struct Paso { uint32_t t_ms; int nivel; };
static void correr(const Paso *g, int n, uint32_t hasta_ms) {
  int i = 0;
  for (uint32_t ms = g_vt_us / 1000u; ms <= hasta_ms; ms++) {
    while (i < n && g[i].t_ms <= ms) g_pin_lvl[KEY_PIN] = g[i++].nivel;
    g_vt_us = ms * 1000u;
    for (int k = 0; k < 3; k++) loop();
  }
}
static void reiniciar() { Serial.out.clear(); Serial.in.clear(); Serial.rd = 0; g_txlog.clear(); }
int main() {
  g_vt_us = 5000000; g_pin_lvl[KEY_PIN] = 0; setup(); reiniciar();
  // Toque de 200 ms con rebotes al cerrar (5 cambios) y al abrir (3 cambios), t relativo a 5000 ms
  const uint32_t t0 = 5010;
  Paso g[] = { {t0,1},{t0+2,0},{t0+4,1},{t0+6,0},{t0+8,1},                 // cierre con rebote; estable en 1 desde t0+8
               {t0+200,0},{t0+203,1},{t0+205,0} };                          // apertura con rebote; estable en 0 desde t0+205
  correr(g, 8, t0 + 400);

  Serial.in = "r\n"; loop();
  check("debounce 15: 8 cambios crudos, solo 2 aceptados", Serial.out.find("crudos=8 aceptados=2") != std::string::npos);
  // TX_DATA cambia SOLO tras el tiempo de estabilidad: sube en t0+8+15=5033, baja en t0+205+15=5230
  bool sube = g_txlog.find("5033:26=1;") != std::string::npos;
  bool baja = g_txlog.find("5230:26=0;") != std::string::npos;
  check("TX sube a los 15 ms de la ultima lectura estable (5033)", sube);
  check("TX baja a los 15 ms de la ultima lectura estable (5230)", baja);
  int n_tx = 0; for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx++;
  check("TX_DATA escrito exactamente 2 veces (sin rebotes en el cable)", n_tx == 2);
  // duracion transmitida = 5230 - 5033 = 197 ms (la real, 200 ms, menos el rebote de apertura): el retardo es igual en ambos flancos
  // Contraste d0: sin filtro los rebotes pasan al cable
  reiniciar(); Serial.in = "d0\nc\n"; loop(); reiniciar();
  const uint32_t t1 = 6000;
  Paso h[] = { {t1,1},{t1+2,0},{t1+4,1},{t1+6,0},{t1+8,1},{t1+200,0},{t1+203,1},{t1+205,0} };
  g_vt_us = 5900000; correr(h, 8, t1 + 400);
  int n_tx0 = 0; for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx0++;
  check("d0: los rebotes SI llegan al cable (8 escrituras)", n_tx0 == 8);
  // Un rebote de 10 ms (revierte antes de 15 ms) no llega al cable con debounce 15
  reiniciar(); Serial.in = "d15\nc\n"; loop(); reiniciar();
  Paso r[] = { {7000,1},{7010,0} }; g_vt_us = 6900000; correr(r, 2, 7100);
  check("glitch de 10 ms con debounce 15: 0 escrituras en TX", g_txlog.find(":26=") == std::string::npos);
  // Rango de d<ms>
  reiniciar(); Serial.in = "d500\n"; loop(); check("d500 rechazado", Serial.out.find("rechazado") != std::string::npos);
  printf("\n%d pruebas, %d fallos\n", pruebas, fallos); return fallos ? 1 : 0;
}
