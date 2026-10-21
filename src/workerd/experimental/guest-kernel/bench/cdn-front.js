// Forwards each request to the origin worker over a service binding: two isolates per request.
export default {
  async fetch(request, env) {
    const r = await env.ORIGIN.fetch(request);
    return new Response(r.body, r);
  },
};
