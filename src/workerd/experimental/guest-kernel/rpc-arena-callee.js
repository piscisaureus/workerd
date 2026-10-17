import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';

class Thing extends RpcTarget {
  #n;
  constructor(n) {
    super();
    this.#n = n;
  }
  hello() {
    return 'hi' + this.#n;
  }
}

export class Api extends WorkerEntrypoint {
  async getThing(n) {
    return new Thing(n);
  }
  async blob(n) {
    return new Uint8Array(n).fill(7);
  }
}

export default {
  async fetch(req) {
    return new Response('callee\n', { headers: { 'x-callee': 'yes' } });
  },
};
