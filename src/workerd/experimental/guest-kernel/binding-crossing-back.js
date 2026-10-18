// Driven by binding-crossing-test.sh through front's service binding: echoes the cf blob a
// request carries, and fails a fetch or an RPC call on request.
import { WorkerEntrypoint } from 'cloudflare:workers';

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
      default:
        return new Response('back');
    }
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
}
