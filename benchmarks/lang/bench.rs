// Cross-language benchmark, Rust side (rustc -O).
use std::hint::black_box;
use std::time::Instant;

/// Opaque call boundary: without it LLVM CSEs repeated calls with the same
/// argument and the second and third reps measure nothing.
#[inline(never)]
fn timed_fib(n: u64) -> u64 {
    black_box(fib(black_box(n)))
}

fn fib(n: u64) -> u64 {
    if n < 2 {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

fn ms(d: std::time::Duration) -> f64 {
    d.as_secs_f64() * 1000.0
}

fn main() {
    let n: i64 = black_box(20_000_000);

    let mut best = f64::MAX;
    for _ in 0..3 {
        let t = Instant::now();
        let mut s: i64 = 0;
        for i in 0..n {
            s = (s.wrapping_mul(31).wrapping_add(i)) % 1000000007;
        }
        black_box(s);
        best = best.min(ms(t.elapsed()));
    }
    println!("int_loop {:.3}", best);

    let mut best = f64::MAX;
    for _ in 0..3 {
        let t = Instant::now();
        let mut x: f64 = 1.0;
        for _ in 0..n {
            x = x * 1.000001 + 0.5;
        }
        black_box(x);
        best = best.min(ms(t.elapsed()));
    }
    println!("float_loop {:.3}", best);

    let arg = black_box(std::env::args().count() as u64 + 29);
    let mut best = f64::MAX;
    for r in 0..3u64 {
        /* loop-variant call: black_box(r) & 0 keeps the value, not the hoisting */
        let a = arg + (black_box(r) & 0);
        let t = Instant::now();
        black_box(timed_fib(a));
        best = best.min(ms(t.elapsed()));
    }
    println!("fib30 {:.3}", best);

    let s_n: usize = 500_000;
    let mut best = f64::MAX;
    for _ in 0..3 {
        let mut x: u64 = 12345;
        let mut xs: Vec<i64> = Vec::with_capacity(s_n);
        for _ in 0..s_n {
            x = (x.wrapping_mul(1103515245).wrapping_add(12345)) % 2147483648;
            xs.push(x as i64);
        }
        let t = Instant::now();
        xs.sort_unstable();
        best = best.min(ms(t.elapsed()));
        black_box(xs[0]);
    }
    println!("sort500k {:.3}", best);

    let k_n: usize = 500_000;
    let mut best = f64::MAX;
    for _ in 0..3 {
        let mut m: std::collections::HashMap<i64, i64> =
            std::collections::HashMap::with_capacity(k_n);
        let mut x: u64 = 999;
        let t = Instant::now();
        for i in 0..k_n {
            x = (x.wrapping_mul(1103515245).wrapping_add(12345)) % 2147483648;
            m.insert((x % 1000000) as i64, i as i64);
        }
        let mut acc: i64 = 0;
        for _ in 0..k_n {
            x = (x.wrapping_mul(1103515245).wrapping_add(12345)) % 2147483648;
            acc += *m.get(&((x % 1000000) as i64)).unwrap_or(&0);
        }
        best = best.min(ms(t.elapsed()));
        black_box(acc);
    }
    println!("dict500k {:.3}", best);

    let m_n: usize = 200_000;
    let mut best = f64::MAX;
    for _ in 0..3 {
        let t = Instant::now();
        let parts: Vec<String> = (0..m_n).map(|i| format!("item{}", i)).collect();
        let out = parts.join(",");
        best = best.min(ms(t.elapsed()));
        black_box(out.len());
    }
    println!("str200k {:.3}", best);
}
