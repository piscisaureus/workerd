# Writes a config with N trivial but distinct workers, each on its own socket,
# so that one workerd process holds N live isolates.
#
# The workers must differ: identical module text would be one script, and the
# point is to pay the per-isolate cost N times. Each module is inlined with its
# index baked in rather than embedded from a file, which keeps the generated
# config self-contained.
#
# usage: iso-gen.py <N> [base-port] > iso.capnp
import sys

n = int(sys.argv[1])
base = int(sys.argv[2]) if len(sys.argv) > 2 else 9400

services, sockets = [], []
for i in range(n):
    # Single-quoted in JS and the newline escaped twice: capnp unescapes the
    # text once on its way into the module source.
    src = ('export default { async fetch(request) { '
           f"return new Response('worker {i}" + r"\\n" + "'); } };")
    services.append(
        f'  (name = "w{i}", worker = (\n'
        f'    modules = [(name = "worker", esModule = "{src}")],\n'
        f'    compatibilityDate = "2025-01-01",\n'
        f'  )),')
    sockets.append(
        f'  (name = "s{i}", address = "127.0.0.1:{base + i}", http = (), '
        f'service = "w{i}"),')

print('using Workerd = import "/workerd/workerd.capnp";')
print()
print("const config :Workerd.Config = (")
print("  services = [")
print("\n".join(services))
print("  ],")
print("  sockets = [")
print("\n".join(sockets))
print("  ]")
print(");")
