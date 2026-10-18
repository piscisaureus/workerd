// Driven by binding-crossing-test.sh: each route makes one crossing of a binding that
// body-arena-test.sh does not -- a request that carries a cf blob, a call the callee fails, a
// body the caller gives up on mid-stream, props a loaded worker reads, a Durable Object another
// worker also addresses, a traced worker, a connect() tunnel, a WebSocket -- and reports what
// came back so that the script can check it.
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

// Newline-delimited messages over a socket, for the tunnel routes: one line at a time, then
// the rest to EOF.
function lineReader(readable) {
  const reader = readable.getReader();
  let buf = '';
  return {
    async line() {
      for (;;) {
        const i = buf.indexOf('\n');
        if (i >= 0) {
          const l = buf.slice(0, i);
          buf = buf.slice(i + 1);
          return l;
        }
        const { value, done } = await reader.read();
        if (done) return null;
        buf += dec.decode(value, { stream: true });
      }
    },
    async rest() {
      let n = buf.length;
      buf = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return n;
        n += value.length;
      }
    },
    cancel: (reason) => reader.cancel(reason),
  };
}

// A WebSocket opened through a binding, with its events queued for the route to await in
// order.
async function wsOpen(fetcher, url, init = {}) {
  const r = await fetcher.fetch(url, {
    ...init,
    headers: { Upgrade: 'websocket' },
  });
  const ws = r.webSocket;
  if (!ws) throw new Error(`no websocket: status=${r.status}`);
  ws.accept();
  const queue = [];
  const waiters = [];
  const push = (ev) => {
    const w = waiters.shift();
    if (w) w(ev);
    else queue.push(ev);
  };
  ws.addEventListener('message', (e) =>
    push({ type: 'message', data: e.data })
  );
  ws.addEventListener('close', (e) =>
    push({ type: 'close', data: `${e.code}:${e.reason}` })
  );
  ws.addEventListener('error', (e) => push({ type: 'error', data: e.message }));
  const next = () =>
    queue.length
      ? Promise.resolve(queue.shift())
      : new Promise((res) => waiters.push(res));
  return { ws, next };
}

// A WebSocket's echo of each of `messages`, sent one at a time.
async function wsEchoes(ws, next, messages) {
  const got = [];
  for (const m of messages) {
    ws.send(m);
    const ev = await next();
    got.push(ev.type === 'message' ? ev.data : `${ev.type}:${ev.data}`);
  }
  return got;
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
        case '/tunnel-echo': {
          // A tunnel the caller writes into first: the callee's read finds the write parked on
          // the pipe, in this isolate's arena.
          const s = env.TUNNEL.connect('back:10');
          const w = s.writable.getWriter();
          const lr = lineReader(s.readable);
          await w.write(enc.encode('ping\n'));
          const got = await lr.line();
          await w.close();
          return new Response(
            `tunnel-echo got=${got} rest=${await lr.rest()}\n`
          );
        }
        case '/tunnel-greet': {
          // A tunnel the callee writes into first: this isolate's read finds the callee's write.
          const s = env.TUNNEL.connect('back:11');
          const lr = lineReader(s.readable);
          const first = await lr.line();
          const w = s.writable.getWriter();
          await w.write(enc.encode('ping\n'));
          const then = await lr.line();
          await w.close();
          return new Response(`tunnel-greet first=${first} then=${then}\n`);
        }
        case '/tunnel-pingpong': {
          // Two hundred round trips of a few bytes, so that both orders of read and write come
          // up, many times.
          const s = env.TUNNEL.connect('back:10');
          const w = s.writable.getWriter();
          const lr = lineReader(s.readable);
          let n = 0;
          for (let i = 0; i < 200; i++) {
            await w.write(enc.encode(`${i}\n`));
            if ((await lr.line()) === `echo:${i}`) n++;
          }
          await w.close();
          await lr.rest();
          return new Response(`tunnel-pingpong n=${n}\n`);
        }
        case '/tunnel-bulk': {
          // A mebibyte each way, in chunks smaller and larger than the relay's staging buffer,
          // with the caller's write side closed before it reads: a half-close.
          const s = env.TUNNEL.connect('back:12');
          const w = s.writable.getWriter();
          const lr = lineReader(s.readable);
          const total = 1048576;
          await w.write(enc.encode(`${total}\n`));
          const chunk = new Uint8Array(4096);
          for (let sent = 0; sent < total; sent += chunk.length) {
            await w.write(chunk);
          }
          await w.close();
          const up = await lr.line();
          const down = await lr.rest();
          return new Response(`tunnel-bulk ${up} down=${down}\n`);
        }
        case '/tunnel-close-parked': {
          // The callee writes twice without waiting to be read; the caller reads once and closes
          // the socket with the second write parked on the pipe. The close drops the caller's end
          // in this turn.
          const s = env.TUNNEL.connect('back:13');
          const lr = lineReader(s.readable);
          const first = await lr.line();
          await s.close();
          return new Response(`tunnel-close-parked first=${first} closed=ok\n`);
        }
        case '/tunnel-throw-mid': {
          // The callee reads one line and throws; the caller's next read must not hang.
          const s = env.TUNNEL.connect('back:14');
          const w = s.writable.getWriter();
          const lr = lineReader(s.readable);
          await w.write(enc.encode('ping\n'));
          let then;
          try {
            const l = await lr.line();
            then = l === null ? 'eof' : `line:${l}`;
          } catch (e) {
            then = `${e.name}:${e.message}`;
          }
          return new Response(`tunnel-throw-mid then=${then}\n`);
        }
        case '/ws-echo': {
          // A WebSocket through the binding: three messages echoed, then closed from here.
          const { ws, next } = await wsOpen(env.BACK, 'http://back/ws-echo');
          const got = await wsEchoes(ws, next, ['one', 'two', 'three']);
          ws.close(1000, 'done');
          const close = await next();
          return new Response(
            `ws-echo got=${got.join(',')} close=${close.type}:${close.data}\n`
          );
        }
        case '/ws-server-first': {
          // The callee sends before this isolate receives anything.
          const { ws, next } = await wsOpen(
            env.BACK,
            'http://back/ws-server-first'
          );
          const first = await next();
          const [then] = await wsEchoes(ws, next, ['x']);
          ws.close(1000, 'done');
          await next();
          return new Response(
            `ws-server-first first=${first.data} then=${then}\n`
          );
        }
        case '/ws-many': {
          // A thousand small messages: five hundred one at a time, five hundred in a burst.
          const { ws, next } = await wsOpen(env.BACK, 'http://back/ws-echo');
          let n = 0;
          for (let i = 0; i < 500; i++) {
            ws.send(`m${i}`);
            if ((await next()).data === `echo:m${i}`) n++;
          }
          for (let i = 500; i < 1000; i++) ws.send(`m${i}`);
          for (let i = 500; i < 1000; i++) {
            if ((await next()).data === `echo:m${i}`) n++;
          }
          ws.close(1000, 'done');
          await next();
          return new Response(`ws-many n=${n}\n`);
        }
        case '/ws-big': {
          // A mebibyte message each way, as text and as bytes.
          const { ws, next } = await wsOpen(env.BACK, 'http://back/ws-echo');
          ws.send('z'.repeat(1048576));
          const text = await next();
          ws.send(new Uint8Array(1048576));
          const bytes = await next();
          ws.close(1000, 'done');
          await next();
          return new Response(
            `ws-big text=${text.data.length} bytes=${bytes.data.byteLength}\n`
          );
        }
        case '/ws-callee-close': {
          // The callee closes after echoing the first message.
          const { ws, next } = await wsOpen(
            env.BACK,
            'http://back/ws-callee-close'
          );
          const [first] = await wsEchoes(ws, next, ['a']);
          const close = await next();
          return new Response(
            `ws-callee-close first=${first} close=${close.type}:${close.data}\n`
          );
        }
        case '/ws-abort': {
          // An upgrade fetch with a signal, aborted after the first echo: the abort drops this
          // isolate's end, with the callee's request attached to it, in this turn.
          const ac = new AbortController();
          const { ws, next } = await wsOpen(env.BACK, 'http://back/ws-echo', {
            signal: ac.signal,
          });
          const [first] = await wsEchoes(ws, next, ['a']);
          ac.abort(new Error('ws-abort'));
          const then = await next();
          return new Response(
            `ws-abort first=${first} then=${then.type}:${then.data}\n`
          );
        }
        case '/ws-do': {
          // A WebSocket served by the Durable Object in a third worker.
          const stub = env.NS.get(env.NS.idFromName('ws'));
          const { ws, next } = await wsOpen(stub, 'http://do/ws');
          const got = await wsEchoes(ws, next, ['hi', 'there']);
          ws.close(1000, 'done');
          await next();
          return new Response(`ws-do got=${got.join(',')}\n`);
        }
        case '/ws-do-hib': {
          // The same object, on the hibernation API: its manager receives, its handler sends.
          const stub = env.NS.get(env.NS.idFromName('ws'));
          const { ws, next } = await wsOpen(stub, 'http://do/ws-hib');
          const got = await wsEchoes(ws, next, ['hi', 'there']);
          ws.close(1000, 'done');
          await next();
          return new Response(`ws-do-hib got=${got.join(',')}\n`);
        }
        case '/ws-proxy': {
          // The client's own upgrade request forwarded to the callee, whose WebSocket is handed
          // back to the client: this isolate's end is pumped by the server, on the event loop.
          return env.BACK.fetch('http://back/ws-echo', req);
        }
        default:
          return new Response('front');
      }
    } catch (e) {
      return new Response(`${name} caught=${e.name}:${e.message}\n`);
    }
  },
};
