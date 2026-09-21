// Harness de host para el sketch REAL n3_bytes.ino.
//
// El .ino se incluye con #include (la ruta llega en -DN3_INO=...): no hay copia
// transcrita a mano, asi que si el sketch cambia, este test lo ve. Como el .ino
// compila TODAS sus funciones en cualquier rol (solo cambia la constante
// SOY_TX), un unico ejecutable ejercita a la vez el transmisor (txIniciar /
// txLoop / enviarTrama) y el receptor (isrReloj / rxLoop / rxReporte), llamando
// directamente a las funciones static del sketch.
//
// Uso:  host_n3 <orden> [args]   -- resultados en stdout como  clave=valor,
// que parsea run_tests.py. Lo que el sketch imprime por "serie" va a stderr.
// Ninguna orden decide aprobado/suspenso: eso lo hace el driver, que es quien
// conoce los valores esperados (SPEC-LINK.md).
#include "Arduino.h"

#include N3_INO

#include <stdlib.h>

#include <string>
#include <vector>

// --- utilidades --------------------------------------------------------------

static std::string hex(const uint8_t *p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof b, "%02x", p[i]);
    s += b;
  }
  return s;
}

static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

// Un flanco de subida de reloj con `bit` en DATA, tal como lo veria la ISR.
static void feedBit(int bit) {
  GPIO.in = bit ? MASK_RX_DATA : 0;
  isrReloj();
}

static void feedBytes(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    for (int b = 7; b >= 0; b--) feedBit((p[i] >> b) & 1);
}

static void rxLimpiar() {
  rxVolverAHunt();
  g_head = 0;
  g_tail = 0;
  g_len_err = 0;
  g_overruns = 0;
}

// Vacia la cola ISR->loop sin pasar por la contabilidad de rxLoop().
static void drain(std::vector<Trama> &out) {
  while (g_tail != g_head) {
    out.push_back(g_ring[g_tail]);
    g_tail = (uint8_t)((g_tail + 1) % RING_N);
  }
}

// rxReporte() imprime por "serie": se captura para devolver la linea CSV tal
// como la veria el alumno (formato de BER incluido).
static std::string captureReport() {
  FILE *tmp = tmpfile();
  FILE *old = g_serial_out;
  g_serial_out = tmp;
  rxReporte(false);
  g_serial_out = old;
  fflush(tmp);
  rewind(tmp);
  char line[512], last[512] = "";
  while (fgets(line, sizeof line, tmp)) {
    if (line[0] != '#') strcpy(last, line);
  }
  fclose(tmp);
  std::string s(last);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static void printRxState() {
  printf("ok=%u crc_err=%u len_err=%u bits_rx=%u bit_errors=%u overruns=%u esperado=%u\n",
         (unsigned)c_ok, (unsigned)c_crc_err, (unsigned)g_len_err, (unsigned)c_bits_rx,
         (unsigned)c_bit_errors, (unsigned)g_overruns, (unsigned)rx_esperado);
  printf("report=%s\n", captureReport().c_str());
}

// --- ordenes ------------------------------------------------------------------

// crc <hex>  ->  crc=XX
static int cmdCrc(const std::string &h) {
  std::vector<uint8_t> d = unhex(h);
  uint8_t c = 0;
  for (uint8_t b : d) c = crc8Byte(c, b);
  printf("crc=%02x\n", c);
  return 0;
}

// frame <hexpayload>  ->  frame=<hex>
static int cmdFrame(const std::string &h) {
  std::vector<uint8_t> p = unhex(h);
  uint8_t out[MAX_LEN + 3];
  uint8_t n = armarTrama(p.data(), (uint8_t)p.size(), out);
  printf("frame=%s\n", hex(out, n).c_str());
  return 0;
}

// gen <seq>  ->  payload=<hex> frame=<hex>   (generador de prueba del SPEC)
static int cmdGen(uint32_t seq) {
  uint8_t p[MAX_LEN], f[MAX_LEN + 3];
  uint8_t len = generarPayload(seq, p);
  uint8_t n = armarTrama(p, len, f);
  printf("payload=%s frame=%s\n", hex(p, len).c_str(), hex(f, n).c_str());
  return 0;
}

// genall  ->  una linea "seq payload frame" por cada seq 0..999
static int cmdGenAll() {
  for (uint32_t seq = 0; seq < 1000; seq++) {
    uint8_t p[MAX_LEN], f[MAX_LEN + 3];
    uint8_t len = generarPayload(seq, p);
    uint8_t n = armarTrama(p, len, f);
    printf("%u %s %s\n", (unsigned)seq, hex(p, len).c_str(), hex(f, n).c_str());
  }
  return 0;
}

// feed <hexframe>  -> pasa la trama (y 36 bytes de linea inactiva) por la FSM
// del receptor:  result=<status>:<len>:<payloadhex> ...
static int cmdFeed(const std::string &h) {
  std::vector<uint8_t> f = unhex(h);
  rxLimpiar();
  feedBytes(f.data(), f.size());
  std::vector<uint8_t> idle(36, 0);
  feedBytes(idle.data(), idle.size());
  std::vector<Trama> got;
  drain(got);
  printf("results=%zu len_err=%u", got.size(), (unsigned)g_len_err);
  for (const Trama &t : got)
    printf(" %s:%u:%s", t.estado == TR_OK ? "ok" : "crc_err", t.len, hex(t.payload, t.len).c_str());
  printf("\n");
  return 0;
}

// sweep: un bit-flip cada vez sobre LEN/PAYLOAD/CRC de las tramas seq 0..999,
// con 36 bytes de ceros de linea inactiva detras (SPEC-LINK "Debilidad conocida").
static int cmdSweep() {
  unsigned len_flips = 0, len_ok_total = 0, len_flips_with_ok = 0, len_first_ok = 0;
  unsigned body_flips = 0, body_ok_total = 0, body_flips_with_ok = 0, body_first_ok = 0;
  std::vector<Trama> got;
  for (uint32_t seq = 0; seq < 1000; seq++) {
    uint8_t p[MAX_LEN], f[MAX_LEN + 3];
    uint8_t len = generarPayload(seq, p);
    uint8_t n = armarTrama(p, len, f);
    for (unsigned bi = 8; bi < (unsigned)n * 8; bi++) {
      uint8_t g[MAX_LEN + 3 + 36];
      memset(g, 0, sizeof g);
      memcpy(g, f, n);
      g[bi / 8] ^= (uint8_t)(0x80u >> (bi % 8));
      rxLimpiar();
      feedBytes(g, (size_t)n + 36);
      got.clear();
      drain(got);
      unsigned oks = 0;
      for (const Trama &t : got) oks += (t.estado == TR_OK);
      bool first_ok = !got.empty() && got[0].estado == TR_OK;
      if (bi < 16) {
        len_flips++;
        len_ok_total += oks;
        len_flips_with_ok += oks > 0;
        len_first_ok += first_ok;
      } else {
        body_flips++;
        body_ok_total += oks;
        body_flips_with_ok += oks > 0;
        body_first_ok += first_ok;
      }
    }
  }
  printf("len_flips=%u len_ok_total=%u len_flips_with_ok=%u len_first_ok=%u\n", len_flips,
         len_ok_total, len_flips_with_ok, len_first_ok);
  printf("body_flips=%u body_ok_total=%u body_flips_with_ok=%u body_first_ok=%u\n", body_flips,
         body_ok_total, body_flips_with_ok, body_first_ok);
  return 0;
}

// glued: tramas pegadas, sin hueco. Las que terminan en CRC con nibble bajo
// 0xA (1010) son el caso que da falso sincronismo si el registro de
// desplazamiento no se pone a cero al completar cada byte.
static int cmdGlued() {
  unsigned nibble_a = 0, pair_fail = 0;
  std::vector<Trama> got;
  auto frameOf = [](uint32_t seq, uint8_t *f, uint8_t *p, uint8_t *len) {
    *len = generarPayload(seq, p);
    return armarTrama(p, *len, f);
  };
  // (1) cada trama con nibble bajo del CRC == 0xA, pegada a la siguiente.
  for (uint32_t seq = 0; seq < 999; seq++) {
    uint8_t f1[MAX_LEN + 3], p1[MAX_LEN], l1, f2[MAX_LEN + 3], p2[MAX_LEN], l2;
    uint8_t n1 = frameOf(seq, f1, p1, &l1);
    if ((f1[n1 - 1] & 0x0F) != 0x0A) continue;
    nibble_a++;
    uint8_t n2 = frameOf(seq + 1, f2, p2, &l2);
    rxLimpiar();
    feedBytes(f1, n1);
    feedBytes(f2, n2);
    got.clear();
    drain(got);
    bool good = got.size() == 2 && g_len_err == 0 && got[0].estado == TR_OK && got[1].estado == TR_OK &&
                got[0].len == l1 && got[1].len == l2 && memcmp(got[0].payload, p1, l1) == 0 &&
                memcmp(got[1].payload, p2, l2) == 0;
    pair_fail += !good;
  }
  // (2) las 1000 tramas seguidas, sin un solo bit de hueco.
  unsigned run_ok = 0, run_bad = 0;
  rxLimpiar();
  for (uint32_t seq = 0; seq < 1000; seq++) {
    uint8_t f[MAX_LEN + 3], p[MAX_LEN], len;
    uint8_t n = frameOf(seq, f, p, &len);
    feedBytes(f, n);
    got.clear();
    drain(got);
    if (got.size() == 1 && got[0].estado == TR_OK && got[0].len == len && memcmp(got[0].payload, p, len) == 0)
      run_ok++;
    else
      run_bad++;
  }
  printf("nibble_a_frames=%u pair_fail=%u run_ok=%u run_bad=%u run_len_err=%u\n", nibble_a, pair_fail, run_ok,
         run_bad, (unsigned)g_len_err);
  return 0;
}

// --- enlace TX -> RX por el registro de bits -----------------------------------

struct LinkProbe {
  uint64_t t_last_data_write = 0, t_last_rise = 0;
  bool have_rise = false, have_data = false;
  double min_setup = 1e18, min_hold = 1e18, min_gap = 1e18;
  double sum_period = 0;
  uint64_t n_period = 0;
  double period_nominal = 0;
  FILE *bits = nullptr;
};
static LinkProbe g_probe;

// Cableado simulado: cada flanco de subida de TX_CLK muestrea TX_DATA y se lo
// entrega a la ISR del receptor, como haria el cable. De paso mide lo que la
// especificacion exige al transmisor: setup y hold del dato respecto al
// flanco, periodo, y el hueco entre tramas.
static void linkHook(uint32_t mask, bool set) {
  LinkProbe &p = g_probe;
  if (mask & MASK_TX_DATA) {
    if (p.have_rise) {
      double hold = (double)(g_vcycles - p.t_last_rise);
      if (hold < p.min_hold) p.min_hold = hold;
    }
    p.t_last_data_write = g_vcycles;
    p.have_data = true;
  }
  if ((mask & MASK_TX_CLK) && set) {
    int bit = (g_out & MASK_TX_DATA) ? 1 : 0;
    if (p.have_data) {
      double setup = (double)(g_vcycles - p.t_last_data_write);
      if (setup < p.min_setup) p.min_setup = setup;
    }
    if (p.have_rise) {
      double d = (double)(g_vcycles - p.t_last_rise);
      if (d > 4 * p.period_nominal) {
        double gap = d / p.period_nominal;
        if (gap < p.min_gap) p.min_gap = gap;
        if (p.bits) fputc('\n', p.bits);
      } else {
        p.sum_period += d;
        p.n_period++;
      }
    }
    p.t_last_rise = g_vcycles;
    p.have_rise = true;
    if (p.bits) fputc(bit ? '1' : '0', p.bits);
    feedBit(bit);
    rxLoop();
  }
}

// link <hz> <ficheroBits|->
static int cmdLink(uint32_t hz, const char *bitsPath) {
  aplicarVelocidad(hz);
  g_cyc_step = g_medio_ciclos / 8 ? g_medio_ciclos / 8 : 1;
  rxReset();
  g_probe = LinkProbe();
  g_probe.period_nominal = 2.0 * g_medio_ciclos;
  if (strcmp(bitsPath, "-") != 0) g_probe.bits = fopen(bitsPath, "w");
  g_out = 0;
  g_out_hook = linkHook;

  txIniciar();
  while (tx_activo) txLoop();
  g_out_hook = nullptr;
  rxLoop();
  if (g_probe.bits) {
    fputc('\n', g_probe.bits);
    fclose(g_probe.bits);
  }
  double mean = g_probe.n_period ? g_probe.sum_period / (double)g_probe.n_period : 0.0;
  printf("rate=%u medio=%u step=%u\n", (unsigned)hz, (unsigned)g_medio_ciclos, (unsigned)g_cyc_step);
  printf("period_nominal=%.1f period_mean=%.3f min_setup=%.1f min_hold=%.1f min_gap_periods=%.3f\n",
         g_probe.period_nominal, mean, g_probe.min_setup, g_probe.min_hold, g_probe.min_gap);
  printf("final_data=%u final_clk=%u tx_seq=%u\n", (unsigned)((g_out >> TX_DATA) & 1),
         (unsigned)((g_out >> TX_CLK) & 1), (unsigned)tx_seq);
  printRxState();
  g_cyc_step = 5;
  return 0;
}

// stream <fichero>: un flujo de bits ('0'/'1') por la ISR + rxLoop() reales.
static int cmdStream(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "no puedo abrir %s\n", path);
    return 2;
  }
  rxReset();
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (c != '0' && c != '1') continue;
    feedBit(c == '1');
    rxLoop();
  }
  fclose(f);
  printRxState();
  return 0;
}

// speeds: comandos '+', '-' y 'b<N>' por el parser serie del sketch.
static int cmdSpeeds() {
  const char *seq[] = {"+", "+", "+", "+", "+", "+", "-", "-", "-", "-", "-", "-", "-", "b12345\n", "+", "-", "b50", "b600000\n"};
  printf("rate0=%u\n", (unsigned)g_rate_hz);
  for (const char *c : seq) {
    Serial.feed(c);
    atenderSerie();
    delay(400);   // 'b<N>' sin fin de linea se cierra por tiempo
    atenderSerie();
    std::string label(c);
    while (!label.empty() && label.back() == '\n') label.pop_back();
    printf("after=%s rate=%u\n", label.c_str(), (unsigned)g_rate_hz);
  }
  return 0;
}

// speedlist: la lista de velocidades del sketch, tal cual.
static int cmdSpeedList() {
  printf("speeds=");
  for (uint8_t i = 0; i < N_VELOCIDADES; i++) printf("%s%u", i ? "," : "", (unsigned)VELOCIDADES[i]);
  printf("\ntotal_tramas=%u max_len=%u\n", (unsigned)TOTAL_TRAMAS, (unsigned)MAX_LEN);
  return 0;
}

// setuporder: setup() del transmisor debe escribir el latch de DATA y CLK a 0
// ANTES de habilitar su driver, o cada arranque mete un flanco de reloj espurio.
static int cmdSetupOrder() {
  if (!SOY_TX) return 2;
  setup();
  int d_pm = g_pin_evt[TX_DATA], c_pm = g_pin_evt[TX_CLK];
  printf("data_latch_evt=%d data_pinmode_evt=%d clk_latch_evt=%d clk_pinmode_evt=%d\n", g_latch_evt[TX_DATA], d_pm,
         g_latch_evt[TX_CLK], c_pm);
  printf("data_latch_first=%d clk_latch_first=%d latch_a_cero=%d\n",
         g_latch_evt[TX_DATA] > 0 && g_latch_evt[TX_DATA] < d_pm,
         g_latch_evt[TX_CLK] > 0 && g_latch_evt[TX_CLK] < c_pm, (int)((g_out & (MASK_TX_DATA | MASK_TX_CLK)) == 0));
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  std::string cmd = argv[1];
  aplicarVelocidad(BIT_RATE_HZ);
  if (cmd == "crc" && argc >= 3) return cmdCrc(argv[2]);
  if (cmd == "frame" && argc >= 3) return cmdFrame(argv[2]);
  if (cmd == "gen" && argc >= 3) return cmdGen((uint32_t)strtoul(argv[2], nullptr, 10));
  if (cmd == "genall") return cmdGenAll();
  if (cmd == "feed" && argc >= 3) return cmdFeed(argv[2]);
  if (cmd == "sweep") return cmdSweep();
  if (cmd == "glued") return cmdGlued();
  if (cmd == "link" && argc >= 4) return cmdLink((uint32_t)strtoul(argv[2], nullptr, 10), argv[3]);
  if (cmd == "stream" && argc >= 3) return cmdStream(argv[2]);
  if (cmd == "speeds") return cmdSpeeds();
  if (cmd == "setuporder") return cmdSetupOrder();
  if (cmd == "speedlist") return cmdSpeedList();
  fprintf(stderr, "orden desconocida: %s\n", cmd.c_str());
  return 2;
}
