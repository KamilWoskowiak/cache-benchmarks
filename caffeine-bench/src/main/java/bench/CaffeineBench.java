package bench;

import com.github.benmanes.caffeine.cache.Cache;
import com.github.benmanes.caffeine.cache.Caffeine;

import java.util.ArrayList;
import java.util.List;
import java.util.SplittableRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class CaffeineBench {
  private enum Mode {
    EXPONENTIAL,
    UNIFORM,
    ZIPFIAN
  }

  private static final class DistributionGenerator {
    private final SplittableRandom rng;
    private final long boundExclusive;
    private final double lambda;
    private final double theta;
    private final Mode mode;

    private final double zipfAlpha;
    private final double zipfEta;
    private final double zipfZeta;

    DistributionGenerator(long seed, long boundExclusive, double lambda, double theta, Mode mode) {
      this.rng = new SplittableRandom(seed);
      this.boundExclusive = boundExclusive;
      this.lambda = lambda;
      this.theta = theta;
      this.mode = mode;

      if (mode == Mode.ZIPFIAN) {
        double zeta = zeta(boundExclusive, theta);
        double zeta2Theta = zeta(2L, theta);
        double n = (double) boundExclusive;

        this.zipfAlpha = 1.0 / (1.0 - theta);
        this.zipfEta =
            (1.0 - Math.pow(2.0 / n, 1.0 - theta)) / (1.0 - (zeta2Theta / zeta));
        this.zipfZeta = zeta;
      } else {
        this.zipfAlpha = 0.0;
        this.zipfEta = 0.0;
        this.zipfZeta = 0.0;
      }
    }

    private static double zeta(long n, double theta) {
      double sum = 0.0;

      for (long i = 1; i <= n; i++) {
        sum += Math.pow(1.0 / (double) i, theta);
      }

      return sum;
    }

    long nextBounded() {
      switch (mode) {
        case UNIFORM:
          return rng.nextLong(boundExclusive);

        case EXPONENTIAL: {
          double u;
          do {
            u = rng.nextDouble();
          } while (u <= 0.0);

          long raw = (long) (-Math.log(u) / lambda);
          return raw >= boundExclusive ? boundExclusive - 1 : raw;
        }

        case ZIPFIAN: {
          if (boundExclusive == 1) {
            return 0;
          }

          double u = rng.nextDouble();
          double uz = u * zipfZeta;

          if (uz < 1.0) {
            return 0;
          }

          if (uz < 1.0 + Math.pow(0.5, theta)) {
            return Math.min(1L, boundExclusive - 1);
          }

          long offset = (long) (boundExclusive * Math.pow(zipfEta * u - zipfEta + 1.0, zipfAlpha));
          return offset >= boundExclusive ? boundExclusive - 1 : offset;
        }

        default:
          throw new IllegalStateException("unknown distribution mode");
      }
    }
  }

  private static final class Config {
    int threads = 8;
    int ops = 10_000;
    int cacheSize = 1_000;
    int totalKeys = 2_000;
    long seed = 12_345L;
    double lambda = 0.01;
    double theta = 0.99;
    String distribution = "e";
    int warmup = 0;
    long mappingSleepUs = 0;
    long mappingSpinUs = 0;
    boolean labeled = true;
  }

  private static void usageAndExit() {
    System.err.println("""
        Usage: CaffeineBench [options]

        Options:
          -t, --threads <N>              Number of threads (default: 8)
          -o, --ops <N>                  Ops per thread (default: 10000)
          -s, --cache_size <N>           Cache capacity (default: 1000)
          -k, --total_keys <N>           Total keyspace size (default: 2000)
              --seed <N>                 Base seed (default: 12345)
          -l, --lambda <F>               Exponential rate lambda (default: 0.01)
              --theta <F>                Zipfian skew parameter theta, 0 < theta < 1 (default: 0.99)
          -d, --distribution <e|u|z>     e = exponential, u = uniform, z = zipfian (default: e)
          -w, --warmup <N>               Warmup inserts count, 0 means cache_size
          -m, --mapping_sleep_us <N>     Sleep duration inside mapping on cache miss, in microseconds
              --mapping_spin_us <N>      Busy-spin duration inside mapping on cache miss, in microseconds
              --labeled <bool>           true or false (default: true)
              --help                     Show this help
        """);
    System.exit(1);
  }

  private static String requireValue(String flag, String[] args, int index) {
    if (index >= args.length) {
      System.err.println("missing value for " + flag);
      System.exit(1);
    }
    return args[index];
  }

  private static int parseInt(String flag, String value) {
    try {
      return Integer.parseInt(value);
    } catch (NumberFormatException e) {
      System.err.println("invalid value for " + flag);
      System.exit(1);
      return 0;
    }
  }

  private static long parseLong(String flag, String value) {
    try {
      return Long.parseLong(value);
    } catch (NumberFormatException e) {
      System.err.println("invalid value for " + flag);
      System.exit(1);
      return 0L;
    }
  }

  private static double parseDouble(String flag, String value) {
    try {
      return Double.parseDouble(value);
    } catch (NumberFormatException e) {
      System.err.println("invalid value for " + flag);
      System.exit(1);
      return 0.0;
    }
  }

  private static boolean parseBoolean(String flag, String value) {
    if ("true".equals(value)) {
      return true;
    }
    if ("false".equals(value)) {
      return false;
    }

    System.err.println("invalid value for " + flag + " (expected true or false)");
    System.exit(1);
    return false;
  }

  private static Config parseArgs(String[] args) {
    Config cfg = new Config();

    for (int i = 0; i < args.length; i++) {
      String arg = args[i];

      switch (arg) {
        case "--help", "-h" -> usageAndExit();

        case "--threads", "-t" -> cfg.threads = parseInt(arg, requireValue(arg, args, ++i));
        case "--ops", "-o" -> cfg.ops = parseInt(arg, requireValue(arg, args, ++i));
        case "--cache_size", "-s" -> cfg.cacheSize = parseInt(arg, requireValue(arg, args, ++i));
        case "--total_keys", "-k" -> cfg.totalKeys = parseInt(arg, requireValue(arg, args, ++i));
        case "--seed" -> cfg.seed = parseLong(arg, requireValue(arg, args, ++i));
        case "--lambda", "-l" -> cfg.lambda = parseDouble(arg, requireValue(arg, args, ++i));
        case "--theta" -> cfg.theta = parseDouble(arg, requireValue(arg, args, ++i));

        case "--distribution", "-d" -> cfg.distribution = requireValue(arg, args, ++i);

        case "--warmup", "-w" -> cfg.warmup = parseInt(arg, requireValue(arg, args, ++i));

        case "--mapping_sleep_us", "-m" ->
            cfg.mappingSleepUs = parseLong(arg, requireValue(arg, args, ++i));

        case "--mapping_spin_us" ->
            cfg.mappingSpinUs = parseLong(arg, requireValue(arg, args, ++i));

        case "--labeled" -> cfg.labeled = parseBoolean(arg, requireValue(arg, args, ++i));

        default -> {
          System.err.println("unknown argument: " + arg);
          usageAndExit();
        }
      }
    }

    return cfg;
  }

  private static void spinForMicros(long micros) {
    long durationNanos = TimeUnit.MICROSECONDS.toNanos(micros);
    long deadline = System.nanoTime() + durationNanos;

    while (System.nanoTime() < deadline) {
      Thread.onSpinWait();
    }
  }

  private static long mapping(long key, long mappingSleepUs, long mappingSpinUs) {
    if (mappingSleepUs > 0) {
      try {
        TimeUnit.MICROSECONDS.sleep(mappingSleepUs);
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
        throw new RuntimeException("mapping sleep interrupted", e);
      }
    } else if (mappingSpinUs > 0) {
      spinForMicros(mappingSpinUs);
    }

    return key;
  }

  public static void main(String[] args) throws InterruptedException {
    Config cfg = parseArgs(args);

    if (cfg.threads <= 0) {
      System.err.println("threads must be > 0");
      System.exit(1);
    }
    if (cfg.ops <= 0) {
      System.err.println("ops per thread must be > 0");
      System.exit(1);
    }
    if (cfg.cacheSize <= 0) {
      System.err.println("cache_size must be > 0");
      System.exit(1);
    }
    if (cfg.totalKeys <= 0) {
      System.err.println("total_keys must be > 0");
      System.exit(1);
    }
    if (cfg.mappingSleepUs > 0 && cfg.mappingSpinUs > 0) {
      System.err.println("set at most one of mapping_sleep_us or mapping_spin_us");
      System.exit(1);
    }

    if (cfg.warmup == 0) {
      cfg.warmup = cfg.cacheSize;
    }
    if (cfg.warmup <= 0) {
      System.err.println("warmup must be > 0 (or omit --warmup to default to cache_size)");
      System.exit(1);
    }

    Mode distributionMode;
    switch (cfg.distribution) {
      case "e" -> distributionMode = Mode.EXPONENTIAL;
      case "u" -> distributionMode = Mode.UNIFORM;
      case "z" -> distributionMode = Mode.ZIPFIAN;
      default -> {
        System.err.println("distribution must be one of: e, u, or z");
        System.exit(1);
        return;
      }
    }

    if (distributionMode == Mode.EXPONENTIAL && !(cfg.lambda > 0.0 && Double.isFinite(cfg.lambda))) {
      System.err.println("lambda must be a finite positive number");
      System.exit(1);
    }

    if (distributionMode == Mode.ZIPFIAN
        && !(cfg.theta > 0.0 && cfg.theta < 1.0 && Double.isFinite(cfg.theta))) {
      System.err.println("theta must be a finite number with 0 < theta < 1");
      System.exit(1);
    }

    Cache<Long, Long> cache = Caffeine.newBuilder()
        .maximumSize(cfg.cacheSize)
        .build();

    int warmupKeys = Math.min(cfg.warmup, cfg.totalKeys);
    AtomicLong warmupMisses = new AtomicLong(0);

    for (long k = 0; k < warmupKeys; k++) {
      Long existing = cache.getIfPresent(k);
      if (existing == null) {
        warmupMisses.incrementAndGet();
        cache.put(k, mapping(k, 0, 0));
      }
    }

    cache.cleanUp();

    List<long[]> keys = new ArrayList<>(cfg.threads);

    SplittableRandom threadSeedRng = new SplittableRandom(cfg.seed);
    for (int t = 0; t < cfg.threads; t++) {
      long[] threadKeys = new long[cfg.ops];
      long threadSeed = threadSeedRng.nextLong();

      DistributionGenerator distGen =
          new DistributionGenerator(
              threadSeed,
              cfg.totalKeys,
              cfg.lambda,
              cfg.theta,
              distributionMode
          );

      for (int i = 0; i < cfg.ops; i++) {
        threadKeys[i] = distGen.nextBounded();
      }

      keys.add(threadKeys);
    }

    AtomicBoolean start = new AtomicBoolean(false);
    List<Thread> threads = new ArrayList<>(cfg.threads);

    for (long[] threadKeys : keys) {
      long mappingSleepUs = cfg.mappingSleepUs;
      long mappingSpinUs = cfg.mappingSpinUs;

      Thread worker = new Thread(() -> {
        while (!start.getAcquire()) {
          Thread.onSpinWait();
        }

        for (long k : threadKeys) {
          cache.get(k, key -> mapping(key, mappingSleepUs, mappingSpinUs));
        }
      });

      threads.add(worker);
      worker.start();
    }

    long t0 = System.nanoTime();
    start.setRelease(true);

    for (Thread thread : threads) {
      thread.join();
    }

    long elapsedNanos = System.nanoTime() - t0;
    double secs = elapsedNanos / 1e9;

    long totalOps = (long) cfg.threads * cfg.ops;
    double mOps = (totalOps / secs) * 1e-6;
    double nsPerOp = (secs * 1e9) / totalOps;

    if (cfg.labeled) {
      System.out.printf(
          "ops_per_thread=%d,%nthreads=%d,%ncache_size=%d,%ntotal_keys=%d,%ndistribution=%s,%nlambda=%s,%ntheta=%s,%nwarmup=%d,%nmapping_sleep_us=%d,%nmapping_spin_us=%d,%nsecs=%s,%nmops=%s,%nns_per_op=%s,%nwarmup_misses=%d%n",
          cfg.ops,
          cfg.threads,
          cfg.cacheSize,
          cfg.totalKeys,
          cfg.distribution,
          cfg.lambda,
          cfg.theta,
          warmupKeys,
          cfg.mappingSleepUs,
          cfg.mappingSpinUs,
          secs,
          mOps,
          nsPerOp,
          warmupMisses.get()
      );
    } else {
      System.out.printf(
          "%d,%d,%d,%d,%s,%s,%s,%d,%d,%d,%s,%s,%s,%d%n",
          cfg.ops,
          cfg.threads,
          cfg.cacheSize,
          cfg.totalKeys,
          cfg.distribution,
          cfg.lambda,
          cfg.theta,
          warmupKeys,
          cfg.mappingSleepUs,
          cfg.mappingSpinUs,
          secs,
          mOps,
          nsPerOp,
          warmupMisses.get()
      );
    }
  }
}
