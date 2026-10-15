using Workerd = import "/workerd/workerd.capnp";

# Config for wasm-signal-test.sh: "victim" instantiates a Wasm module that exports
# __instance_signal (signal.wasm, built from io/wasm/signal-basic.wat) and burns CPU reading the
# field until the shutdown signal lands in it; "hello" must keep answering throughout. The socket
# addresses are placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [
        (name = "worker", esModule = embed "wasm-signal-test.js"),
        (name = "signal.wasm", wasm = embed "signal.wasm"),
      ],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8875", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8874", http = (), service = "victim"),
  ],
);
