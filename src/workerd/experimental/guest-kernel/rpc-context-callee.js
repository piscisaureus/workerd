import { WorkerEntrypoint } from 'cloudflare:workers';

export class Api extends WorkerEntrypoint {
  // A plain object comes back as a stub, so the callee exports a
  // TransientJsRpcTarget: an IoOwn of the originating call and a ReverseIoOwn of the
  // JS handles, both registered against this worker's own IoContext from this worker's
  // own turn -- the context the caller's turn built.
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
