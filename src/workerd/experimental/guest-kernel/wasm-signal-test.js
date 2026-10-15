import signalModule from 'signal.wasm';

// The module exports its memory, a global __instance_signal naming the word at offset 0, and
// get_signal(), which loads that word. An instance is kept alive across requests so that the
// probe paths of wasm-signal-test.sh can act on its registered entry between turns.
let kept = null;

export default {
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === '/instantiate') {
      kept = new WebAssembly.Instance(signalModule);
      return new Response(
        JSON.stringify({ signal: kept.exports.get_signal() }) + '\n'
      );
    }
    if (url.pathname === '/burn') {
      // Spin reading the field until the shutdown signal lands in it, or give up after a bound
      // of about ten seconds of CPU, which no timer the script arms lets it reach.
      if (kept === null) kept = new WebAssembly.Instance(signalModule);
      // Start from a clear field, so that an earlier signal cannot stand in for this one.
      new Uint32Array(kept.exports.memory.buffer)[0] = 0;
      const limit = 1e9;
      let i = 0,
        value = 0;
      for (; i < limit; i++) {
        value = kept.exports.get_signal();
        if (value !== 0) break;
      }
      return new Response(
        JSON.stringify({ observed: value, iterations: i }) + '\n'
      );
    }
    if (url.pathname === '/instantiate-many') {
      // For timing an instantiation: n registrations, each of which is a hostcall under the
      // guest kernel. The instances are dropped at once, so the GC prologue unregisters them.
      const n = Number(url.searchParams.get('n') ?? 1000);
      let sum = 0;
      for (let i = 0; i < n; i++)
        sum += new WebAssembly.Instance(signalModule).exports.get_signal();
      return new Response(JSON.stringify({ n, sum }) + '\n');
    }
    return new Response('ok\n');
  },
};
