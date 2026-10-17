export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/rpc') {
      const n = Number(url.searchParams.get('n') || '100');
      let got = 0;
      for (let i = 0; i < n; i++) {
        const stub = await env.CALLEE.getThing(i);
        if (stub.n !== i) throw new Error(`wrong answer: ${stub.n} != ${i}`);
        if (typeof stub.hello !== 'function') throw new Error('no method');
        // Dropped inside this turn: the callee's ReverseIoOwn of its own handles is
        // released from a turn of this worker.
        stub[Symbol.dispose]();
        got++;
      }
      return new Response(`rpc=${got}\n`);
    }
    if (url.pathname === '/sub') {
      // The same context-building path without RPC: a service-binding fetch also
      // constructs the callee's IoContext inside this turn.
      const n = Number(url.searchParams.get('n') || '20');
      let got = 0;
      for (let i = 0; i < n; i++) {
        const res = await env.CALLEE_FETCH.fetch('http://callee/');
        if ((await res.text()) !== 'callee\n') throw new Error('wrong body');
        got++;
      }
      return new Response(`sub=${got}\n`);
    }
    return new Response('ok\n');
  },
};
