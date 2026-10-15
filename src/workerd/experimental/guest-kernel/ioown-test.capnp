using Workerd = import "/workerd/workerd.capnp";

# Config for ioown-test.sh: "victim" takes the in-turn rewrite of an IoOwn's handle, "other"
# announces an IoOwn of its own for victim's cross-isolate rewrite, "node" takes the in-turn
# rewrite of an owned node, which faults and condemns its isolate, and "hello" must keep answering
# throughout. The socket addresses are placeholders that the script overrides with --socket-addr.
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
    (name = "other", worker = (
      # The announcing request stays open for three seconds, so its IoContext -- and the owned
      # object its IoOwn names -- outlive the victim's steal, and end before the second one.
      modules = [(name = "worker", esModule = "export default { async fetch(req) { if (new URL(req.url).pathname === '/__gk_ioown_announce') await new Promise(r => setTimeout(r, 3000)); return new Response(\"other ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "node", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"node ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8885", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8884", http = (), service = "victim"),
    (name = "other", address = "127.0.0.1:8883", http = (), service = "other"),
    (name = "node", address = "127.0.0.1:8882", http = (), service = "node"),
  ],
);
