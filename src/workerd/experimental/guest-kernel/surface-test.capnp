using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [(name = "surf", worker = (
    modules = [
      (name = "worker", esModule = embed "surface-test.js"),
      (name = "add.wasm", wasm = embed "add.wasm"),
      (name = "oob.wasm", wasm = embed "oob.wasm") ],
    compatibilityDate = "2024-01-01") )],
  sockets = [(name = "surf", address = "127.0.0.1:8897", http = (), service = "surf") ]
);
