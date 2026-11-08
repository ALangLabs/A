# Plain-Python blocked matmul (64x64x64) — same workload as
# benchmarks/numerical/matmul.a (A native blocked matmul) and the C/C++
# equivalents. Flat row-major, ikj order, tile 32.
import sys

def matmul(n):
    a = [1.5] * (n * n)
    b = [2.5] * (n * n)
    c = [0.0] * (n * n)
    TILE = 32
    for i in range(n):
        orow = i * n
        arow = i * n
        jj = 0
        while jj < n:
            jend = jj + TILE if jj + TILE < n else n
            for t in range(n):
                av = a[arow + t]
                brow = t * n
                for j in range(jj, jend):
                    c[orow + j] += av * b[brow + j]
            jj = jend
    return c[0]

if __name__ == "__main__":
    reps = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    v = 0.0
    for _ in range(reps):
        v = matmul(64)
    print(v)
