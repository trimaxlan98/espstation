// Mock minimo del core Arduino-ESP32 para ejecutar n3_bytes.ino en el host.
//
// Solo cubre lo que ese sketch usa. Su trabajo NO es imitar el hardware sino
// dar un tiempo virtual determinista y un registro GPIO observable, de modo
// que el .ino REAL (incluido con #include, nunca copiado) corra sin placa.
// Lo que este mock NO puede decir: latencias de ISR, jitter, rebotes, nada
// analogico. Eso sigue siendo cosa del banco.
#pragma once

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>

#define IRAM_ATTR
#define OUTPUT 0x03
#define INPUT_PULLDOWN 0x09
#define LOW 0
#define HIGH 1
#define RISING 0x01
#define CHANGE 0x03

// --- tiempo virtual ---------------------------------------------------------
// g_vcycles avanza SOLO cuando el sketch pide la hora de ciclos o duerme.
// g_cyc_step es cuanto avanza por cada lectura del contador: el harness lo
// ajusta a una fraccion del medio periodo para que las esperas activas
// terminen en pocas iteraciones (a 1 kbit/s serian millones) sin perder la
// resolucion que hace falta para medir periodo, setup y hold.
static uint64_t g_vcycles = 0;
static uint32_t g_cyc_step = 5;

static uint32_t millis() { return (uint32_t)(g_vcycles / 240000ull); }
static void delay(uint32_t ms) { g_vcycles += (uint64_t)ms * 240000ull; }
static uint32_t getCpuFrequencyMhz() { return 240; }

struct EspClass {
  uint32_t getCycleCount() {
    g_vcycles += g_cyc_step;
    return (uint32_t)g_vcycles;
  }
};
static EspClass ESP;

// --- registros GPIO ---------------------------------------------------------
// out_w1ts / out_w1tc son proxies: asignarles una mascara actualiza el latch
// simulado y avisa al harness (que es quien "cablea" TX con RX).
static uint32_t g_out = 0;
static void (*g_out_hook)(uint32_t mask, bool set) = nullptr;

// Orden relativo de eventos, para comprobar que el latch de un pin se escribe
// ANTES de habilitar su driver con pinMode(OUTPUT) (ver setup() del sketch).
static int g_evt = 0;
static int g_pin_evt[64];    // evento en que se hizo pinMode(pin, OUTPUT); 0 = nunca
static int g_latch_evt[64];  // primer evento de escritura del latch de ese pin; 0 = nunca

struct W1 {
  bool set;
  explicit W1(bool s) : set(s) {}
  W1 &operator=(uint32_t mask) {
    g_out = set ? (g_out | mask) : (g_out & ~mask);
    ++g_evt;
    for (int b = 0; b < 32; b++)
      if ((mask >> b) & 1 && g_latch_evt[b] == 0) g_latch_evt[b] = g_evt;
    if (g_out_hook) g_out_hook(mask, set);
    return *this;
  }
};
struct GpioDev {
  W1 out_w1ts{true};
  W1 out_w1tc{false};
  volatile uint32_t in = 0;
};
static GpioDev GPIO;

static void pinMode(int pin, int mode) {
  if (mode == OUTPUT && pin >= 0 && pin < 64) g_pin_evt[pin] = ++g_evt;
}
static void (*g_isr)(void) = nullptr;
static void attachInterrupt(int, void (*f)(void), int) { g_isr = f; }

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))

// --- serie ------------------------------------------------------------------
// La salida del sketch va a g_serial_out (stderr por defecto) para que stdout
// quede libre para los resultados del harness, que el driver Python parsea.
static FILE *g_serial_out = stderr;

struct SerialMock {
  std::deque<char> in;
  void begin(int) {}
  void flush() {}
  int available() { return (int)in.size(); }
  int read() {
    if (in.empty()) return -1;
    char c = in.front();
    in.pop_front();
    return (unsigned char)c;
  }
  void feed(const char *s) {
    while (*s) in.push_back(*s++);
  }
  void print(const char *s) { fputs(s, g_serial_out); }
  void print(char c) { fputc(c, g_serial_out); }
  void print(unsigned char v) { fprintf(g_serial_out, "%u", v); }
  void print(int v) { fprintf(g_serial_out, "%d", v); }
  void print(unsigned v) { fprintf(g_serial_out, "%u", v); }
  void print(long v) { fprintf(g_serial_out, "%ld", v); }
  void print(unsigned long v) { fprintf(g_serial_out, "%lu", v); }
  void println() { fputc('\n', g_serial_out); }
  template <typename T> void println(T v) {
    print(v);
    fputc('\n', g_serial_out);
  }
  int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(g_serial_out, fmt, ap);
    va_end(ap);
    return n;
  }
};
static SerialMock Serial;

void setup();
void loop();
