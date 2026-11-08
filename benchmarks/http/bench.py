import http.client, os, time, sys

PORT = int(os.environ.get("BENCH_PORT", "8901"))

path = os.environ["BENCH_PATH"]
n = int(os.environ["BENCH_N"])
keepalive = os.environ.get("BENCH_KEEPALIVE") == "1"

def once():
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=30)
    c.request("GET", path)
    r = c.getresponse()
    b = r.read()
    c.close()
    return len(b)

if keepalive:
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=30)
    def once_ka():
        conn.request("GET", path)
        r = conn.getresponse()
        return len(r.read())

once()  # warm
total = 0
t0 = time.perf_counter()
for _ in range(n):
    total += once_ka() if keepalive else once()
t1 = time.perf_counter()
print("%.0f %d" % ((t1 - t0) * 1000.0, total))
