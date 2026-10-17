using Workerd = import "/workerd/workerd.capnp";

# The allocation census's phase experiment (see memory-classes.md and
# census-run.sh): "wa" and "wb" run the same module, so a site that grows while
# wa serves and not while wb serves is state wa's isolate established for the
# process. "callee" is the RPC target both of them call. The socket addresses
# are placeholders that census-run.sh overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "callee", worker = (
      modules = [(name = "worker", esModule = embed "census-callee.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
    )),
    (name = "wa", worker = (
      modules = [
        (name = "worker", esModule = embed "census-worker.js"),
        (name = "census-types.wasm", wasm = embed "census-types.wasm"),
      ],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "CALLEE", service = (name = "callee", entrypoint = "Api")),
        (name = "CALLEE_FETCH", service = "callee"),
      ],
    )),
    (name = "wb", worker = (
      modules = [
        (name = "worker", esModule = embed "census-worker.js"),
        (name = "census-types.wasm", wasm = embed "census-types.wasm"),
      ],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "CALLEE", service = (name = "callee", entrypoint = "Api")),
        (name = "CALLEE_FETCH", service = "callee"),
      ],
    )),
  ],
  sockets = [
    (name = "wa", address = "127.0.0.1:8861", http = (), service = "wa"),
    (name = "wb", address = "127.0.0.1:8862", http = (), service = "wb"),
  ],
);
