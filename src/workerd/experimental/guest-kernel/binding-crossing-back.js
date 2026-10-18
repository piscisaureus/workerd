// Driven by binding-crossing-test.sh through front's service binding: echoes the cf blob a
// request carries, fails a fetch, a tunnel or an RPC call on request, streams slowly, takes and
// gives streams over RPC, addresses the Durable Object front addresses, and restores itself.
import { WorkerEntrypoint, restore } from 'cloudflare:workers';

const enc = new TextEncoder();

// What [restore]() hands back: a loopback stub to this entrypoint, with props.
export class Target extends WorkerEntrypoint {
  ping() {
    return `pong:${this.ctx.props.p}`;
  }
}

export default class extends WorkerEntrypoint {
  async fetch(req) {
    const url = new URL(req.url);
    switch (url.pathname) {
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
