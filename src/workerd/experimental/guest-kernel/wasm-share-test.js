import addModule from 'add.wasm';
import oobModule from 'oob.wasm';

// Two modules, so that each worker can be the one reaching the other's wasm state. oob.wasm's
// load() reads its linear memory at the offset it is given, so a large offset traps, which is
// what sends the turn through V8's trap handler and the code manager's address lookup.
const modules = { add: addModule, oob: oobModule };

// Calls a module's single export, so that the call reaches the lazily compiled code of the
// NativeModule the engine handed out and not only the instantiation.
function call(name, instance) {
  if (name === 'add') return instance.exports.add(1, 2);
  return instance.exports.load(0);
}

export default {
  async fetch(req) {
    const url = new URL(req.url);
    const name = url.searchParams.get('mod') ?? 'add';
    if (url.pathname === '/instantiate') {
      const instance = new WebAssembly.Instance(modules[name]);
      return new Response(
        JSON.stringify({ mod: name, result: call(name, instance) }) + '\n'
      );
    }
    if (url.pathname === '/trap') {
      // An out-of-bounds load traps, and V8 resolves the trapping address through the process-
      // wide code manager's map of code ranges before it throws.
      const instance = new WebAssembly.Instance(oobModule);
      let caught = 'none';
      try {
        instance.exports.load(1 << 24);
      } catch (e) {
        caught = e.constructor.name;
      }
      return new Response(
        JSON.stringify({ caught, stack: trapStack() }) + '\n'
      );
    }
    if (url.pathname === '/lazy') {
      // Repeated instantiation and calls, each of which looks the module up in the engine's
      // registries and reaches its code manager through lazy compilation.
      const n = Number(url.searchParams.get('n') ?? 200);
      let sum = 0;
      for (let i = 0; i < n; i++) {
        sum += call(name, new WebAssembly.Instance(modules[name]));
      }
      return new Response(JSON.stringify({ mod: name, n, sum }) + '\n');
    }
    return new Response('ok\n');
  },
};

// Walks a stack that has a Wasm frame on it, which is the other reader of the code manager's
// address map: building the stack trace iterates the frames and resolves each code address.
function trapStack() {
  const instance = new WebAssembly.Instance(oobModule);
  try {
    instance.exports.load(1 << 24);
  } catch (e) {
    return (e.stack ?? '').includes('wasm')
      ? 'has-wasm-frame'
      : 'no-wasm-frame';
  }
  return 'no-trap';
}
