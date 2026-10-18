// The streaming tail worker of binding-crossing-test.sh: keeps the events of each stream and
// serves them on its own socket, one line per event.
const records = [];
export default {
  tailStream(event) {
    records.push(JSON.stringify({ onset: event.event?.info }));
    return (ev) => {
      records.push(JSON.stringify(ev.event));
    };
  },
  async fetch() {
    return new Response(records.join('\n') + '\n');
  },
};
