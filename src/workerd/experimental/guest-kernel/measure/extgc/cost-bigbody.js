// Serves a 4 MiB body from a buffer it keeps: each Response keeps a kj copy of it until collected.
const buf = new Uint8Array(4194304);
export default {
  fetch() {
    return new Response(buf);
  },
};
