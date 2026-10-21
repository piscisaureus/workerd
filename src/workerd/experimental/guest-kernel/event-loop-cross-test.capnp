using Workerd = import "/workerd/workerd.capnp";

# Config for event-loop-cross-test.sh: "checker" serves /__gk_loop_other_arm and
# /__gk_loop_other_check, which start a parked host thread with an event loop of its own and read
# that thread's copy of kj's current-loop pointer back host-side; "victim" serves
# /__gk_loop_other_poison, which reads and writes that word from inside its JS turn (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++) and may be condemned for it;
# "hello" must keep answering. The socket addresses are placeholders that the script overrides
# with --socket-addr.
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
    (name = "checker", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"checker ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8808", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8807", http = (), service = "victim"),
    (name = "checker", address = "127.0.0.1:8806", http = (), service = "checker"),
  ],
);
