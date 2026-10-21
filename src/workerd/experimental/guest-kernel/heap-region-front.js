// Driven by heap-region-test.sh: /down reads `count` response bodies of `n` bytes from back
// through the BACK service binding, each whole into an ArrayBuffer, which is the shape of
// traffic that used to fill this isolate's heap arena region (see heap-region-test.sh), and
// reports the bytes read. /back-region has back report its own region, see __gk_heap_region in
// api/global-scope.c++; this isolate's is reported by requesting that path here directly.
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const n = Number(url.searchParams.get('n') || 4194304);
    const count = Number(url.searchParams.get('count') || 1);
    switch (url.pathname) {
      case '/down': {
        let total = 0;
        for (let i = 0; i < count; i++) {
          const r = await env.BACK.fetch(`http://back/make?n=${n}`);
          const b = await r.arrayBuffer();
          total += b.byteLength;
        }
        return new Response(`down n=${n} count=${count} total=${total}\n`);
      }
      case '/back-region': {
        const r = await env.BACK.fetch('http://back/__gk_heap_region');
        return new Response(`back-region status=${r.status}\n`);
      }
      case '/back-gc': {
        // Has back run a full GC, see __gk_gc in api/global-scope.c++.
        const r = await env.BACK.fetch('http://back/__gk_gc');
        return new Response(`back-gc status=${r.status}\n`);
      }
      default:
        return new Response('front');
    }
  },
};
