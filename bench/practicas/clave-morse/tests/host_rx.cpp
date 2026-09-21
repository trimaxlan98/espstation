// Prueba en el host de la logica de receptor.ino (mock del core; NO mide temporizacion real).
#include "Arduino.h"
#include RECEPTOR_INO
#include <stdio.h>
static uint32_t T = 1000000;               // us
static int fallos = 0, pruebas = 0;
static void flanco(int nivel) {
  g_vt_us = T; GPIO.in = nivel ? (1UL << 25) : 0; g_isr(); loop();
}
static void tocar(uint32_t dur_ms) { flanco(1); T += dur_ms * 1000u; flanco(0); }
static void silencio(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) { T += 10000; g_vt_us = T; loop(); } }
static void limpiar() { Serial.out.clear(); }
static void check(const char *n, bool ok) { pruebas++; if (!ok) { fallos++; printf("FALLO: %s\n", n); } else printf("ok:    %s\n", n); }
static void tecla(const char *morse, uint32_t punto_ms = 100, uint32_t raya_ms = 500) {
  for (const char *p = morse; *p; p++) { tocar(*p == '.' ? punto_ms : raya_ms); silencio(100); }
}
static void cmd(const char *c) { Serial.in += c; Serial.in += "\n"; loop(); }
int main(int argc, char **argv) {
  setup();
  if (argc > 1 && !strcmp(argv[1], "tabla")) {      // modo tabla: un codigo por linea de stdin
    char buf[64];
    while (fgets(buf, sizeof buf, stdin)) {
      buf[strcspn(buf, "\n")] = 0; limpiar(); tecla(buf); silencio(1000);
      std::string o = Serial.out; size_t i = o.find("[letra");
      printf("%s|%s\n", buf, i == std::string::npos ? "SIN_LETRA" : o.substr(i, o.find('\n', i) - i).c_str());
    }
    return 0;
  }
  // 1. SOS exacto, con [palabra] una sola vez pese a muchas vueltas de loop()
  limpiar(); tecla("..."); silencio(800); tecla("---"); silencio(800); tecla("..."); silencio(3000);
  const char *esperado = ".\n.\n.\n[letra: S] [bin: 01010011]\n-\n-\n-\n[letra: O] [bin: 01001111]\n.\n.\n.\n[letra: S] [bin: 01010011]\n[palabra]\n";
  check("SOS: salida exacta, [palabra] una sola vez", Serial.out == esperado);
  if (Serial.out != esperado) printf("--- obtenido:\n%s---\n", Serial.out.c_str());
  // 2. dos palabras: E, hueco largo, E
  limpiar(); tecla("."); silencio(3000); tecla("."); silencio(3000);
  check("E [palabra] E [palabra]", Serial.out == ".\n[letra: E] [bin: 01000101]\n[palabra]\n.\n[letra: E] [bin: 01000101]\n[palabra]\n");
  // 3. pulso de ruido < debounce se filtra y no genera simbolo
  cmd("c"); limpiar(); tocar(5); silencio(3000);
  check("pulso de 5 ms filtrado, sin simbolo ni letra", Serial.out == "# contadores a cero\n" || Serial.out.find("[letra") == std::string::npos);
  cmd("r"); check("contador filtrados=1", Serial.out.find("filtrados=1") != std::string::npos);
  // 4. silencio sin simbolo pendiente no imprime nada
  limpiar(); silencio(5000); check("silencio sin simbolos no imprime", Serial.out.empty());
  // 5. simbolo desconocido y desbordado
  limpiar(); tecla("......."); silencio(1000);
  check("codigo desconocido ......." , Serial.out.find("[letra: ?] [morse: .......]") != std::string::npos);
  limpiar(); tecla("........."); silencio(1000);
  check("mas de 8 pulsos -> desbordado (+)", Serial.out.find("[letra: ?] [morse: ........+]") != std::string::npos);
  // 6. umbral punto/raya: 299 -> punto, 300 -> raya, 250 con p200 -> raya
  limpiar(); tocar(299); silencio(1000); check("299 ms = punto", Serial.out.find("[letra: E]") != std::string::npos);
  limpiar(); tocar(300); silencio(1000); check("300 ms = raya", Serial.out.find("[letra: T]") != std::string::npos);
  cmd("p200"); limpiar(); tocar(250); silencio(1000); check("p200: 250 ms = raya", Serial.out.find("[letra: T]") != std::string::npos);
  cmd("p300");
  // 7. comandos: rechazos y aceptaciones
  limpiar(); cmd("l2000"); check("l2000 rechazado (>= palabra)", Serial.out.find("rechazado") != std::string::npos);
  limpiar(); cmd("w400"); check("w400 rechazado (<= letra)", Serial.out.find("rechazado") != std::string::npos);
  limpiar(); cmd("l500"); check("l500 aceptado", Serial.out.find("letra_ms=500") != std::string::npos);
  cmd("l700");
  limpiar(); cmd("p0"); check("p0 rechazado", Serial.out.find("rechazado") != std::string::npos);
  // 8. niveles repetidos (la ISR lee la linea cuando ya volvio)
  cmd("c"); limpiar();
  T += 1000; g_vt_us = T; GPIO.in = 1UL << 25; g_isr(); g_isr(); loop();   // dos eventos, mismo nivel 1
  T += 50000; g_vt_us = T; GPIO.in = 0; g_isr(); loop();
  cmd("r"); check("niveles_repetidos=1", Serial.out.find("niveles_repetidos=1") != std::string::npos);
  // 9. desbordamiento del buffer: 100 flancos sin drenar
  cmd("c"); limpiar();
  for (int i = 0; i < 100; i++) { T += 1000; g_vt_us = T; GPIO.in = (i & 1) ? 0 : (1UL << 25); g_isr(); }
  loop(); silencio(3000); cmd("r");
  check("desbordes_buffer > 0 con 100 flancos sin drenar", Serial.out.find("desbordes_buffer=0") == std::string::npos);
  check("flancos_crudos=100 contados aunque se descarten", Serial.out.find("flancos_crudos=100") != std::string::npos);
  // 10. verbose imprime duraciones
  cmd("c"); cmd("v"); limpiar(); tocar(120); silencio(200); tocar(120); silencio(1000);
  check("verbose: # pulso_ms=120 y # silencio_ms=", Serial.out.find("# pulso_ms=120") != std::string::npos && Serial.out.find("# silencio_ms=") != std::string::npos);
  printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
  return fallos ? 1 : 0;
}
