using Workerd = import "/workerd/workerd.capnp";

# Config for eh-globals-cross-test.sh. "victim" is a modules worker configured with a module
# fallback service (moduleFallback plus --experimental, server.c++), so the real
# FallbackServiceClient thread runs: a thread that throws and catches a C++ exception on every
# failed fetch while holding no isolate lock. It serves /__gk_eh_arm, /__gk_eh_poison and
# /__gk_eh_check, which start a stand-in for that thread, write its caughtExceptions pointer from
# inside a JS turn and report whether the steer reaches the forged cleanup pointer (see
# maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++), and /__gk_fallback_poke, which
# drives a real failed resolution so the real fallback thread throws and catches. "hello" must
# keep answering. The fallback address points at a closed port so every resolution fails; the
# socket addresses are placeholders the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = embed "eh-globals-cross-test.js")],
      compatibilityDate = "2024-01-01",
      moduleFallback = "127.0.0.1:1",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8819", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8818", http = (), service = "victim"),
  ],
);
