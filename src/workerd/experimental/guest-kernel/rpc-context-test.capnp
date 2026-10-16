using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "callee", worker = (
      modules = [(name = "worker", esModule = embed "rpc-context-callee.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
    )),
    (name = "caller", worker = (
      modules = [(name = "worker", esModule = embed "rpc-context-caller.js")],
      compatibilityDate = "2024-10-01",
      compatibilityFlags = ["experimental"],
      bindings = [
        (name = "CALLEE", service = (name = "callee", entrypoint = "Api")),
        (name = "CALLEE_FETCH", service = "callee"),
      ],
    )),
  ],
  sockets = [
    (name = "caller", address = "127.0.0.1:8865", http = (), service = "caller"),
  ],
);
