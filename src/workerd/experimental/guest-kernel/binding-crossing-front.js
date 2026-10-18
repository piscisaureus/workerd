// Driven by binding-crossing-test.sh: each route makes one crossing of a binding that
// body-arena-test.sh does not -- a request that carries a cf blob, a call the callee fails, a
// body the caller gives up on mid-stream, props a loaded worker reads, a Durable Object another
// worker also addresses, a traced worker -- and reports what came back so that the script can
// check it.
import { WorkerEntrypoint } from 'cloudflare:workers';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Loopback entrypoint whose props are made here and read here, after a stub to it has gone
// through BACK over RPC.
export class FrontEntry extends WorkerEntrypoint {
  hello() {
    return `hello ${this.ctx.props.greeting}`;
  }
}

async function readAll(readable) {
  const reader = readable.getReader();
  let out = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    out += dec.decode(value);
  }
  return out;
}

const loaded = {
  compatibilityDate: '2025-01-01',
  mainModule: 'm.js',
  modules: {
    'm.js': `export default {
      async fetch(req, env, ctx) { return new Response(JSON.stringify(ctx.props)); }
    };`,
  },
};

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    const name = url.pathname.slice(1);
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
        case '/nonresponse': {
          // The callee's fetch handler returns something that is not a Response; the host
          // raises the error under the callee's lock.
          await env.BACK.fetch('http://back/nonresponse');
          return new Response('nonresponse did-not-throw\n');
        }
        case '/body-error': {
          // The callee's response body errors after its first chunk.
          const r = await env.BACK.fetch('http://back/body-error');
          return new Response(`body-error read=${await r.text()}\n`);
        }
        case '/body-error-up': {
          // The request body errors after its first chunk; the callee reports what it read.
          const body = new ReadableStream({
            start(c) {
              c.enqueue(enc.encode('x'));
            },
            pull(c) {
              c.error(new Error('upmid'));
            },
          });
          const r = await env.BACK.fetch('http://back/body-error-up', {
            method: 'POST',
            body,
          });
          return new Response(`body-error-up ${await r.text()}\n`);
        }
        case '/connect-none': {
          // A tunnel to a worker with no connect handler: the host refuses it under the
          // callee's lock.
          const sock = env.PLAIN.connect('plain:1');
          await sock.writable.getWriter().write(enc.encode('ping'));
          return new Response(
            `connect-none got=${await readAll(sock.readable)}\n`
          );
        }
        case '/connect-throw': {
          // A tunnel to a worker whose connect handler throws before it reads.
          const sock = env.BACK.connect('back:1');
          await sock.writable.getWriter().write(enc.encode('ping'));
          return new Response(
            `connect-throw got=${await readAll(sock.readable)}\n`
          );
        }
        case '/restore': {
          // The callee calls ctx.restore(), which reads what this turn made for its request.
          const r = await env.BACK.fetch('http://back/restore');
          return new Response(`restore ${await r.text()}\n`);
        }
        case '/do-restore': {
          // The Durable Object does the same.
          const r = await env.NS.get(env.NS.idFromName('shared')).fetch(
            'http://do/restore'
          );
          return new Response(`do-restore ${await r.text()}\n`);
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
        case '/rpc-stream': {
          // A stream as an RPC argument, read whole by the callee.
          const s = new ReadableStream({
            start(c) {
              c.enqueue(enc.encode('z'.repeat(70000)));
              c.close();
            },
          });
          return new Response(`rpc-stream n=${await env.BACK.takeStream(s)}\n`);
        }
        case '/rpc-stream-ret': {
          // A stream as an RPC result, read whole here.
          const t = await readAll(await env.BACK.giveStream());
          return new Response(`rpc-stream-ret n=${t.length}\n`);
        }
        case '/cancel-mid': {
          // One chunk of a slow body, then the reader cancels the rest.
          const r = await env.BACK.fetch('http://back/slow');
          const reader = r.body.getReader();
          const first = await reader.read();
          await reader.cancel(new Error('mid-cancel'));
          return new Response(`cancel-mid first=${first.value.length}\n`);
        }
        case '/abort-mid': {
          // One chunk, then an AbortController aborts the fetch; the next read must reject.
          const ac = new AbortController();
          const r = await env.BACK.fetch('http://back/slow', {
            signal: ac.signal,
          });
          const reader = r.body.getReader();
          const first = await reader.read();
          ac.abort(new Error('mid-abort'));
          let then = 'read-ok';
          try {
            await reader.read();
          } catch (e) {
            then = `${e.name}:${e.message}`;
          }
          return new Response(
            `abort-mid first=${first.value.length} then=${then}\n`
          );
        }
        case '/timeout-mid': {
          // A signal that times out while the body is still arriving.
          const r = await env.BACK.fetch('http://back/slow', {
            signal: AbortSignal.timeout(300),
          });
          let then = 'read-ok';
          try {
            await r.text();
          } catch (e) {
            then = `${e.name}:${e.message}`;
          }
          return new Response(`timeout-mid then=${then}\n`);
        }
        case '/slow':
          // The slow body passed through, for a client that goes away mid-response.
          return env.BACK.fetch('http://back/slow');
        case '/ws-reject': {
          // An upgrade request the callee answers with a plain response: the body comes back
          // on the WebSocket path.
          const r = await env.BACK.fetch('http://back/ws-reject', {
            headers: { Upgrade: 'websocket' },
          });
          return new Response(
            `ws-reject status=${r.status} body=${await r.text()}\n`
          );
        }
        case '/cache': {
          // The Cache API, served by a worker over cacheApiOutbound.
          const key = `http://front/cached-${Date.now()}`;
          await caches.default.put(
            key,
            new Response('cached-body', {
              headers: { 'cache-control': 'max-age=60' },
            })
          );
          const m = await caches.default.match(key);
          return new Response(`cache match=${m ? await m.text() : 'miss'}\n`);
        }
        case '/props': {
          // A loaded worker's entrypoint with props, requested three times: the first request
          // goes through the loader's startup, the later ones start from this turn.
          const w = env.LOADER.get('loaded', () => loaded);
          const ep = w.getEntrypoint(undefined, {
            props: { greeting: 'twice', n: 2 },
          });
          const a = await (await ep.fetch('http://loaded/')).text();
          const b = await (await ep.fetch('http://loaded/')).text();
          const ep2 = w.getEntrypoint(undefined, {
            props: { greeting: 'thrice', n: 3 },
          });
          const c = await (await ep2.fetch('http://loaded/')).text();
          return new Response(`props ${a} ${b} ${c}\n`);
        }
        case '/loopback-props': {
          // A loopback stub with props, handed to BACK over RPC and called from there.
          const stub = ctx.exports.FrontEntry({
            props: { greeting: 'via-back' },
          });
          return new Response(
            `loopback-props got=${await env.BACK.callStub(stub)}\n`
          );
        }
        case '/do': {
          // A Durable Object whose class is in another worker, addressed from here; BACK
          // addresses the same object from its own turns.
          const r = await env.NS.get(env.NS.idFromName('shared')).fetch(
            'http://do/'
          );
          return new Response(`do ${await r.text()}\n`);
        }
        case '/do-via-back': {
          // The same object, addressed from back's turn.
          const r = await env.BACK.fetch('http://back/do');
          return new Response(`do-via-back ${await r.text()}\n`);
        }
        case '/traced-log':
        case '/traced-throw': {
          // A traced worker reached through a binding, so that what its tail workers receive
          // -- the event's url, a log line, an exception -- was made in this turn and its own.
          const r = await env.TRACED.fetch(`http://traced/${name.slice(7)}`);
          return new Response(`${name} status=${r.status}\n`);
        }
        default:
          return new Response('front');
      }
    } catch (e) {
      return new Response(`${name} caught=${e.name}:${e.message}\n`);
    }
  },
};
