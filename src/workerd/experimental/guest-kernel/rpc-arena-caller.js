export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const n = Number(url.searchParams.get('n') || '5');
    if (url.pathname === '/rpc') {
      // Each round takes a stub from the callee, makes a second call on it, and disposes of it
      // inside this turn: the stub's host-side plumbing is built in the callee's turn and freed
      // in this one.
      for (let i = 0; i < n; i++) {
        const stub = await env.CALLEE.getThing(i);
        const greeting = await stub.hello();
        if (greeting !== 'hi' + i)
          throw new Error('wrong greeting: ' + greeting);
        stub[Symbol.dispose]();
      }
      return new Response('rpc=' + n + '\n');
    }
    if (url.pathname === '/blob') {
      // A return value large enough to need more than one message segment.
      let total = 0;
      for (let i = 0; i < n; i++) {
        const bytes = await env.CALLEE.blob(4096);
        if (bytes.length !== 4096 || bytes[0] !== 7)
          throw new Error('wrong blob');
        total += bytes.length;
      }
      return new Response('blob=' + total + '\n');
    }
    if (url.pathname === '/sub') {
      // A service-binding fetch, whose response headers this turn reads. The body is not read:
      // body bytes cross an in-process service binding by pointer and are not placed yet (see
      // the Limitations in README.md).
      let total = 0;
      for (let i = 0; i < n; i++) {
        const res = await env.CALLEE_FETCH.fetch('http://callee/');
        if (res.headers.get('x-callee') !== 'yes')
          throw new Error('wrong response');
        total += res.status;
      }
      return new Response('sub=' + total + '\n');
    }
    return new Response('caller\n');
  },
};
