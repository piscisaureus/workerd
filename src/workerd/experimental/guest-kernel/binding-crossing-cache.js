// The Cache API backend of binding-crossing-test.sh, a worker over cacheApiOutbound: a PUT
// stores the serialized response under its url, a GET answers with its body or 504.
const store = new Map();
export default {
  async fetch(req) {
    if (req.method === 'PUT') {
      store.set(req.url, await req.text());
      return new Response(null, { status: 204 });
    }
    const s = store.get(req.url);
    if (s === undefined) {
      return new Response(null, {
        status: 504,
        headers: { 'CF-Cache-Status': 'MISS' },
      });
    }
    return new Response(s.slice(s.indexOf('\r\n\r\n') + 4), {
      headers: { 'CF-Cache-Status': 'HIT' },
    });
  },
};
