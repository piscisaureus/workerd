// Driven by binding-crossing-test.sh through front's service binding: echoes the cf blob a
// request carries, fails a fetch, a tunnel or an RPC call on request, streams slowly, takes and
// gives streams over RPC, addresses the Durable Object front addresses, restores itself, serves
// tunnels through its Tunnel entrypoint and WebSockets through its fetch handler.
import { WorkerEntrypoint, restore } from 'cloudflare:workers';

const enc = new TextEncoder();
const dec = new TextDecoder();

// What [restore]() hands back: a loopback stub to this entrypoint, with props.
export class Target extends WorkerEntrypoint {
  ping() {
    return `pong:${this.ctx.props.p}`;
  }
}

// Newline-delimited messages over a socket, as in binding-crossing-front.js.
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
  };
}

// The tunnels front opens, told apart by the port of the address they were opened to.
const TUNNELS = {
  10: 'echo',
  11: 'greet',
  12: 'bulk',
  13: 'parked',
  14: 'throw-mid',
};
export class Tunnel extends WorkerEntrypoint {
  async connect(socket) {
    const { localAddress } = await socket.opened;
    const kind = TUNNELS[+localAddress.split(':')[1]];
    const w = socket.writable.getWriter();
    const lr = lineReader(socket.readable);
    switch (kind) {
      case 'echo': {
        // Reads first; echoes each line until the caller's EOF.
        for (;;) {
          const l = await lr.line();
          if (l === null) break;
          await w.write(enc.encode(`echo:${l}\n`));
        }
        await w.close();
        return;
      }
      case 'greet': {
        // Writes first.
        await w.write(enc.encode('hello\n'));
        const l = await lr.line();
        await w.write(enc.encode(`got:${l}\n`));
        await w.close();
        return;
      }
      case 'bulk': {
        // Reads the announced number of bytes to the caller's EOF, reports the count, then
        // sends as many back in 16 KiB chunks.
        const total = +(await lr.line());
        const up = await lr.rest();
        await w.write(enc.encode(`up=${up}\n`));
        const chunk = new Uint8Array(16384);
        for (let sent = 0; sent < total; sent += chunk.length) {
          await w.write(chunk);
        }
        await w.close();
        return;
      }
      case 'parked': {
        // Two writes before anything is read; the second stays parked until the caller reads
        // it or drops its end, which rejects it.
        await w.write(enc.encode('a\n'));
        try {
          await w.write(enc.encode('b\n'));
          await w.close();
        } catch {}
        return;
      }
      case 'throw-mid': {
        await lr.line();
        throw new Error('tunnel boom');
      }
      default:
        throw new Error(`unknown tunnel ${localAddress}`);
    }
  }
}

// A WebSocket pair whose server side echoes, sends first or closes after the first message,
// by route.
function echoPair(mode) {
  const pair = new WebSocketPair();
  const [client, server] = Object.values(pair);
  server.accept();
  if (mode === 'server-first') server.send('hello');
  server.addEventListener('message', (e) => {
    // Text comes back prefixed, bytes as they are.
    server.send(typeof e.data === 'string' ? `echo:${e.data}` : e.data);
    if (mode === 'callee-close') server.close(4000, 'callee-close');
  });
  server.addEventListener('close', (e) => {
    try {
      server.close(e.code, e.reason);
    } catch {}
  });
  return new Response(null, { status: 101, webSocket: client });
}

export default class extends WorkerEntrypoint {
  async fetch(req) {
    const url = new URL(req.url);
    switch (url.pathname) {
      case '/ws-echo':
        return echoPair('echo');
      case '/ws-server-first':
        return echoPair('server-first');
      case '/ws-callee-close':
        return echoPair('callee-close');
      case '/forward':
      case '/cf':
        return new Response(JSON.stringify(req.cf));
      case '/throw':
        throw new Error('boom');
      case '/reject':
        return Promise.reject(new TypeError('rejected'));
      case '/nonresponse':
        return 42;
      case '/body-error':
        return new Response(
          new ReadableStream({
            start(c) {
              c.enqueue(enc.encode('a'));
            },
            pull(c) {
              c.error(new Error('mid'));
            },
          })
        );
      case '/body-error-up': {
        try {
          return new Response(`read=${await req.text()}`);
        } catch (e) {
          return new Response(`caught=${e.name}:${e.message}`);
        }
      }
      case '/slow':
        // Five chunks, 300 ms apart.
        return new Response(
          new ReadableStream({
            async pull(c) {
              for (let i = 0; i < 5; i++) {
                c.enqueue(enc.encode('y'.repeat(1000)));
                await new Promise((r) => setTimeout(r, 300));
              }
              c.close();
            },
          })
        );
      case '/ws-reject':
        return new Response('no websocket here', { status: 400 });
      case '/restore': {
        // ctx.restore() reaches the self-token factory the caller's turn made for this request.
        const stub = await this.ctx.restore({ p: 'r' });
        return new Response(`restored ${await stub.ping()}`);
      }
      case '/do': {
        const r = await this.env.NS.get(this.env.NS.idFromName('shared')).fetch(
          'http://do/'
        );
        return new Response(`back-do ${await r.text()}`);
      }
      default:
        return new Response('back');
    }
  }
  async connect(socket) {
    throw new RangeError('connect boom');
  }
  ok() {
    return 42;
  }
  boom() {
    throw new RangeError('rpc boom');
  }
  async rejectLater() {
    await new Promise((r) => setTimeout(r, 1));
    throw new TypeError('rpc rejected');
  }
  async takeStream(s) {
    let n = 0;
    const reader = s.getReader();
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      n += value.length;
    }
    return n;
  }
  giveStream() {
    return new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('q'.repeat(70000)));
        c.close();
      },
    });
  }
  async [restore](params) {
    return this.ctx.exports.Target({ props: { p: params.p } });
  }
  async callStub(stub) {
    return await stub.hello();
  }
}
