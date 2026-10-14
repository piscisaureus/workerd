using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [(
    name = "cpu",
    worker = (
      modules = [(name = "worker", esModule = embed "cpu.js")],
      compatibilityDate = "2025-01-01",
    ),
  )],
  sockets = [(name = "http", address = "127.0.0.1:9300", http = (), service = "cpu")],
);
