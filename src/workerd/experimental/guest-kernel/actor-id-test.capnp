using Workerd = import "/workerd/workerd.capnp";

# Config for actor-id-test.sh: "victim" has a Durable Object namespace whose unique key the
# script knows, so it can compute the ids the namespace must derive, and takes the ring-3 read of
# the namespace's HMAC key last, which condemns it; "hello" must keep answering afterwards. The
# socket addresses are placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = embed "actor-id-test.js")],
      compatibilityDate = "2024-01-01",
      durableObjectNamespaces = [(className = "Counter", uniqueKey = "actor-id-test-unique-key")],
      durableObjectStorage = (inMemory = void),
      bindings = [(name = "NS", durableObjectNamespace = "Counter")],
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8885", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8884", http = (), service = "victim"),
  ],
);
