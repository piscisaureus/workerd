using Workerd = import "/workerd/workerd.capnp";

# Config for binding-crossing-test.sh. "plain" is served first so that it holds the first arena
# and no worker under test does. "cdn" and "cdn-origin" are the pair of bench/bench.capnp,
# embedded from there so that the benchmark's own configuration stays covered. "front" drives the
# other crossings: through its binding to "back", which addresses the Durable Object in
# "do-worker" as front does; through "traced", whose tail workers "tail" and "tail-stream" each
# serve what they received; through the Cache API, which "cache-backend" serves; and through a
# worker it loads. All have a compatibility date at which JS RPC is on. The socket addresses are
# placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"plain\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "cdn", worker = (
      modules = [(name = "worker", esModule = embed "bench/cdn-front.js")],
      compatibilityDate = "2024-01-01",
      bindings = [(name = "ORIGIN", service = "cdn-origin")],
    )),
    (name = "cdn-origin", worker = (
      modules = [(name = "worker", esModule = embed "bench/cdn-origin.js")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "front", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-front.js")],
      compatibilityDate = "2024-09-23",
      compatibilityFlags = ["experimental", "enable_ctx_exports"],
      bindings = [
        (name = "BACK", service = "back"),
        (name = "PLAIN", service = "plain"),
        (name = "TRACED", service = "traced"),
        (name = "LOADER", workerLoader = ()),
        (name = "NS", durableObjectNamespace = (className = "Counter", serviceName = "do-worker")),
      ],
      cacheApiOutbound = "cache-backend",
    )),
    (name = "back", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-back.js")],
      compatibilityDate = "2024-09-23",
      compatibilityFlags = ["experimental", "enable_ctx_exports", "allow_irrevocable_stub_storage"],
      bindings = [
        (name = "NS", durableObjectNamespace = (className = "Counter", serviceName = "do-worker")),
      ],
    )),
    (name = "do-worker", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-do.js")],
      compatibilityDate = "2024-09-23",
      compatibilityFlags = ["experimental", "enable_ctx_exports", "allow_irrevocable_stub_storage"],
      durableObjectNamespaces = [(className = "Counter", uniqueKey = "binding-crossing-counter")],
      durableObjectStorage = (inMemory = void),
    )),
    (name = "cache-backend", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-cache.js")],
      compatibilityDate = "2024-09-23",
    )),
    (name = "traced", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-traced.js")],
      compatibilityDate = "2024-09-23",
      tails = ["tail"],
      streamingTails = ["tail-stream"],
    )),
    (name = "tail", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-tail.js")],
      compatibilityDate = "2024-09-23",
    )),
    (name = "tail-stream", worker = (
      modules = [(name = "worker", esModule = embed "binding-crossing-tail-stream.js")],
      compatibilityDate = "2024-09-23",
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8880", http = (), service = "plain"),
    (name = "cdn", address = "127.0.0.1:8881", http = (), service = "cdn"),
    (name = "front", address = "127.0.0.1:8882", http = (), service = "front"),
    (name = "tail", address = "127.0.0.1:8883", http = (), service = "tail"),
    (name = "tail-stream", address = "127.0.0.1:8884", http = (), service = "tail-stream"),
  ],
);
