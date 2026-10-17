using Workerd = import "/workerd/workerd.capnp";

# Config for lock-slot-test.sh: "victim" serves /__gk_lock_slot, /__gk_lock_announce and
# /__gk_lock_check, "attacker" serves /__gk_lock_poison_other, which overwrites the victim
# isolate's lock slot from inside the attacker's turn (see maybeForceGuestFaultForTest in
# src/workerd/api/global-scope.c++); "hello" must keep answering. The socket addresses are
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
    (name = "attacker", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"attacker ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8835", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8834", http = (), service = "victim"),
    (name = "attacker", address = "127.0.0.1:8833", http = (), service = "attacker"),
  ],
);
