// Structural runtime smoke for the wasm example modules.
//
// Fully compiles each .wasm in node with WebAssembly.compile, which parses every
// section and function body. That catches a truncated or malformed module the
// link smokes' 4-byte magic check cannot. It deliberately does NOT instantiate
// (the emscripten import object needs the browser JS runtime) or run main (SDL
// video needs a canvas), so it stays headless and works even with the
// -sENVIRONMENT=web artifacts. A canvas paint check is still manual in Safari.
//
// Usage: node scripts/wasm-node-smoke.mjs <module.wasm> [more.wasm ...]
import { readFile } from "node:fs/promises";

const paths = process.argv.slice(2);
if (paths.length === 0) {
  console.error("wasm-node-smoke: no modules given");
  process.exit(2);
}

let failed = 0;
for (const path of paths) {
  try {
    const bytes = await readFile(path);
    const mod = await WebAssembly.compile(bytes);
    const imports = WebAssembly.Module.imports(mod).length;
    const exports = WebAssembly.Module.exports(mod).length;
    console.log(`  OK      ${path} (${imports} imports, ${exports} exports)`);
  } catch (err) {
    console.error(`  FAIL    ${path}: ${err.message}`);
    failed++;
  }
}
process.exit(failed ? 1 : 0);
