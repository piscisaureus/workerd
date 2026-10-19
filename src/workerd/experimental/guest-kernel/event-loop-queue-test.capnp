using Workerd = import "/workerd/workerd.capnp";

# Config for event-loop-queue-test.sh: "victim" serves /__gk_queue_head_poison, which rewrites from
# inside the turn the handle the loop names its (host-memory) run queue by (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++); "hello" must keep answering.
# The socket addresses are placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { await new Promise(r => setTimeout(r, 1)); return new Response(\"victim ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8805", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8804", http = (), service = "victim"),
  ],
);
