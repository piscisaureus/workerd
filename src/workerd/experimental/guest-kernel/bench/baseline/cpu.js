export default {
  async fetch(request) {
    // Roughly 1 ms of JSON and string work per request.
    let s = 0;
    for (let i = 0; i < 2000; i++) {
      const o = { i, name: "item" + i, tags: ["a", "b", "c"], nested: { x: i * 2 } };
      s += JSON.stringify(o).length + JSON.parse(JSON.stringify(o)).nested.x;
    }
    return new Response(String(s) + "\n");
  }
};
