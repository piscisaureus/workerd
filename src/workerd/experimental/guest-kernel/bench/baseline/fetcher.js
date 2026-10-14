export default {
  async fetch(request, env) {
    // Two subrequests through a service binding, bodies read in full.
    const a = await env.hello.fetch("http://hello/");
    const b = await env.hello.fetch("http://hello/");
    return new Response((await a.text()) + (await b.text()));
  }
};
