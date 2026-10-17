// Both: a large stable JS heap and 4 MiB bodies from a kept buffer.
let heap = null;
function build(n) {
  const a = [];
  for (let i = 0; i < n; i++)
    a.push({ i, s: 'item-' + i, t: [i, i + 1, i + 2] });
  return a;
}
const buf = new Uint8Array(4194304);
export default {
  fetch() {
    if (heap === null) heap = build(500000);
    return new Response(buf);
  },
};
