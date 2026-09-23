import addMod from "add.wasm";
import oobMod from "oob.wasm";

// Exercises a broad V8 surface from a worker whose JS turn runs at guest ring 3.
export default {
  async fetch(req) {
    const url = new URL(req.url);
    const out = {};
    // Wasm compiled from a config module (runtime codegen from JS is disallowed by the embedder).
    out.wasmAdd = new WebAssembly.Instance(addMod).exports.add(40, 2);
    // irregexp.
    out.regex = [...("12-abc 34-def".matchAll(/(\d+)-(\w+)/g))].map(x => x[2]).join(",");
    // WebCrypto (BoringSSL) digest + CSPRNG.
    const dig = await crypto.subtle.digest("SHA-256", new TextEncoder().encode("hello"));
    out.sha256 = [...new Uint8Array(dig)].slice(0, 4).map(b => b.toString(16).padStart(2, "0")).join("");
    const rnd = new Uint8Array(8); crypto.getRandomValues(rnd); out.rndLen = rnd.length;
    // A JIT-heavy loop.
    let s = 0; for (let i = 0; i < 1000000; i++) s = (s + i * 2654435761) >>> 0; out.compute = s;
    // A Wasm out-of-bounds access: should surface as a WebAssembly.RuntimeError, not a guest fault.
    if (url.pathname === "/wasm-oob") {
      const load = new WebAssembly.Instance(oobMod).exports.load;
      try { out.oob = load(1048576); } catch (e) { out.oob = "threw:" + e.constructor.name; }
    }
    return new Response(JSON.stringify(out) + "\n");
  }
};
