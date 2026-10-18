// The Durable Object of binding-crossing-test.sh, in a worker of its own: front and back both
// address it, so its class is looked up and its container made from turns of other isolates,
// and it restores itself on request.
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
    this.n++;
    return new Response(`count=${this.n} name=${this.ctx.id.name}`);
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
