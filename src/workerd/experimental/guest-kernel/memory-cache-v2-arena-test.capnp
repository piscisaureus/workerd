using Workerd = import "/workerd/workerd.capnp";

# Config for memory-cache-v2-arena-test.sh: four workers, hence four isolates. "owner" and
# "sharer" are configured with one memory cache id ("gk-v2-shared") and so share a cache
# instance; "owner" is listed first, which makes its isolate the one whose bindings are
# compiled first and therefore the one whose arena the instance is allocated in. "solo-a" and
# "solo-b" give their bindings no id, so each gets an instance of its own. The socket addresses
# are placeholders that the script overrides with --socket-addr.
#
# The `autogates` entry selects the Rust memory cache (api/memory-cache-v2.c++) on paper. It
# does not take effect: the script says why, and shows it.
const config :Workerd.Config = (
  autogates = ["workerd-autogate-memory-cache-v2"],
  services = [
    (name = "owner", worker = .owner),
    (name = "sharer", worker = .sharer),
    (name = "soloA", worker = .soloA),
    (name = "soloB", worker = .soloB),
  ],
  sockets = [
    (name = "owner", address = "127.0.0.1:8820", http = (), service = "owner"),
    (name = "sharer", address = "127.0.0.1:8821", http = (), service = "sharer"),
    (name = "soloA", address = "127.0.0.1:8822", http = (), service = "soloA"),
    (name = "soloB", address = "127.0.0.1:8823", http = (), service = "soloB"),
  ],
);

const owner :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-v2-arena-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "owner"),
    (name = "CACHE", memoryCache = (
      id = "gk-v2-shared",
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);

const sharer :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-v2-arena-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "sharer"),
    (name = "CACHE", memoryCache = (
      id = "gk-v2-shared",
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);

const soloA :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-v2-arena-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "solo-a"),
    (name = "CACHE", memoryCache = (
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);

const soloB :Workerd.Worker = (
  modules = [(name = "worker", esModule = embed "memory-cache-v2-arena-test.js")],
  compatibilityDate = "2024-01-01",
  bindings = [
    (name = "NAME", text = "solo-b"),
    (name = "CACHE", memoryCache = (
      limits = (maxKeys = 10, maxValueSize = 1024, maxTotalValueSize = 4096),
    )),
  ],
);
