using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (
      name = "cdn",
      worker = (
        modules = [(name = "worker", esModule = embed "cdn.js")],
        compatibilityDate = "2025-01-01",
        bindings = [(name = "origin", service = "origin")],
      ),
    ),
    (
      name = "origin",
      worker = (
        modules = [(name = "worker", esModule = embed "origin.js")],
        compatibilityDate = "2025-01-01",
      ),
    ),
  ],
  sockets = [(name = "http", address = "127.0.0.1:9300", http = (), service = "cdn")],
);
