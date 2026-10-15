using Workerd = import "/workerd/workerd.capnp";

# Config for tls-test.sh: "victim" serves /__gk_tls_restore, which overwrites from inside the turn
# the heap slot the isolate lock restores Worker::Api's thread-local from (see
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
    (name = "hello", address = "127.0.0.1:8865", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8864", http = (), service = "victim"),
  ],
);
