// Compute-bound: integer hashing in a hot loop, about 1ms per request.
export default {
  fetch() {
    let h = 0x811c9dc5;
    for (let i = 0; i < 2_000_000; i++) {
      h ^= i & 0xff;
      h = Math.imul(h, 0x01000193) >>> 0;
    }
    return new Response(String(h));
  },
};
