// CLI para las pruebas: lee JSON {casos:[{umbrales:{p,l,w,d}, pulsos:[{sil,dur}]}]} por stdin y
// escribe JSON con las lineas que imprimiria B segun el port del visor.
const M = require("./decodificador.js");
let s = "";
process.stdin.on("data", (c) => (s += c));
process.stdin.on("end", () => {
  const { casos } = JSON.parse(s);
  const salida = casos.map((c) => {
    const r = M.decodificar(M.pulsosDesdeSilencios(c.pulsos), c.umbrales);
    return r.eventos.map((e) => e.texto);
  });
  process.stdout.write(JSON.stringify(salida));
});
