import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';

class FarThing extends RpcTarget {
  #n;
  constructor(n) {
    super();
    this.#n = n;
  }
  hello() {
    return 'far' + this.#n;
  }
}

export class FarApi extends WorkerEntrypoint {
  getThing(n) {
    return new FarThing(n);
  }
}

export default {
  async fetch() {
    return new Response('far\n');
  },
};
