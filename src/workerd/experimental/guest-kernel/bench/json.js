// Parses and re-serializes a ~20KB JSON document per request.
const doc = JSON.stringify({
  items: Array.from({ length: 200 }, (_, i) => ({
    id: i,
    name: `item-${i}`,
    tags: ['a', 'b', 'c'],
    price: i * 1.25,
    active: i % 2 === 0,
  })),
});

export default {
  fetch() {
    const o = JSON.parse(doc);
    o.items.forEach((it) => (it.price *= 2));
    return new Response(JSON.stringify(o), {
      headers: { 'content-type': 'application/json' },
    });
  },
};
