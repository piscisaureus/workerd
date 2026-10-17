// Driven by heap-region-test.sh through front's service binding: /make answers with a body of
// `n` bytes from a buffer it keeps, so that what each response costs is the body's passage and
// not its construction.
const bufs = new Map();
function buf(n) {
  let b = bufs.get(n);
  if (b === undefined) {
    b = new Uint8Array(n);
    bufs.set(n, b);
  }
  return b;
}

export default {
  async fetch(req) {
    const url = new URL(req.url);
    switch (url.pathname) {
      case '/make':
        return new Response(buf(Number(url.searchParams.get('n') || 4194304)));
      case '/__gk_heap_region':
        return new Response('region');
      case '/__gk_gc':
        return new Response('gc');
      default:
        return new Response('back');
    }
  },
};
