using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (modules = [(name = "worker", esModule = embed "cost-hello.js")], compatibilityDate = "2024-01-01")),
    (name = "bigheap", worker = (modules = [(name = "worker", esModule = embed "cost-bigheap.js")], compatibilityDate = "2024-01-01")),
    (name = "bigbody", worker = (modules = [(name = "worker", esModule = embed "cost-bigbody.js")], compatibilityDate = "2024-01-01")),
    (name = "bigheapbody", worker = (modules = [(name = "worker", esModule = embed "cost-bigheapbody.js")], compatibilityDate = "2024-01-01")),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:9101", http = (), service = "hello"),
    (name = "bigheap", address = "127.0.0.1:9102", http = (), service = "bigheap"),
    (name = "bigbody", address = "127.0.0.1:9103", http = (), service = "bigbody"),
    (name = "bigheapbody", address = "127.0.0.1:9104", http = (), service = "bigheapbody"),
  ],
  v8Flags = [],
);
