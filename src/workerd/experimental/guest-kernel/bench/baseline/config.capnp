using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "hello", worker = .hello),
    (name = "fetcher", worker = .fetcher),
    (name = "cpu", worker = .cpu),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:18080", http = (), service = "hello"),
    (name = "fetcher", address = "127.0.0.1:18081", http = (), service = "fetcher"),
    (name = "cpu", address = "127.0.0.1:18082", http = (), service = "cpu"),
  ]
);

const hello :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "hello.js")],
  compatibilityDate = "2025-01-01",
);

const fetcher :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "fetcher.js")],
  compatibilityDate = "2025-01-01",
  bindings = [(name = "hello", service = "hello")],
);

const cpu :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "cpu.js")],
  compatibilityDate = "2025-01-01",
);
