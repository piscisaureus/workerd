// The RPC callee of the census workload: a service-binding fetch and a JS RPC
// entrypoint whose return value becomes a stub, so a call carries a Cap'n Proto
// message arena built under the caller's lock and read from the callee's turn.
import { WorkerEntrypoint } from 'cloudflare:workers';

export class Api extends WorkerEntrypoint {
  getThing(i) {
    return {
      hello() {
        return 'hi';
      },
      n: i,
    };
  }
}

export default {
  async fetch() {
    return new Response('callee\n');
  },
};
