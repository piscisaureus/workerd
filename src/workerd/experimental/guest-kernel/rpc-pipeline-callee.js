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
  self() {
    return this;
  }
}

export class Api extends WorkerEntrypoint {
  getThing(n) {
    return new Thing(n);
  }
  async slowThing(n) {
    // Returns after a timer, so calls pipelined on the result are queued when they are sent.
    await new Promise((resolve) => setTimeout(resolve, 10));
    return new Thing(n);
  }
  getObj(n) {
    return {
      hello() {
        return 'obj' + n;
      },
      child: {
        hello() {
          return 'child' + n;
        },
      },
    };
  }
  getFar(n) {
    // A stub on the far worker, passed through to the caller.
    return this.env.FAR.getThing(n);
  }
  boom(n) {
    throw new Error('boom' + n);
  }
  getNumber(n) {
    return n;
  }
  echo(stub) {
    return stub;
  }
}

export default {
  async fetch() {
    return new Response('callee\n');
  },
};
