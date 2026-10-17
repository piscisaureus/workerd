using Workerd = import "/workerd/workerd.capnp";

# Config for log-flood-test.sh: one worker, hence one isolate, which forces refusals in a loop
# and must not be able to drive the process's log output with them. The socket address is a
# placeholder that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "flood", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"flood ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "flood", address = "127.0.0.1:8879", http = (), service = "flood"),
  ],
);
