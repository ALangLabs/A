// Cross-language benchmark, Node side (v24, JIT warmed).
const N = 20000000;

function bestOf(fn, reps = 3) {
  let best = Infinity;
  for (let i = 0; i < reps; i++) {
    const t0 = process.hrtime.bigint();
    fn();
    const d = Number(process.hrtime.bigint() - t0) / 1e6;
    if (d < best) best = d;
  }
  return best;
}

const intLoop = () => {
  let s = 0;
  for (let i = 0; i < N; i++) s = (s * 31 + i) % 1000000007;
  return s;
};

const floatLoop = () => {
  let x = 1.0;
  for (let i = 0; i < N; i++) x = x * 1.000001 + 0.5;
  return x;
};

const fib = (n) => (n < 2 ? n : fib(n - 1) + fib(n - 2));

const S = 500000;
const genArr = () => {
  const xs = new Array(S);
  let x = 12345;
  for (let i = 0; i < S; i++) {
    x = (x * 1103515245 + 12345) % 2147483648;
    xs[i] = x;
  }
  return xs;
};

const K = 500000;
const dictRun = () => {
  const m = new Map();
  let x = 999;
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < K; i++) {
    x = (x * 1103515245 + 12345) % 2147483648;
    m.set(x % 1000000, i);
  }
  let acc = 0;
  for (let i = 0; i < K; i++) {
    x = (x * 1103515245 + 12345) % 2147483648;
    acc += m.get(x % 1000000) ?? 0;
  }
  return { ms: Number(process.hrtime.bigint() - t0) / 1e6, acc };
};

const M = 200000;
const strRun = () => {
  const t0 = process.hrtime.bigint();
  const parts = new Array(M);
  for (let i = 0; i < M; i++) parts[i] = "item" + i;
  const out = parts.join(",");
  return Number(process.hrtime.bigint() - t0) / 1e6;
};

console.log("int_loop " + bestOf(intLoop).toFixed(3));
console.log("float_loop " + bestOf(floatLoop).toFixed(3));
console.log("fib30 " + bestOf(() => fib(30)).toFixed(3));

let best = Infinity;
for (let r = 0; r < 3; r++) {
  const xs = genArr();
  const t0 = process.hrtime.bigint();
  xs.sort((a, b) => a - b);
  const d = Number(process.hrtime.bigint() - t0) / 1e6;
  if (d < best) best = d;
}
console.log("sort500k " + best.toFixed(3));

best = Infinity;
for (let r = 0; r < 3; r++) {
  const out = dictRun();
  if (out.ms < best) best = out.ms;
}
console.log("dict500k " + best.toFixed(3));

best = Infinity;
for (let r = 0; r < 3; r++) {
  const d = strRun();
  if (d < best) best = d;
}
console.log("str200k " + best.toFixed(3));
