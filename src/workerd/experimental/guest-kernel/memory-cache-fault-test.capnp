using Workerd = import "/workerd/workerd.capnp";

# Config for memory-cache-fault-test.sh: two workers, hence two isolates, sharing one memory
# cache ("gk-shared"). "victim" takes the forced guest fault inside a cache read's fallback
# (WORKERD_GK_TEST_FAULT plus a request for /__gk_fault_cache_fallback); "peer" must then be
# able to read the same key. The socket addresses are placeholders that the script overrides
# with --socket-addr.
const config :Workerd.Config = (
  services = [
    (name = "victim", worker = .victim),
    (name = "peer", worker = .peer),
  ],
  sockets = [
    (name = "victim", address = "127.0.0.1:8814", http = (), service = "victim"),
    (name = "peer", address = "127.0.0.1:8815", http = (), service = "peer"),
  ],
);

const victim :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-fault-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "victim"),
    (name = "CACHE", memoryCache = (
      id = "gk-shared",
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);

const peer :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-fault-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "peer"),
    (name = "CACHE", memoryCache = (
      id = "gk-shared",
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);
