// Driven by body-arena-test.sh: every route makes a subrequest through the BACK service binding
// that carries a body in at least one direction, and reports what came back so that the script
// can check it byte for byte. The bodies are the pattern below, which back computes the same
// way, so neither side has to send the other what to expect.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function pattern(n) {
  return '0123456789abcdef'.repeat(Math.ceil(n / 16)).slice(0, n);
}

// A JS-implemented ReadableStream of `n` bytes of the pattern in 1 KiB chunks: a body that the
// runtime pumps from JS reads, rather than from a buffer it already holds.
function chunkedStream(n) {
  const bytes = new TextEncoder().encode(pattern(n));
  let offset = 0;
  return new ReadableStream({
    pull(controller) {
      if (offset >= bytes.length) {
        controller.close();
        return;
      }
      controller.enqueue(
        bytes.subarray(offset, Math.min(offset + 1024, bytes.length))
      );
      offset += 1024;
    },
  });
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const n = Number(url.searchParams.get('n') || 1000);
    switch (url.pathname) {
      case '/post': {
        // A string body, which the runtime holds in a buffer and pumps from there; back reads
        // it whole with text().
        const r = await env.BACK.fetch('http://back/echo', {
          method: 'POST',
          body: pattern(n),
        });
        const t = await r.text();
        return new Response(`post n=${n} status=${r.status} back=${t}\n`);
      }
      case '/getbody': {
        // No request body; a response body of n bytes read whole with text().
        const r = await env.BACK.fetch(`http://back/body?n=${n}`);
        const t = await r.text();
        return new Response(
          `getbody n=${n} status=${r.status} len=${t.length} ok=${t === pattern(n)}\n`
        );
      }
      case '/stream': {
        // A JS-implemented stream body, pumped from JS; back reads it whole with text().
        const r = await env.BACK.fetch('http://back/echo', {
          method: 'POST',
          body: chunkedStream(n),
          duplex: 'half',
        });
        const t = await r.text();
        return new Response(`stream n=${n} status=${r.status} back=${t}\n`);
      }
      case '/passthrough': {
        // A JS-implemented stream body that back returns as its response body without reading
        // it: the request body's relay feeds the response pipe directly.
        const r = await env.BACK.fetch('http://back/passthrough', {
          method: 'POST',
          body: chunkedStream(n),
          duplex: 'half',
        });
        const t = await r.text();
        return new Response(
          `passthrough n=${n} status=${r.status} len=${t.length} ok=${t === pattern(n)}\n`
        );
      }
      case '/abort-upload': {
        // A streaming upload that produces a chunk every 300 ms, aborted 450 ms in while back is
        // blocked in a read of it: the abort tears the body pump down inside this isolate's turn.
        const ac = new AbortController();
        let pulls = 0;
        const body = new ReadableStream({
          async pull(controller) {
            await sleep(300);
            pulls++;
            controller.enqueue(new Uint8Array(1024));
          },
        });
        setTimeout(() => ac.abort(new Error('abort-upload')), 450);
        const started = Date.now();
        let outcome;
        try {
          const r = await env.BACK.fetch('http://back/slow', {
            method: 'POST',
            body,
            signal: ac.signal,
            duplex: 'half',
          });
          outcome = 'responded:' + r.status;
        } catch (e) {
          outcome = 'rejected:' + e.message;
        }
        return new Response(
          `abort-upload outcome=${outcome} elapsed=${Date.now() - started} pulls=${pulls}\n`
        );
      }
      default:
        return new Response('front');
    }
  },
};
