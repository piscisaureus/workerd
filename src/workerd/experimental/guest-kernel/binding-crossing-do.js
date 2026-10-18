// The Durable Object of binding-crossing-test.sh, in a worker of its own: front and back both
// address it, so its class is looked up and its container made from turns of other isolates,
// it restores itself on request, and it serves WebSockets, accepted and hibernatable.
import { DurableObject, WorkerEntrypoint, restore } from 'cloudflare:workers';

export class Target extends WorkerEntrypoint {
  ping() {
    return `pong:${this.ctx.props.p}`;
  }
}

export class Counter extends DurableObject {
  n = 0;
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === '/restore') {
      const stub = await this.ctx.restore({ p: 'd' });
      return new Response(`restored ${await stub.ping()}`);
    }
    if (url.pathname === '/ws') {
      // A WebSocket echoed from this object's own turns.
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      server.accept();
      server.addEventListener('message', (e) => server.send(`do:${e.data}`));
      server.addEventListener('close', (e) => {
        try {
          server.close(e.code, e.reason);
        } catch {}
      });
      return new Response(null, { status: 101, webSocket: client });
    }
    if (url.pathname === '/ws-hib') {
      // The same on the hibernation API: the object's manager receives, webSocketMessage sends.
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      this.ctx.acceptWebSocket(server);
      return new Response(null, { status: 101, webSocket: client });
    }
    this.n++;
    return new Response(`count=${this.n} name=${this.ctx.id.name}`);
  }
  webSocketMessage(ws, message) {
    ws.send(`hib:${message}`);
  }
  webSocketClose(ws, code, reason) {
    try {
      ws.close(code, reason);
    } catch {}
  }
  async [restore](params) {
    return this.ctx.exports.Target({ props: { p: params.p } });
  }
}

export default {
  fetch() {
    return new Response('do-worker');
  },
};
