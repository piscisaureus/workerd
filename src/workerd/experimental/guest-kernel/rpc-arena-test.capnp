using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    # A worker that serves before the pair does, so that the arena the pair's turns cross into
    # is not the first one and a fault cannot be mistaken for one of the first arena's own pages.
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = embed "rpc-arena-plain.js")],
      compatibilityDate = "2024-10-01",
    )),
    (name = "callee", worker = (
      modules = [(name = "worker", esModule = embed "rpc-arena-callee.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
    )),
    (name = "caller", worker = (
      modules = [(name = "worker", esModule = embed "rpc-arena-caller.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "CALLEE", service = (name = "callee", entrypoint = "Api")),
        (name = "CALLEE_FETCH", service = "callee"),
      ],
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8866", http = (), service = "plain"),
    (name = "caller", address = "127.0.0.1:8867", http = (), service = "caller"),
  ],
);
