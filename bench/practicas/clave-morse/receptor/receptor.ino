/*
 * CLAVE MORSE - RECEPTOR (placa B) (ESP32 WROOM, esp32:esp32:esp32)
 *
 * Que hace: mide la duracion de cada pulso que llega por RX_DATA, arma puntos y
 * rayas, decodifica letras con una tabla Morse y las imprime como TEXTO y como su
 * BYTE ASCII en binario (8 bits, con ceros a la izquierda). La placa A es la
 * llave (transmisor.ino); esta placa nunca lee la llave.
 *
 * CABLEADO (el de N1):
 *      A GPIO26 --330R--> B GPIO25 (RX_DATA)
 *      A GND ------------ B GND                <-- OBLIGATORIO
 *      LED testigo: GPIO4 -> 330 ohm -> LED -> GND  (nivel crudo recibido)
 *
 * COMO FUNCIONA (contrato en SPEC-MORSE.md):
 *  - Como N2: attachInterrupt(RX_DATA, CHANGE). La ISR SOLO sella micros(), lee
 *    el nivel del registro GPIO.in y lo mete en un buffer circular. NO imprime ni
 *    decodifica: un Serial.print dentro de una ISR bloquea o corrompe.
 *  - loop() drena el buffer y corre la maquina de estados:
 *      subida  -> guarda t_subida
 *      bajada  -> dur = t_bajada - t_subida; < punto_raya_ms es '.', si no '-'
 *                 (se imprime al decidirlo: el operador ve el simbolo mientras
 *                 teclea); un pulso < debounce_ms se descarta como ruido
 *      silencio -> >= letra_ms decodifica la letra; >= palabra_ms imprime
 *                 [palabra] UNA sola vez (bandera de "ya reportado", si no se
 *                 repetiria en cada vuelta del loop)
 *
 * INPUT_PULLDOWN en RX_DATA: si el cable se suelta, la linea queda en 0 estable.
 * GPIO12 esta PROHIBIDO (pin de strapping).
 *
 * Serie: 115200 baudios. Comandos (linea terminada en \n, ms):
 *      p<ms> punto/raya   l<ms> fin de letra   w<ms> fin de palabra
 *      d<ms> filtro de ruido   v verbose (duraciones)   c contadores a cero
 *      r imprimir umbrales y contadores
 * Las lineas que empiezan con '#' son diagnostico.
 */

#include <Arduino.h>
#include "soc/gpio_struct.h"      // registro GPIO.in, como en N2 y N3

#define RX_DATA 25       // entrada de datos, viene de TX_DATA (GPIO26) de A
#define LED_PIN 4         // 2 = LED on-board
#define MASK_RX (1UL << RX_DATA)   // GPIO25 < 32: se lee de GPIO.in, como en N2

#define N_FLANCOS 64      // buffer circular ISR -> loop (potencia de 2 no requerida)
#define MAX_SIMBOLO 8     // puntos/rayas maximos por letra

// --- Umbrales (calibrar en el banco con un operador real) --------------------
static uint32_t punto_raya_ms = 300;   // < esto = punto, >= esto = raya
static uint32_t letra_ms = 700;        // ms de silencio = fin de letra
static uint32_t palabra_ms = 1800;     // ms de silencio = fin de palabra
static uint32_t debounce_ms = 15;      // pulsos mas cortos = ruido, se ignoran
static bool verbose = false;

// --- Tabla Morse: datos, no una cadena de if/else ----------------------------
struct Morse { const char *codigo; char c; };
static const Morse TABLA[] = {
  {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
  {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
  {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
  {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
  {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
  {"--..", 'Z'},
  {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
  {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
};
#define N_TABLA (sizeof(TABLA) / sizeof(TABLA[0]))

// --- Compartido con la ISR: volatile; la ISR no imprime ni reserva memoria ---
struct Flanco { uint32_t t_us; uint8_t nivel; };
static volatile Flanco g_buf[N_FLANCOS];
static volatile uint8_t g_head = 0;          // lo escribe SOLO la ISR
static volatile uint8_t g_tail = 0;          // lo escribe SOLO loop()
static volatile uint32_t g_crudos = 0;       // flancos que entraron en la ISR
static volatile uint32_t g_desbordes = 0;    // flancos perdidos por buffer lleno

void IRAM_ATTR isrFlanco() {
  uint32_t t = micros();                               // sellar el tiempo primero
  uint8_t nivel = (GPIO.in & MASK_RX) ? 1 : 0;
  g_crudos = g_crudos + 1;   // (++ sobre volatile esta obsoleto y avisa)
  uint8_t sig = (uint8_t)((g_head + 1) % N_FLANCOS);
  if (sig == g_tail) {                                 // lleno: contar y descartar
    g_desbordes = g_desbordes + 1;
    return;
  }
  g_buf[g_head].t_us = t;
  g_buf[g_head].nivel = nivel;
  g_head = sig;
}

// --- Estado de la maquina de estados (solo loop() lo toca) -------------------
static uint8_t nivel_ant = 0;          // ultimo nivel ya procesado
static bool en_pulso = false;          // hubo una subida y aun no su bajada
static uint32_t t_subida_us = 0;
static uint32_t t_bajada_us = 0;       // fin del ultimo pulso valido
static bool hay_bajada = false;        // t_bajada_us es valido
static bool midiendo_silencio = false; // aun falta decidir letra o palabra
static char simbolo[MAX_SIMBOLO + 1];
static uint8_t n_simbolo = 0;
static bool desbordado = false;        // llegaron mas de MAX_SIMBOLO pulsos
static bool letra_desde_palabra = false;  // hubo letra desde el ultimo [palabra]

static uint32_t filtrados = 0, puntos = 0, rayas = 0, letras = 0;
static uint32_t desconocidas = 0, repetidos = 0;

static char linea[16];
static uint8_t n_linea = 0;

static void imprimirBin8(uint8_t v) {
  // 8 bits SIEMPRE: Serial.print(c, BIN) no rellena ('A' saldria de 7 bits).
  for (int i = 7; i >= 0; i--) Serial.print((v >> i) & 1);
}

static void imprimirUmbrales() {
  Serial.print("# umbrales punto_raya_ms=");
  Serial.print(punto_raya_ms);
  Serial.print(" letra_ms=");
  Serial.print(letra_ms);
  Serial.print(" palabra_ms=");
  Serial.print(palabra_ms);
  Serial.print(" debounce_ms=");
  Serial.print(debounce_ms);
  Serial.print(" verbose=");
  Serial.println(verbose ? 1 : 0);
}

static void imprimirContadores() {
  Serial.print("# contadores flancos_crudos=");
  Serial.print(g_crudos);
  Serial.print(" filtrados=");
  Serial.print(filtrados);
  Serial.print(" puntos=");
  Serial.print(puntos);
  Serial.print(" rayas=");
  Serial.print(rayas);
  Serial.print(" letras=");
  Serial.print(letras);
  Serial.print(" desconocidas=");
  Serial.print(desconocidas);
  Serial.print(" desbordes_buffer=");
  Serial.print(g_desbordes);
  Serial.print(" niveles_repetidos=");
  Serial.println(repetidos);
}

static void cerrarLetra() {
  simbolo[n_simbolo] = '\0';
  char c = '?';
  bool hallada = false;
  for (size_t i = 0; i < N_TABLA; i++) {
    if (strcmp(TABLA[i].codigo, simbolo) == 0) {
      c = TABLA[i].c;
      hallada = true;
      break;
    }
  }
  if (hallada && !desbordado) {
    letras++;
    Serial.print("[letra: ");
    Serial.print(c);
    Serial.print("] [bin: ");
    imprimirBin8((uint8_t)c);
    Serial.println("]");
  } else {
    desconocidas++;
    Serial.print("[letra: ?] [morse: ");
    Serial.print(simbolo);
    Serial.println(desbordado ? "+]" : "]");
  }
  n_simbolo = 0;
  desbordado = false;
  letra_desde_palabra = true;
}

static void procesarFlanco(uint8_t nivel, uint32_t t_us) {
  if (nivel == nivel_ant) {   // la ISR lo leyo cuando la linea ya habia vuelto
    repetidos++;
    return;
  }
  nivel_ant = nivel;
  digitalWrite(LED_PIN, nivel);   // LED = nivel crudo recibido

  if (nivel) {                    // subida
    if (verbose && hay_bajada) {
      Serial.print("# silencio_ms=");
      Serial.println((uint32_t)(t_us - t_bajada_us) / 1000UL);
    }
    t_subida_us = t_us;
    en_pulso = true;
    return;
  }

  // bajada
  if (!en_pulso) return;          // arranque con la llave ya cerrada: sin subida
  en_pulso = false;
  uint32_t dur_ms = (uint32_t)(t_us - t_subida_us) / 1000UL;

  if (dur_ms < debounce_ms) {     // ruido: no es simbolo ni interrumpe el silencio
    filtrados++;
    if (verbose) {
      Serial.print("# pulso_ms=");
      Serial.print(dur_ms);
      Serial.println(" filtrado");
    }
    return;
  }

  char s = (dur_ms < punto_raya_ms) ? '.' : '-';
  if (s == '.') puntos++; else rayas++;
  if (n_simbolo < MAX_SIMBOLO) simbolo[n_simbolo++] = s; else desbordado = true;
  Serial.println(s);              // feedback inmediato, al decidirlo
  if (verbose) {
    Serial.print("# pulso_ms=");
    Serial.println(dur_ms);
  }
  t_bajada_us = t_us;
  hay_bajada = true;
  midiendo_silencio = true;
}

static void revisarSilencio() {
  if (!midiendo_silencio || nivel_ant != 0) return;
  uint32_t sil_ms = (uint32_t)(micros() - t_bajada_us) / 1000UL;
  if (n_simbolo > 0 && sil_ms >= letra_ms) cerrarLetra();
  if (letra_desde_palabra && sil_ms >= palabra_ms) {
    Serial.println("[palabra]");             // una sola vez por hueco
    letra_desde_palabra = false;
  }
  // Nada mas que esperar: dejar de medir (y no envolver micros() a los 71 min).
  if (n_simbolo == 0 && !letra_desde_palabra) midiendo_silencio = false;
}

static void ponerContadoresACero() {
  noInterrupts();
  g_crudos = 0;
  g_desbordes = 0;
  interrupts();
  filtrados = puntos = rayas = letras = desconocidas = repetidos = 0;
}

static void ejecutarComando(const char *cmd) {
  char k = cmd[0];
  long v = atol(cmd + 1);
  if (k == 'r') {
    imprimirUmbrales();
    imprimirContadores();
  } else if (k == 'c') {
    ponerContadoresACero();
    Serial.println("# contadores a cero");
  } else if (k == 'v') {
    verbose = !verbose;
    imprimirUmbrales();
  } else if (k == 'd') {
    if (v < 0 || v > 200) { Serial.println("# rechazado: d<ms> admite 0..200"); return; }
    debounce_ms = (uint32_t)v;
    imprimirUmbrales();
  } else if (k == 'p' || k == 'l' || k == 'w') {
    if (v < 1 || v > 60000) { Serial.println("# rechazado: admite 1..60000 ms"); return; }
    if (k == 'l' && (uint32_t)v >= palabra_ms) {
      Serial.println("# rechazado: letra_ms debe ser menor que palabra_ms");
      return;
    }
    if (k == 'w' && (uint32_t)v <= letra_ms) {
      Serial.println("# rechazado: palabra_ms debe ser mayor que letra_ms");
      return;
    }
    if (k == 'p') punto_raya_ms = (uint32_t)v;
    if (k == 'l') letra_ms = (uint32_t)v;
    if (k == 'w') palabra_ms = (uint32_t)v;
    imprimirUmbrales();
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
  pinMode(RX_DATA, INPUT_PULLDOWN);   // sin cable: 0 estable, nunca flotante
  pinMode(LED_PIN, OUTPUT);
  nivel_ant = (GPIO.in & MASK_RX) ? 1 : 0;
  digitalWrite(LED_PIN, nivel_ant);
  attachInterrupt(RX_DATA, isrFlanco, CHANGE);
  Serial.println("# receptor morse listo");
  imprimirUmbrales();
}

void loop() {
  // Drenar el buffer: la ISR solo escribe head, loop() solo escribe tail.
  while (g_tail != g_head) {
    uint8_t t = g_tail;
    uint32_t t_us = g_buf[t].t_us;
    uint8_t nivel = g_buf[t].nivel;
    g_tail = (uint8_t)((t + 1) % N_FLANCOS);
    procesarFlanco(nivel, t_us);
  }
  revisarSilencio();
  leerSerie();
}
