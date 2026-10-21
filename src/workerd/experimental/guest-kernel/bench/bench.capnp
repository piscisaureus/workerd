using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "hello", worker = .hello),
    (name = "cpu", worker = .cpu),
    (name = "json", worker = .json),
    (name = "cdn", worker = .cdnFront),
    (name = "cdn-origin", worker = .cdnOrigin),
    (name = "alloc", worker = .alloc),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:9001", http = (), service = "hello"),
    (name = "cpu", address = "127.0.0.1:9002", http = (), service = "cpu"),
    (name = "json", address = "127.0.0.1:9003", http = (), service = "json"),
    (name = "cdn", address = "127.0.0.1:9004", http = (), service = "cdn"),
    (name = "alloc", address = "127.0.0.1:9005", http = (), service = "alloc"),
  ],
);

const hello :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "hello.js")],
  compatibilityDate = "2024-01-01",
);
const cpu :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "cpu.js")],
  compatibilityDate = "2024-01-01",
);
const json :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "json.js")],
  compatibilityDate = "2024-01-01",
);
const cdnFront :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "cdn-front.js")],
  compatibilityDate = "2024-01-01",
  bindings = [(name = "ORIGIN", service = "cdn-origin")],
);
const cdnOrigin :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "cdn-origin.js")],
  compatibilityDate = "2024-01-01",
);
const alloc :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "alloc.js")],
  compatibilityDate = "2024-01-01",
);
