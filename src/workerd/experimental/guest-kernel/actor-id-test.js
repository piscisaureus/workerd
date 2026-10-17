// Worker for actor-id-test.sh: mints and resolves Durable Object ids from inside JS turns, and
// reaches a Durable Object by a minted id. The Counter keeps its count in memory, so the count a
// request gets back tells whether it reached the same object as the request before it.
export class Counter {
  constructor(state, env) {
    this.count = 0;
  }
  async fetch(req) {
    this.count++;
    return new Response(String(this.count));
  }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const name = url.searchParams.get('n');
    switch (url.pathname) {
      case '/mint':
        return new Response(env.NS.newUniqueId().toString());
      case '/name':
        return new Response(env.NS.idFromName(name).toString());
      case '/long-name': {
        // A name longer than one hostcall frame carries, repeated to the requested length.
        const len = Number(url.searchParams.get('len'));
        return new Response(
          env.NS.idFromName(
            name.repeat(Math.ceil(len / name.length)).slice(0, len)
          ).toString()
        );
      }
      case '/do': {
        const id = env.NS.idFromString(url.searchParams.get('id'));
        return env.NS.get(id).fetch('http://do/');
      }
      case '/do-name':
        return env.NS.get(env.NS.idFromName(name)).fetch('http://do/');
      case '/forged': {
        // A 64-hex id whose second half is not the HMAC of its first must be refused.
        try {
          env.NS.idFromString('00'.repeat(16) + '11'.repeat(16));
          return new Response('accepted');
        } catch (e) {
          return new Response('rejected: ' + e.message);
        }
      }
      default:
        return new Response('victim ok\n');
    }
  },
};
