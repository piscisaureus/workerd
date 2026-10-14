// Roughly 1 ms of pure compute per request: no allocation of consequence and
// no I/O, so the request cost is dominated by JIT-compiled JS running in the
// isolate. This is the workload where guest-side cost, not the world switch,
// decides the result.
export default {
  async fetch(request) {
    let s = 0;
    for (let i = 0; i < 2000; i++) {
      const o = {i, name: "item" + i, tags: ["a", "b", "c"], nested: {x: i * 2}};
      s += JSON.stringify(o).length + JSON.parse(JSON.stringify(o)).nested.x;
    }
    return new Response(String(s) + "\n");
  },
};
