using Workerd = import "/workerd/workerd.capnp";

# Config for disk-sqlite-test.sh and vfs-root-test.sh: "victim" has a SQLite-backed Durable
# Object namespace stored on local disk, in a directory the script supplies with
# --directory-path, and "hello" must keep answering whatever happens to it. The socket addresses
# are placeholders that the script overrides with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "hello", worker = (
      modules = [(name = "worker", esModule = "export default { async fetch(req) { return new Response(\"hello ok\\n\"); } };")],
      compatibilityDate = "2024-01-01",
    )),
    (name = "victim", worker = (
      modules = [(name = "worker", esModule = embed "disk-sqlite-test.js")],
      compatibilityDate = "2024-01-01",
      durableObjectNamespaces = [(className = "Store", uniqueKey = "disk-sqlite-test-unique-key", enableSql = true)],
      durableObjectStorage = (localDisk = "disk"),
      bindings = [(name = "NS", durableObjectNamespace = "Store")],
    )),
    (name = "disk", disk = (writable = true)),
  ],
  sockets = [
    (name = "hello", address = "127.0.0.1:8855", http = (), service = "hello"),
    (name = "victim", address = "127.0.0.1:8854", http = (), service = "victim"),
  ],
);
