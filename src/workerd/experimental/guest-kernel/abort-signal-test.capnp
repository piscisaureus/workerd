using Workerd = import "/workerd/workerd.capnp";

# Config for abort-signal-test.sh: "victim" serves /__gk_abort_cell_corrupt,
# /__gk_abort_cell_live and /__gk_abort_cell_dropped, which drive an AbortSignal's native abort
# registration from inside the turn (see maybeForceGuestFaultForTest in
# src/workerd/api/global-scope.c++); "fetcher" serves /fetch-abort, which aborts a real
# subrequest through a service binding; "hello" must keep answering. The socket addresses are
# placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"victim ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "fetcher", worker = (
      modules = [(name = "worker", esModule = embed "abort-signal-test.js")],
      compatibilityDate = "2024-01-01",
      compatibilityFlags = ["experimental"],
      bindings = [(name = "BACK", service = "back")],
    )),
    (name = "back", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { await new Promise(r => setTimeout(r, 5000)); return new Response(\"slow\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8875", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8874", http = (), service = "victim"),
    (name = "fetcher", address = "127.0.0.1:8873", http = (), service = "fetcher"),
  ],
);
