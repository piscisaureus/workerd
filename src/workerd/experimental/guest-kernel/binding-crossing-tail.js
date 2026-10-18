// The buffered tail worker of binding-crossing-test.sh: keeps what each trace carried and
// serves it on its own socket, one line per trace.
const records = [];
export default {
  async tail(events) {
    for (const e of events) {
      records.push(
        JSON.stringify({
          url: e.event?.request?.url,
          logs: e.logs.map((l) => l.message),
          exceptions: e.exceptions.map((x) => x.message),
          outcome: e.outcome,
        })
      );
    }
  },
  async fetch() {
    return new Response(records.join('\n') + '\n');
  },
};
