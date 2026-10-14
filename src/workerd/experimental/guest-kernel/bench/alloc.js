// Allocates and touches a fresh 4MB buffer per request.
export default {
  fetch() {
    const b = new Uint8Array(4 << 20);
    b.fill(1);
    return new Response(String(b[b.length - 1]));
  },
};
