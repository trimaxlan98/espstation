// Mock minimo del core Arduino para probar la logica de los sketches en el host.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#define IRAM_ATTR
#define INPUT_PULLDOWN 9
#define OUTPUT 3
#define LOW 0
#define HIGH 1
#define CHANGE 3
static uint32_t g_vt_us = 0;                       // tiempo virtual
static uint32_t micros() { return g_vt_us; }
static uint32_t millis() { return g_vt_us / 1000u; }
struct GpioDev { uint32_t in; };
static GpioDev GPIO;
static int g_pin_lvl[40];                          // lo que "lee" digitalRead
static int g_out_lvl[40];                          // lo que se escribio
static std::string g_txlog;                        // "t_ms:pin=nivel;" por escritura
static void pinMode(int, int) {}
static int digitalRead(int p) { return g_pin_lvl[p]; }
static void digitalWrite(int p, int v) {
  g_out_lvl[p] = v;
  g_txlog += std::to_string(g_vt_us / 1000u) + ":" + std::to_string(p) + "=" + std::to_string(v) + ";";
}
static void (*g_isr)(void) = nullptr;
static void attachInterrupt(int, void (*f)(void), int) { g_isr = f; }
static void noInterrupts() {}
static void interrupts() {}
struct SerialMock {
  std::string out, in;
  size_t rd = 0;
  void begin(int) {}
  void print(const char *s) { out += s; }
  void print(char c) { out += c; }
  void print(unsigned char v) { out += std::to_string((unsigned)v); }
  void print(int v) { out += std::to_string(v); }
  void print(unsigned v) { out += std::to_string(v); }
  void print(long v) { out += std::to_string(v); }
  void print(unsigned long v) { out += std::to_string(v); }
  template <class T> void println(T v) { print(v); out += "\n"; }
  void println() { out += "\n"; }
  int available() { return (int)(in.size() - rd); }
  int read() { return rd < in.size() ? (unsigned char)in[rd++] : -1; }
};
static SerialMock Serial;
