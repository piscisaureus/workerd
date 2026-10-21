// Holds a large, stable JS heap: built on the first request, kept for the life of the isolate.
let heap = null;
function build(n) {
  const a = [];
  for (let i = 0; i < n; i++)
    a.push({ i, s: 'item-' + i, t: [i, i + 1, i + 2] });
  return a;
}
export default {
  fetch() {
    if (heap === null) heap = build(500000);
    return new Response('ok ' + heap.length);
  },
};
