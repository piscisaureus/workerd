using Workerd = import "/workerd/workerd.capnp";

# Config for fault-timeout-test.sh: two workers, hence two isolates, so the test can fault one
# inside a timeout callback and show the other still serving. "victim" takes the forced guest
# fault (WORKERD_GK_TEST_FAULT plus a request for /__gk_fault_timeout, whose callback faults in
# the turn the timer fires); "hello" must keep answering afterwards. The socket addresses are
# placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = embed "fault-timeout-test.js")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8825", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8824", http = (), service = "victim"),
  ],
);
