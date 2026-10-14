// The floor on per-request cost: no allocation, no parsing, no I/O.
export default {
  async fetch(request) {
    return new Response("Hello World\n");
  },
};
