# Sends N requests over a single keep-alive connection and reports the rate.
import http.client, sys, time
port = int(sys.argv[1]); n = int(sys.argv[2])
c = http.client.HTTPConnection("127.0.0.1", port)
t0 = time.time()
for i in range(n):
    c.request("GET", "/")
    r = c.getresponse(); r.read()
    assert r.status == 200, r.status
dt = time.time() - t0
print(f"{n} requests in {dt:.2f}s, {n/dt:.0f} req/s")
