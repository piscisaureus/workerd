using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    # Serves first, so the chain's blocks that any turn allocates are allocated by an isolate
    # that is not the one the formatters run in.
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = embed "atexit-arena-plain.js")],
      compatibilityDate = "2024-10-01",
    )),
    (name = "fmt", worker = (
      modules = [(name = "worker", esModule = embed "atexit-arena-fmt.js")],
      compatibilityDate = "2024-10-01",
    )),
    (name = "fmt2", worker = (
      modules = [(name = "worker", esModule = embed "atexit-arena-fmt.js")],
      compatibilityDate = "2024-10-01",
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8868", http = (), service = "plain"),
    (name = "fmt", address = "127.0.0.1:8869", http = (), service = "fmt"),
    (name = "fmt2", address = "127.0.0.1:8870", http = (), service = "fmt2"),
  ],
);
