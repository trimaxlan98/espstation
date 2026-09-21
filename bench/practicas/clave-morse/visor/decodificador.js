/*
 * Decodificador Morse del visor: port en JavaScript de la maquina de estados de
 * receptor/receptor.ino (SPEC-MORSE.md). Sirve para VER la traduccion en el navegador;
 * la referencia sigue siendo el sketch. tests/replay_evidencia.py comprueba que este
 * port produce exactamente lo mismo que el sketch compilado en el host y que lo que
 * imprimio la placa real en la evidencia.
 *
 * Entrada: pulsos [{t0, dur}] en ms (t0 = instante de la subida, dur = tiempo a 1).
 * Los silencios son los huecos entre pulsos. `hasta` es el instante hasta el que se
 * evalua el silencio pendiente (Infinity = "ya no llega nada mas").
 */
(function (raiz) {
  "use strict";

  // Tabla Morse internacional (A-Z, 0-9). Mismo contenido que TABLA[] del sketch.
  const MORSE = {
    A: ".-", B: "-...", C: "-.-.", D: "-..", E: ".", F: "..-.", G: "--.", H: "....", I: "..", J: ".---",
    K: "-.-", L: ".-..", M: "--", N: "-.", O: "---", P: ".--.", Q: "--.-", R: ".-.", S: "...", T: "-",
    U: "..-", V: "...-", W: ".--", X: "-..-", Y: "-.--", Z: "--..",
    0: "-----", 1: ".----", 2: "..---", 3: "...--", 4: "....-", 5: ".....", 6: "-....", 7: "--...", 8: "---..", 9: "----.",
  };
  const INVERSA = {};
  for (const c of Object.keys(MORSE)) INVERSA[MORSE[c]] = c;

  const MAX_SIMBOLO = 8; // igual que el sketch

  /** Byte ASCII en 8 bits SIEMPRE, con ceros a la izquierda ('A' = 01000001). */
  function bin8(ch) {
    return ch.charCodeAt(0).toString(2).padStart(8, "0");
  }

  function decodificar(pulsos, p, hasta) {
    if (hasta === undefined) hasta = Infinity;
    const ev = [];
    const cnt = { puntos: 0, rayas: 0, filtrados: 0, letras: 0, desconocidas: 0 };
    let sim = "", desb = false, letraDesdePalabra = false, midiendo = false, tf = null;
    let indices = []; // indices (en la lista de entrada) de los pulsos de la letra en construccion

    function cerrarLetra(t) {
      const c = INVERSA[sim];
      if (c !== undefined && !desb) {
        cnt.letras++;
        ev.push({ t, k: "let", letra: c, bin: bin8(c), morse: sim, indices, texto: `[letra: ${c}] [bin: ${bin8(c)}]` });
      } else {
        cnt.desconocidas++;
        ev.push({ t, k: "let", letra: "?", bin: null, morse: sim, indices, texto: `[letra: ?] [morse: ${sim}${desb ? "+" : ""}]` });
      }
      sim = ""; desb = false; letraDesdePalabra = true; indices = [];
    }

    // Igual que revisarSilencio(): letra a los letra_ms, palabra a los palabra_ms (una sola vez).
    function silencioHasta(limite) {
      if (!midiendo) return;
      const hueco = limite - tf;
      if (sim.length > 0 && hueco >= p.l && tf + p.l <= hasta) cerrarLetra(tf + p.l);
      if (letraDesdePalabra && hueco >= p.w && tf + p.w <= hasta) {
        ev.push({ t: tf + p.w, k: "pal", texto: "[palabra]" });
        letraDesdePalabra = false;
      }
      if (sim.length === 0 && !letraDesdePalabra) midiendo = false;
    }

    for (let i = 0; i < pulsos.length; i++) {
      const pu = pulsos[i];
      silencioHasta(pu.t0); // el silencio que precede a este pulso
      if (pu.dur < p.d) { cnt.filtrados++; continue; } // ruido: ni simbolo ni corta el silencio
      const s = pu.dur < p.p ? "." : "-";
      if (s === ".") cnt.puntos++; else cnt.rayas++;
      if (sim.length < MAX_SIMBOLO) sim += s; else desb = true;
      indices.push(i);
      ev.push({ t: pu.t0 + pu.dur, k: "sim", texto: s, dur: pu.dur, idx: i });
      tf = pu.t0 + pu.dur;
      midiendo = true;
    }
    silencioHasta(hasta);
    return { eventos: ev, contadores: cnt };
  }

  /** Construye [{t0,dur}] a partir de [{sil,dur}] (sil = silencio antes del pulso; null en el primero). */
  function pulsosDesdeSilencios(lista, t0 = 0) {
    const out = [];
    let t = t0;
    lista.forEach((x, i) => {
      t += i === 0 ? 0 : (x.sil || 0);
      out.push({ t0: t, dur: x.dur });
      t += x.dur;
    });
    return out;
  }

  /** Texto -> pulsos con temporizacion Morse estandar (punto=1u, raya=3u, dentro=1u, letra=3u, palabra=7u). */
  function textoAPulsos(texto, u) {
    const out = [];
    let t = 0;
    const palabras = texto.toUpperCase().split(/\s+/).filter(Boolean);
    let primera = true;
    for (const pal of palabras) {
      if (!primera) t += 7 * u;
      primera = false;
      let primeraLetra = true;
      for (const ch of pal) {
        if (!MORSE[ch]) continue;
        if (!primeraLetra) t += 3 * u;
        primeraLetra = false;
        [...MORSE[ch]].forEach((s, i) => {
          if (i > 0) t += u;
          const dur = s === "." ? u : 3 * u;
          out.push({ t0: t, dur, simbolo: s, letra: ch });
          t += dur;
        });
      }
    }
    return out;
  }

  const API = { MORSE, INVERSA, MAX_SIMBOLO, bin8, decodificar, pulsosDesdeSilencios, textoAPulsos };
  if (typeof module !== "undefined" && module.exports) module.exports = API;
  else raiz.Morse = API;
})(typeof window !== "undefined" ? window : globalThis);
