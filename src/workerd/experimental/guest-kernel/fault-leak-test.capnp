using Workerd = import "/workerd/workerd.capnp";

# Config for fault-leak-test.sh: three workers, hence three isolates. "dtors" takes a forced
# guest fault inside a scope that disallows destroying async objects and "gcflag" one with the
# cppgc-destructor flag set (WORKERD_GK_TEST_FAULT plus a request for /__gk_fault_disallow_dtors
# or /__gk_fault_gc_dtor, see maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++);
# "keeper" must keep answering afterwards and serves /__gk_gc_dtor_check, which reports what the
# host reads of that flag on the event loop once the faults are over. The socket addresses are
# placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "keeper", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"keeper ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "dtors", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"dtors ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "gcflag", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"gcflag ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "keeper", address = "127.0.0.1:8871", http = (), service = "keeper"),
    (name = "dtors", address = "127.0.0.1:8872", http = (), service = "dtors"),
    (name = "gcflag", address = "127.0.0.1:8873", http = (), service = "gcflag"),
  ],
);
