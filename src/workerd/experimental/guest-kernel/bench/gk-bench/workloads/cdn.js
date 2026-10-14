// Fetches an object from an origin worker and forwards it. Two isolates are
// entered per request (this one and the origin's), so a per-turn or per-switch
// cost is paid twice, and the body crossing makes it the most I/O-heavy of the
// five workloads.
export default {
  async fetch(request, env) {
    const upstream = await env.origin.fetch("http://origin/object");
    return new Response(upstream.body, {headers: upstream.headers});
  },
};
