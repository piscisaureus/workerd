// A small, realistic serialize/parse round trip per request.
export default {
  async fetch(request) {
    const payload = {
      id: 12345,
      name: "widget",
      tags: ["alpha", "beta", "gamma"],
      items: Array.from({length: 32}, (_, i) => ({i, v: i * i, s: "row" + i})),
    };
    const text = JSON.stringify(payload);
    const back = JSON.parse(text);
    return Response.json({count: back.items.length, bytes: text.length});
  },
};
