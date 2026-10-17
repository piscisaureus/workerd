// Serves before the formatters do, so that its isolate holds the first arena.
export default {
  async fetch(req) {
    return new Response('plain\n');
  },
};
