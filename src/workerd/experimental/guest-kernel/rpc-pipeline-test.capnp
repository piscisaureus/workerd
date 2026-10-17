using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    # A worker that serves before the others do, so that the arenas the calls cross into are
    # not the first one and a fault cannot be mistaken for one of the first arena's own pages.
    (name = "plain", worker = (
      modules = [(name = "worker", esModule = embed "rpc-arena-plain.js")],
      compatibilityDate = "2024-10-01",
    )),
    # A third isolate whose stubs the callee hands to the caller.
    (name = "far", worker = (
      modules = [(name = "worker", esModule = embed "rpc-pipeline-far.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
    )),
    (name = "callee", worker = (
      modules = [(name = "worker", esModule = embed "rpc-pipeline-callee.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "FAR", service = (name = "far", entrypoint = "FarApi")),
      ],
    )),
    (name = "caller", worker = (
      modules = [(name = "worker", esModule = embed "rpc-pipeline-caller.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "CALLEE", service = (name = "callee", entrypoint = "Api")),
      ],
    )),
  ],
  sockets = [
    (name = "plain", address = "127.0.0.1:8870", http = (), service = "plain"),
    (name = "caller", address = "127.0.0.1:8871", http = (), service = "caller"),
  ],
);
