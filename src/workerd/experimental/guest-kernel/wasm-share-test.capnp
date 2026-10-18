using Workerd = import "/workerd/workerd.capnp";

# Config for wasm-share-test.sh: "first" and "second" are separate workers, each in its own
# isolate and so its own heap arena, that embed the same two Wasm modules. The script says what
# each worker costs the other by reaching V8's process-wide wasm state, and which of its steps
# drives which part of it. "hello" is a bystander that must keep serving. The socket addresses are placeholders that the script
# overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "first", worker = (
      modules = [
        (name = "worker", esModule = embed "wasm-share-test.js"),
        (name = "add.wasm", wasm = embed "add.wasm"),
        (name = "oob.wasm", wasm = embed "oob.wasm"),
      ],
      compatibilityDate = "2024-01-01",
    )),
    (name = "second", worker = (
      modules = [
        (name = "worker", esModule = embed "wasm-share-test.js"),
        (name = "add.wasm", wasm = embed "add.wasm"),
        (name = "oob.wasm", wasm = embed "oob.wasm"),
      ],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8885", http = (), service = "hello"),
    (name = "first", address = "127.0.0.1:8886", http = (), service = "first"),
    (name = "second", address = "127.0.0.1:8887", http = (), service = "second"),
  ],
);
