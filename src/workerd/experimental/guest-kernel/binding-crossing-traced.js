// The traced worker of binding-crossing-test.sh: logs a line and, on request, throws. Its tail
// workers receive the trace.
export default {
  async fetch(req) {
    const url = new URL(req.url);
    console.log(`traced ${url.pathname}`);
    if (url.pathname === '/throw') throw new Error('traced boom');
    return new Response('traced');
  },
};
