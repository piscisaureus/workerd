using Workerd = import "/workerd/workerd.capnp";

# Config for fault-test.sh: two workers, hence two isolates, so the test can fault one and show
# the other still serving. "victim" takes the forced guest fault (WORKERD_GK_TEST_FAULT plus a
# request for /__gk_fault); "hello" must keep answering afterwards. The socket addresses are
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
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8899", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8898", http = (), service = "victim"),
  ],
);
