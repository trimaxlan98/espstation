// Prueba en el host de la logica de transceptor.ino (mock del core; NO mide
// temporizacion real: eso se mide en el banco, ver README "Resultados").
//
// Lo que esta practica anade sobre clave-morse y hay que demostrar aqui:
//   - un solo sketch hace las dos mitades a la vez sin mezclarlas,
//   - los dos decodificadores tienen umbrales y contadores independientes,
//   - el antirrebote de la llave NO altera la duracion que ve el eco (ni el otro).
#include "Arduino.h"
#include TRANSCEPTOR_INO
#include <stdio.h>

static uint32_t T = 1000000;               // us
static int fallos = 0, pruebas = 0;

static void check(const char *n, bool ok) {
  pruebas++;
  if (!ok) { fallos++; printf("FALLO: %s\n", n); } else printf("ok:    %s\n", n);
}
static void limpiar() { Serial.out.clear(); }

// El transceptor imprime resumenes periodicos y verbose; para comparar la
// secuencia EXACTA de simbolos se quitan las lineas de diagnostico ('#').
static std::string sin_diag() {
  std::string r, o = Serial.out;
  size_t i = 0;
  while (i < o.size()) {
    size_t j = o.find('\n', i);
    if (j == std::string::npos) j = o.size();
    std::string l = o.substr(i, j - i);
    if (l.empty() || l[0] != '#') { r += l; r += "\n"; }
    i = j + 1;
  }
  return r;
}
static bool tiene(const char *s) { return Serial.out.find(s) != std::string::npos; }

// --- lado RX: flancos que "llegan por el cable" ------------------------------
static void rx_flanco(int nivel) {
  g_vt_us = T; GPIO.in = nivel ? MASK_RX : 0; g_isr(); loop();
}
static void rx_tocar(uint32_t dur_ms) { rx_flanco(1); T += dur_ms * 1000u; rx_flanco(0); }

// --- avance de tiempo: drena el buffer, filtra la llave y cierra letras ------
static void avanzar(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) { T += 1000u; g_vt_us = T; loop(); loop(); }
}
static void rx_tecla(const char *morse, uint32_t punto_ms = 100, uint32_t raya_ms = 500) {
  for (const char *p = morse; *p; p++) { rx_tocar(*p == '.' ? punto_ms : raya_ms); avanzar(100); }
}

// --- lado TX: mi propia llave ------------------------------------------------
static void llave(int nivel) { g_pin_lvl[KEY_PIN] = nivel; }
// Un toque de dur_ms: el antirrebote retrasa los DOS flancos lo mismo, asi que
// la duracion decodificada es dur_ms exacta (eso lo comprueba la prueba 7).
static void tx_tocar(uint32_t dur_ms) { llave(1); avanzar(dur_ms); llave(0); avanzar(30); }

static void cmd(const char *c) { Serial.in += c; Serial.in += "\n"; loop(); }

// Deja los DOS decodificadores sin nada pendiente. Sin esto, una letra a medio
// cerrar de la prueba anterior se cierra dentro de la siguiente y ensucia las
// comprobaciones de "no imprime nada" (paso mas de palabra_ms, el mayor umbral).
static void reposo() { avanzar(2500); Serial.out.clear(); }

int main(int argc, char **argv) {
  g_pin_lvl[KEY_PIN] = 0;
  GPIO.in = 0;
  g_vt_us = T;
  setup();

  if (argc > 1 && !strcmp(argv[1], "tabla")) {   // un codigo Morse por linea de stdin
    char buf[64];
    while (fgets(buf, sizeof buf, stdin)) {
      buf[strcspn(buf, "\n")] = 0;
      limpiar(); rx_tecla(buf); avanzar(1000);
      std::string o = Serial.out;
      size_t i = o.find("[letra");
      printf("%s|%s\n", buf, i == std::string::npos
             ? "SIN_LETRA" : o.substr(i, o.find('\n', i) - i).c_str());
    }
    return 0;
  }

  // 1. RX: SOS completo, prefijado, con [palabra] una sola vez
  limpiar();
  rx_tecla("..."); avanzar(800); rx_tecla("---"); avanzar(800); rx_tecla("..."); avanzar(3000);
  const char *esperado =
      "RX .\nRX .\nRX .\nRX [letra: S] [bin: 01010011]\n"
      "RX -\nRX -\nRX -\nRX [letra: O] [bin: 01001111]\n"
      "RX .\nRX .\nRX .\nRX [letra: S] [bin: 01010011]\nRX [palabra]\n";
  check("RX: SOS exacto con prefijo, [palabra] una sola vez", sin_diag() == esperado);
  if (sin_diag() != esperado) printf("--- obtenido:\n%s---\n", sin_diag().c_str());

  // 2. TX: mi llave sale al cable Y la decodifica el eco local
  cmd("c"); limpiar(); g_txlog.clear();
  tx_tocar(100); avanzar(1000);
  check("TX: un toque corto da 'TX .' y letra E", sin_diag() == "TX .\nTX [letra: E] [bin: 01000101]\n");
  int n_tx = 0;
  for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx++;
  check("TX_DATA escrito 2 veces (sube y baja, sin rebotes)", n_tx == 2);

  // 3. LOS DOS SENTIDOS A LA VEZ: no se mezclan los simbolos.
  //    Mi llave manda una raya larga mientras por el cable entran tres puntos.
  cmd("c"); limpiar();
  llave(1); avanzar(20);                       // flanco de subida de mi llave aceptado
  rx_tocar(100); avanzar(120);                 // punto del otro
  rx_tocar(100); avanzar(120);                 // punto del otro
  rx_tocar(100); avanzar(120);                 // punto del otro
  llave(0); avanzar(30);                       // mi bajada: raya de ~710 ms
  avanzar(1000);                               // cierra letra en los dos sentidos
  check("duplex: RX decodifica S", tiene("RX [letra: S] [bin: 01010011]"));
  check("duplex: TX decodifica T", tiene("TX [letra: T] [bin: 01010100]"));
  check("duplex: RX no se queda con mi raya", !tiene("RX -"));
  check("duplex: TX no se queda con sus puntos", !tiene("TX ."));
  limpiar(); cmd("r");
  check("duplex: contadores RX = 3 puntos, 0 rayas",
        tiene("contadores RX puntos=3 rayas=0"));
  check("duplex: contadores TX = 0 puntos, 1 raya",
        tiene("contadores TX puntos=0 rayas=1"));

  // 4. Umbrales independientes: p cambia RX y deja TX intacto
  limpiar(); cmd("p150");
  check("p150 aplica a RX", tiene("# umbrales RX punto_raya_ms=150"));
  limpiar(); cmd("r");
  check("p150 NO toca TX (sigue en 300)", tiene("# umbrales TX punto_raya_ms=300"));
  limpiar(); cmd("tp150");
  check("tp150 aplica al eco TX", tiene("# umbrales TX punto_raya_ms=150"));
  cmd("p300"); cmd("tp300");

  // 5. El umbral de RX decide sobre la mano del otro, no sobre la mia
  cmd("p200"); cmd("c"); limpiar();
  rx_tocar(250); avanzar(1000);
  check("p200: un pulso RX de 250 ms es raya (T)", tiene("RX [letra: T]"));
  cmd("c"); limpiar();
  tx_tocar(250); avanzar(1000);
  check("p200 no afecta al eco: 250 ms sigue siendo punto (E)", tiene("TX [letra: E]"));
  cmd("p300");

  // 6. Eco local apagable: la llave sigue saliendo al cable, pero no se imprime
  reposo(); cmd("e"); cmd("c"); limpiar(); g_txlog.clear();
  tx_tocar(100); avanzar(1000);
  check("eco off: no hay lineas TX", sin_diag().empty());
  n_tx = 0;
  for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx++;
  check("eco off: el cable SIGUE llevando la llave", n_tx == 2);
  cmd("e");

  // 7. El antirrebote no altera la duracion: 200 ms de llave -> 200 ms medidos
  cmd("v"); cmd("c"); limpiar();
  tx_tocar(200); avanzar(1000);
  check("verbose: '# TX pulso_ms=200' exacto pese al debounce de 15 ms",
        tiene("# TX pulso_ms=200"));
  limpiar(); rx_tocar(180); avanzar(1000);
  check("verbose: '# RX pulso_ms=180'", tiene("# RX pulso_ms=180"));
  check("verbose: silencio etiquetado por sentido", tiene("# RX silencio_ms="));
  cmd("v");

  // 8. Rebotes de la llave: 8 cambios crudos, 2 aceptados, 1 solo simbolo
  cmd("c"); limpiar(); g_txlog.clear();
  llave(1); avanzar(2); llave(0); avanzar(2); llave(1); avanzar(2);
  llave(0); avanzar(2); llave(1); avanzar(200);
  llave(0); avanzar(3); llave(1); avanzar(2); llave(0); avanzar(400);
  limpiar(); cmd("r");
  check("llave: 8 cambios crudos, 2 aceptados", tiene("# llave debounce_ms=15 crudos=8 aceptados=2"));
  n_tx = 0;
  for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx++;
  check("llave: los rebotes no llegan al cable (2 escrituras)", n_tx == 2);

  // 9. k0 desactiva el filtro: los rebotes SI pasan al cable (contraste)
  cmd("k0"); cmd("c"); limpiar(); g_txlog.clear();
  llave(1); avanzar(2); llave(0); avanzar(2); llave(1); avanzar(200);
  llave(0); avanzar(400);
  n_tx = 0;
  for (size_t p = 0; (p = g_txlog.find(":26=", p)) != std::string::npos; p++) n_tx++;
  check("k0: sin filtro los rebotes llegan al cable (4 escrituras)", n_tx == 4);
  cmd("k15");
  limpiar(); cmd("k500"); check("k500 rechazado", tiene("rechazado"));

  // 10. Ruido por debajo de debounce_ms en RX: se filtra, no genera simbolo
  reposo(); cmd("c"); limpiar(); rx_tocar(5); avanzar(1000);
  check("RX: pulso de 5 ms sin simbolo", sin_diag().empty());
  limpiar(); cmd("r"); check("RX: filtrados=1", tiene("contadores RX puntos=0 rayas=0 letras=0 desconocidas=0 filtrados=1"));

  // 11. Codigo desconocido y desbordado, por sentido
  cmd("c"); limpiar(); rx_tecla("......."); avanzar(1000);
  check("RX: codigo desconocido .......", tiene("RX [letra: ?] [morse: .......]"));
  limpiar(); rx_tecla("........."); avanzar(1000);
  check("RX: mas de 8 pulsos -> desbordado (+)", tiene("RX [letra: ?] [morse: ........+]"));

  // 12. Rechazos de coherencia, por sentido
  limpiar(); cmd("l2000"); check("l2000 rechazado (>= palabra_ms de RX)", tiene("rechazado"));
  limpiar(); cmd("w400"); check("w400 rechazado (<= letra_ms de RX)", tiene("rechazado"));
  limpiar(); cmd("tl2000"); check("tl2000 rechazado (>= palabra_ms de TX)", tiene("rechazado"));
  limpiar(); cmd("l500"); check("l500 aceptado en RX", tiene("# umbrales RX punto_raya_ms=300 letra_ms=500"));
  cmd("l700");
  limpiar(); cmd("z9"); check("comando desconocido avisa", tiene("# rechazado: comando desconocido z9"));

  // 13. Niveles repetidos (la ISR leyo la linea cuando ya habia vuelto)
  cmd("c"); limpiar();
  T += 1000; g_vt_us = T; GPIO.in = MASK_RX; g_isr(); g_isr(); loop();
  T += 50000; g_vt_us = T; GPIO.in = 0; g_isr(); loop();
  limpiar(); cmd("r"); check("RX: niveles_repetidos=1", tiene("niveles_repetidos=1"));

  // 14. Desbordamiento del buffer de la ISR
  cmd("c"); limpiar();
  for (int i = 0; i < 100; i++) { T += 1000; g_vt_us = T; GPIO.in = (i & 1) ? 0 : MASK_RX; g_isr(); }
  loop(); avanzar(3000);
  limpiar(); cmd("r");
  check("desbordes_buffer > 0 con 100 flancos sin drenar", !tiene("desbordes_buffer=0"));
  check("flancos_crudos=100 contados aunque se descarten", tiene("flancos_crudos=100"));

  printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
  return fallos ? 1 : 0;
}
