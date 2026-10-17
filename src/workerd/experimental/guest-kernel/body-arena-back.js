// Driven by body-arena-test.sh through front's service binding: reads request bodies that
// another isolate's turns wrote and writes response bodies that another isolate's turns read.
function pattern(n) {
  return '0123456789abcdef'.repeat(Math.ceil(n / 16)).slice(0, n);
}

export default {
  async fetch(req) {
    const url = new URL(req.url);
    switch (url.pathname) {
      case '/echo': {
        // Reads the request body whole and says how long it was and whether it was intact.
        const t = await req.text();
        return new Response(`len=${t.length} ok=${t === pattern(t.length)}`);
      }
      case '/body': {
        const n = Number(url.searchParams.get('n') || 1000);
        return new Response(pattern(n));
      }
      case '/passthrough':
        return new Response(req.body);
      case '/slow': {
        // Reads the request body chunk by chunk, so that it is blocked in a read of the next
        // chunk whenever the producer is slower than it.
        const reader = req.body.getReader();
        let n = 0;
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          n += value.byteLength;
        }
        return new Response(`slow got ${n}`);
      }
      default:
        return new Response('back');
    }
  },
};
