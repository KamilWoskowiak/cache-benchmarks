use std::env;
use std::process;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::thread;
use std::time::Instant;

use quick_cache::sync::Cache;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};

type Key = u64;
type Value = Key;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Exponential,
    Uniform,
}

struct DistributionGenerator {
    rng: StdRng,
    lambda: f64,
    mode: Mode,
}

impl DistributionGenerator {
    fn new(seed: u64, lambda: f64, mode: Mode) -> Self {
        Self {
            rng: StdRng::seed_from_u64(seed),
            lambda,
            mode,
        }
    }

    fn next_bounded(&mut self, bound_exclusive: u64) -> Key {
        match self.mode {
            Mode::Uniform => self.rng.random_range(0..bound_exclusive),
            Mode::Exponential => {
                // Inverse CDF sampling for Exp(lambda):
                // X = -ln(U) / lambda, where U ~ Uniform(0, 1]
                let u = loop {
                    let x: f64 = self.rng.random();
                    if x > 0.0 {
                        break x;
                    }
                };
                let raw = (-u.ln() / self.lambda) as u64;
                if raw >= bound_exclusive {
                    bound_exclusive - 1
                } else {
                    raw
                }
            }
        }
    }
}

#[derive(Debug, Clone)]
struct Config {
    threads: usize,
    ops: usize,
    cache_size: usize,
    total_keys: usize,
    seed: u64,
    lambda: f64,
    distribution: String,
    warmup: usize,
    labeled: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            threads: 8,
            ops: 10_000,
            cache_size: 1_000,
            total_keys: 2_000,
            seed: 12_345,
            lambda: 0.01,
            distribution: "e".to_string(),
            warmup: 0,
            labeled: true,
        }
    }
}

fn print_usage_and_exit() -> ! {
    eprintln!(
        "Usage: cache_bench [options]

Options:
  -t, --threads <N>         Number of threads (default: 8)
  -o, --ops <N>             Ops per thread (default: 10000)
  -s, --cache_size <N>      Cache capacity (default: 1000)
  -k, --total_keys <N>      Total keyspace size (default: 2000)
      --seed <N>            Base seed (default: 12345)
  -l, --lambda <F>          Exponential rate lambda (default: 0.01)
  -d, --distribution <e|u>  e = exponential, u = uniform (default: e)
  -w, --warmup <N>          Warmup inserts count, 0 means cache_size
      --labeled <bool>      true or false (default: true)
      --help                Show this help
"
    );
    process::exit(1);
}

fn parse_usize(flag: &str, value: Option<String>) -> usize {
    match value.and_then(|s| s.parse::<usize>().ok()) {
        Some(v) => v,
        None => {
            eprintln!("invalid value for {}", flag);
            process::exit(1);
        }
    }
}

fn parse_u64(flag: &str, value: Option<String>) -> u64 {
    match value.and_then(|s| s.parse::<u64>().ok()) {
        Some(v) => v,
        None => {
            eprintln!("invalid value for {}", flag);
            process::exit(1);
        }
    }
}

fn parse_f64(flag: &str, value: Option<String>) -> f64 {
    match value.and_then(|s| s.parse::<f64>().ok()) {
        Some(v) => v,
        None => {
            eprintln!("invalid value for {}", flag);
            process::exit(1);
        }
    }
}

fn parse_bool(flag: &str, value: Option<String>) -> bool {
    match value.as_deref() {
        Some("true") => true,
        Some("false") => false,
        _ => {
            eprintln!("invalid value for {} (expected true or false)", flag);
            process::exit(1);
        }
    }
}

fn parse_args() -> Config {
    let mut cfg = Config::default();
    let mut args = env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--help" | "-h" => print_usage_and_exit(),

            "--threads" | "-t" => {
                cfg.threads = parse_usize("--threads", args.next());
            }
            "--ops" | "-o" => {
                cfg.ops = parse_usize("--ops", args.next());
            }
            "--cache_size" | "-s" => {
                cfg.cache_size = parse_usize("--cache_size", args.next());
            }
            "--total_keys" | "-k" => {
                cfg.total_keys = parse_usize("--total_keys", args.next());
            }
            "--seed" => {
                cfg.seed = parse_u64("--seed", args.next());
            }
            "--lambda" | "-l" => {
                cfg.lambda = parse_f64("--lambda", args.next());
            }
            "--distribution" | "-d" => {
                cfg.distribution = match args.next() {
                    Some(v) => v,
                    None => {
                        eprintln!("missing value for --distribution");
                        process::exit(1);
                    }
                };
            }
            "--warmup" | "-w" => {
                cfg.warmup = parse_usize("--warmup", args.next());
            }
            "--labeled" => {
                cfg.labeled = parse_bool("--labeled", args.next());
            }

            _ => {
                eprintln!("unknown argument: {}", arg);
                print_usage_and_exit();
            }
        }
    }

    cfg
}

fn main() {
    let mut cfg = parse_args();

    if cfg.threads == 0 {
        eprintln!("threads must be > 0");
        process::exit(1);
    }
    if cfg.ops == 0 {
        eprintln!("ops per thread must be > 0");
        process::exit(1);
    }
    if cfg.cache_size == 0 {
        eprintln!("cache_size must be > 0");
        process::exit(1);
    }
    if cfg.total_keys == 0 {
        eprintln!("total_keys must be > 0");
        process::exit(1);
    }
    if cfg.warmup == 0 {
        cfg.warmup = cfg.cache_size;
    }
    if cfg.warmup == 0 {
        eprintln!("warmup must be > 0 (or omit --warmup to default to cache_size)");
        process::exit(1);
    }

    let distribution_mode = match cfg.distribution.as_str() {
        "e" => Mode::Exponential,
        "u" => Mode::Uniform,
        _ => {
            eprintln!("distribution must be one of: e or u");
            process::exit(1);
        }
    };

    if distribution_mode == Mode::Exponential && !(cfg.lambda > 0.0 && cfg.lambda.is_finite()) {
        eprintln!("lambda must be a finite positive number");
        process::exit(1);
    }

    let cache: Cache<Key, Value> = Cache::new(cfg.cache_size);

    let warmup_keys = cfg.warmup.min(cfg.total_keys);
    let warmup_misses = AtomicUsize::new(0);

    for k in 0..(warmup_keys as Key) {
        if cache.get(&k).is_none() {
            cache.insert(k, k);
        }
    }

    let mut keys: Vec<Vec<Key>> = vec![Vec::with_capacity(cfg.ops); cfg.threads];

    let mut thread_seed_rng = StdRng::seed_from_u64(cfg.seed);
    for thread_keys in &mut keys {
        let thread_seed: u64 = thread_seed_rng.random();
        let mut dist_gen = DistributionGenerator::new(thread_seed, cfg.lambda, distribution_mode);
        for _ in 0..cfg.ops {
            thread_keys.push(dist_gen.next_bounded(cfg.total_keys as u64));
        }
    }

    let start = Arc::new(AtomicBool::new(false));
    let cache = Arc::new(cache);

    let mut threads = Vec::with_capacity(cfg.threads);

    for thread_keys in keys {
        let start = Arc::clone(&start);
        let cache = Arc::clone(&cache);

        threads.push(thread::spawn(move || {
            while !start.load(Ordering::Acquire) {
                std::hint::spin_loop();
            }

            for k in thread_keys {
                if cache.get(&k).is_none() {
                    cache.insert(k, k);
                }
            }
        }));
    }

    let t0 = Instant::now();
    start.store(true, Ordering::Release);

    for t in threads {
        t.join().expect("worker thread panicked");
    }

    let secs = t0.elapsed().as_secs_f64();
    let total_ops = cfg.threads * cfg.ops;
    let m_ops = (total_ops as f64 / secs) * 1e-6;
    let ns_per_op = (secs * 1e9) / total_ops as f64;

    if cfg.labeled {
        println!(
            "ops_per_thread={},\nthreads={},\nncache_size={},\ntotal_keys={},\ndistribution={},\nlambda={},\nwarmup={},\nsecs={},\nmops={},\nns_per_op={},\nwarmup_misses={}",
            cfg.ops,
            cfg.threads,
            cfg.cache_size,
            cfg.total_keys,
            cfg.distribution,
            cfg.lambda,
            warmup_keys,
            secs,
            m_ops,
            ns_per_op,
            warmup_misses.load(Ordering::Relaxed)
        );
    } else {
        println!(
            "{},{},{},{},{},{},{},{},{},{},{}",
            cfg.ops,
            cfg.threads,
            cfg.cache_size,
            cfg.total_keys,
            cfg.distribution,
            cfg.lambda,
            warmup_keys,
            secs,
            m_ops,
            ns_per_op,
            warmup_misses.load(Ordering::Relaxed)
        );
    }
}
