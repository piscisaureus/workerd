using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"plain\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "front", worker = (
      modules = [(name = "worker", esModule = embed "../../heap-region-front.js")],
      compatibilityDate = "2024-01-01",
      bindings = [(name = "BACK", service = "back")],
    )),
    (name = "back", worker = (
      modules = [(name = "worker", esModule = embed "../../heap-region-back.js")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8902", http = (), service = "plain"),
    (name = "front", address = "127.0.0.1:8903", http = (), service = "front"),
  ],
  v8Flags = ["--max-old-space-size=128", "--trace-gc"],
);
