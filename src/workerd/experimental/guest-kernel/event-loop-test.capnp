using Workerd = import "/workerd/workerd.capnp";

# Config for event-loop-test.sh: "victim" serves /__gk_loop_repoint and /__gk_loop_clear, which
# rewrite from inside the turn the pointer this thread finds its event loop through (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++); "hello" must keep answering.
# The socket addresses are placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"victim ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8805", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8804", http = (), service = "victim"),
  ],
);
