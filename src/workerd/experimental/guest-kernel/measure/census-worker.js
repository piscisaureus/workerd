// The subject worker of the allocation census's phase experiment (see
// memory-classes.md). Two services run this same module, so the two isolates do
// identical work and a site that grows for the first and not the second is
// process-wide state the first isolate established.
//
// Each request exercises the four paths the guest-kernel faults came from:
// instantiating a wasm module (V8's process-wide type canonicalizer), an
// optimizing compile and a deopt (V8's process-wide JIT-page map), the Intl
// builtins (ICU's and V8's process-wide locale state), and a service-binding
// fetch and a JS RPC call (Cap'n Proto's message arena).

// The module arrives already compiled: workerd does not allow a worker to
// generate wasm code at run time, so the decode -- which is where V8's type
// canonicalizer gains the module's recursion groups -- happens at script load,
// under the isolate's lock and outside any turn.
import mod from 'census-types.wasm';

// Hot enough to reach the optimizing tier, then called with another type so the
// optimized code deopts; the deopt is what walks the JIT-page map.
function arith(a, b) {
  return a * 3 + b * 7 - (a ^ b);
}

// One call with a long loop, so the loop is replaced on the stack (OSR).
function osr(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += (i * 2654435761) % 1000003;
  }
  return s;
}

function wasm() {
  const instance = new WebAssembly.Instance(mod);
  const e = instance.exports;
  return (
    e.f0(2, 3) + Number(e.f1(1n)) + e.f2(1.5, 2.5) + e.f3() + e.f4(1, 2n, 3.5)
  );
}

function intl(d) {
  const parts = [];
  for (const loc of ['en-US', 'de-DE', 'ja-JP', 'ar-EG']) {
    parts.push(
      new Intl.DateTimeFormat(loc, {
        dateStyle: 'full',
        timeStyle: 'long',
        timeZone: 'UTC',
      }).format(d)
    );
    parts.push(
      new Intl.NumberFormat(loc, { style: 'currency', currency: 'EUR' }).format(
        1234.5
      )
    );
    parts.push(String(new Intl.Collator(loc).compare('a', 'b')));
    parts.push(new Intl.PluralRules(loc).select(2));
    parts.push(new Intl.RelativeTimeFormat(loc).format(-1, 'day'));
  }
  return parts.join('|').length;
}

export default {
  async fetch(req, env) {
    let acc = 0;

    acc += wasm();

    for (let i = 0; i < 60000; i++) acc += arith(i, i + 1);
    acc += String(arith('deopt', 1)).length; // the deopt
    acc += osr(300000) % 1000;

    acc += intl(new Date(1700000000000));

    const res = await env.CALLEE_FETCH.fetch('http://callee/');
    acc += (await res.text()).length;
    for (let i = 0; i < 8; i++) {
      const stub = await env.CALLEE.getThing(i);
      acc += stub.n;
      acc += (await stub.hello()).length;
      stub[Symbol.dispose]();
    }

    return new Response(`ok ${acc}\n`);
  },
};
