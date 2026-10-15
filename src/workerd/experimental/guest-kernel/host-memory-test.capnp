using Workerd = import "/workerd/workerd.capnp";

# Config for host-memory-test.sh: three workers, hence three isolates. "victim" takes the probes
# that do not fault (the handle overwrite, the entropy-buffer poisoning) and then the ring-3 read
# of the channel token key, which condemns it; "victim2" takes the ring-3 read of the host
# entropy buffer, which condemns it; "hello" must keep answering afterwards. The socket addresses
# are placeholders that the script overrides with --socket-addr.
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
    (name = "victim2", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"victim2 ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8895", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8894", http = (), service = "victim"),
    (name = "victim2", address = "127.0.0.1:8893", http = (), service = "victim2"),
  ],
);
