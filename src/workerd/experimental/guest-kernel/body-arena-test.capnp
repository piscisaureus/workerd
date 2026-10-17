using Workerd = import "/workerd/workerd.capnp";

# Config for body-arena-test.sh: "front" makes body-bearing subrequests through its service
# binding to "back" and reads the response bodies back; "plain" is served first so that it holds
# the first arena and neither of the other two does. The socket addresses are placeholders that
# the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"plain\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "front", worker = (
      modules = [(name = "worker", esModule = embed "body-arena-front.js")],
      compatibilityDate = "2024-01-01",
      bindings = [(name = "BACK", service = "back")],
    )),
    (name = "back", worker = (
      modules = [(name = "worker", esModule = embed "body-arena-back.js")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8862", http = (), service = "plain"),
    (name = "front", address = "127.0.0.1:8863", http = (), service = "front"),
  ],
);
