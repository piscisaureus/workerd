export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const route = url.pathname.slice(1);
    const n = Number(url.searchParams.get('n') || '5');
    const out = (s) => new Response(`${route} n=${n} ${s}\n`);

    switch (route) {
      case 'pipeline': {
        // A method called on the promise of a stub: the second call is sent before the first
        // has returned, and capnp queues it on the pipeline the first call's result will resolve.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          if ((await env.CALLEE.getThing(i).hello()) === 'hi' + i) ok++;
        }
        return out(`ok=${ok}`);
      }
      case 'deep': {
        // `depth` pipelined hops before the call whose result is awaited, so the queued-call
        // chain the caller's turn builds is as long as JS cares to make it.
        const depth = Number(url.searchParams.get('depth') || '8');
        let ok = 0;
        for (let i = 0; i < n; i++) {
          let p = env.CALLEE.getThing(i);
          for (let d = 0; d < depth; d++) p = p.self();
          if ((await p.hello()) === 'hi' + i) ok++;
        }
        return out(`depth=${depth} ok=${ok}`);
      }
      case 'object': {
        // A plain-object return value, which the callee wraps in a transient RPC target of its
        // own, and a property path through it.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          const a = await env.CALLEE.getObj(i).hello();
          const b = await env.CALLEE.getObj(i).child.hello();
          if (a === 'obj' + i && b === 'child' + i) ok++;
        }
        return out(`ok=${ok}`);
      }
      case 'far': {
        // The callee returns a stub it holds on a third isolate, so the pipelined call is
        // forwarded through two membranes into a turn of neither party.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          if ((await env.CALLEE.getFar(i).hello()) === 'far' + i) ok++;
        }
        return out(`ok=${ok}`);
      }
      case 'fanout': {
        // `width` calls queued on one promise that is still pending when every one of them is
        // sent; the callee's answer releases them all at once.
        const width = Number(url.searchParams.get('width') || '50');
        let ok = 0;
        for (let i = 0; i < n; i++) {
          const p = env.CALLEE.slowThing(i);
          const rs = await Promise.all(
            Array.from({ length: width }, () => p.hello())
          );
          if (rs.every((r) => r === 'hi' + i)) ok++;
        }
        return out(`width=${width} ok=${ok}`);
      }
      case 'resolved': {
        // Pipelining on a promise that has already settled: a stub result is called directly,
        // and a plain-object result is wrapped in a one-off local target for the call.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          const p = env.CALLEE.getThing(i);
          await p;
          const q = env.CALLEE.getObj(i);
          await q;
          if ((await p.hello()) === 'hi' + i && (await q.hello()) === 'obj' + i)
            ok++;
        }
        return out(`ok=${ok}`);
      }
      case 'reject': {
        // The first call throws, so the pipeline it would have resolved is broken; and the
        // first call returns a number, which nothing can be pipelined on.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          let threw = false;
          let notObject = false;
          try {
            await env.CALLEE.boom(i).hello();
          } catch (e) {
            threw = String(e).includes('boom' + i);
          }
          try {
            await env.CALLEE.getNumber(i).hello();
          } catch (e) {
            notObject = true;
          }
          if (threw && notObject) ok++;
        }
        return out(`ok=${ok}`);
      }
      case 'drop': {
        // Pipelined calls made and abandoned while the first call is still pending, so that
        // the response leaves the caller's turn with the queued calls unforwarded.
        for (let i = 0; i < n; i++) {
          env.CALLEE.slowThing(i)
            .hello()
            .catch(() => {});
        }
        return out('ok=true');
      }
      case 'echo': {
        // A stub sent as an argument and returned, then pipelined on: the capability crosses
        // the binding twice before the queued call reaches it.
        let ok = 0;
        for (let i = 0; i < n; i++) {
          const stub = await env.CALLEE.getThing(i);
          if ((await env.CALLEE.echo(stub).hello()) === 'hi' + i) ok++;
          stub[Symbol.dispose]();
        }
        return out(`ok=${ok}`);
      }
    }
    return new Response('caller\n');
  },
};
