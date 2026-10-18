// Driven by binding-crossing-test.sh: each route makes one crossing of the BACK service binding
// that body-arena-test.sh does not -- a request that carries a cf blob, or a call the callee
// fails -- and reports what came back so that the script can check it.
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      switch (url.pathname) {
        case '/forward': {
          // The incoming Request itself, as a proxy forwards it. It carries the cf blob the
          // listener made for the connection, which crosses the binding beside the url and the
          // headers; back echoes what it sees as req.cf.
          const r = await env.BACK.fetch(req);
          return new Response(
            `forward status=${r.status} cf=${await r.text()}\n`
          );
        }
        case '/cf': {
          // A fresh request with a cf property of its own.
          const r = await env.BACK.fetch('http://back/cf', {
            cf: { colo: 'XYZ', n: 1 },
          });
          return new Response(`cf status=${r.status} cf=${await r.text()}\n`);
        }
        case '/throw': {
          // The callee's fetch handler throws.
          await env.BACK.fetch('http://back/throw');
          return new Response('throw did-not-throw\n');
        }
        case '/reject': {
          // The callee's fetch handler returns a rejected promise.
          await env.BACK.fetch('http://back/reject');
          return new Response('reject did-not-throw\n');
        }
        case '/rpc-ok':
          return new Response(`rpc-ok ${await env.BACK.ok()}\n`);
        case '/rpc-throw': {
          // The callee's RPC method throws.
          await env.BACK.boom();
          return new Response('rpc-throw did-not-throw\n');
        }
        case '/rpc-reject': {
          // The callee's RPC method rejects after an await.
          await env.BACK.rejectLater();
          return new Response('rpc-reject did-not-throw\n');
        }
        default:
          return new Response('front');
      }
    } catch (e) {
      return new Response(
        `${url.pathname.slice(1)} caught=${e.name}:${e.message}\n`
      );
    }
  },
};
