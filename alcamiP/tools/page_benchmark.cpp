#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <alcami/cache.h>
#include <alcami/predefined.h>

constexpr std::size_t PAGE_SIZE = 16 * 1024;

// Each cache entry is one 16 KiB page.
//
// Alcami's lookup handle owns/protects access to the cached value,
// so there is no need for shared_ptr/Arc around the page.
using value_t = std::array<std::uint8_t, PAGE_SIZE>;

namespace {

std::uint64_t g_mapping_sleep_us = 0;
std::uint64_t g_mapping_spin_us = 0;
std::atomic<bool> g_timed_region{false};

auto spin_for(std::chrono::microseconds duration) -> void {
  const auto deadline = std::chrono::steady_clock::now() + duration;

  while (std::chrono::steady_clock::now() < deadline) {
  }
}

auto maybe_delay_mapping() -> void {
  if (g_timed_region.load(std::memory_order_relaxed)) {
    if (g_mapping_sleep_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(g_mapping_sleep_us));
    } else if (g_mapping_spin_us > 0) {
      spin_for(std::chrono::microseconds(g_mapping_spin_us));
    }
  }
}

} // namespace

// Finite-range Zipfian integer distribution over [a, b].
template <typename IntType = int>
class zipfian_int_distribution {
  static_assert(std::is_integral_v<IntType>, "Template argument must be an integral type.");

public:
  using result_type = IntType;

  struct param_type {
    using distribution_type = zipfian_int_distribution<IntType>;

    explicit param_type(IntType a = 0, IntType b = std::numeric_limits<IntType>::max(), double theta = 0.99)
        : a_(a), b_(b), theta_(theta), zeta_(zeta(static_cast<unsigned long>(b_ - a_ + 1), theta_)),
          zeta_2_theta_(zeta(2UL, theta_)) {
      assert(a_ <= b_);
      assert(theta_ > 0.0 && theta_ < 1.0);
    }

    explicit param_type(IntType a, IntType b, double theta, double zeta_value)
        : a_(a), b_(b), theta_(theta), zeta_(zeta_value), zeta_2_theta_(zeta(2UL, theta_)) {
      assert(a_ <= b_);
      assert(theta_ > 0.0 && theta_ < 1.0);
    }

    result_type a() const { return a_; }

    result_type b() const { return b_; }

    double theta() const { return theta_; }

    double zeta() const { return zeta_; }

    double zeta2theta() const { return zeta_2_theta_; }

    friend bool operator==(const param_type& lhs, const param_type& rhs) {
      return lhs.a_ == rhs.a_ && lhs.b_ == rhs.b_ && lhs.theta_ == rhs.theta_ && lhs.zeta_ == rhs.zeta_ &&
             lhs.zeta_2_theta_ == rhs.zeta_2_theta_;
    }

  private:
    static double zeta(unsigned long n, double theta) {
      double ans = 0.0;

      for (unsigned long i = 1; i <= n; ++i) {
        ans += std::pow(1.0 / static_cast<double>(i), theta);
      }

      return ans;
    }

    IntType a_;
    IntType b_;
    double theta_;
    double zeta_;
    double zeta_2_theta_;
  };

  explicit zipfian_int_distribution(IntType a = IntType(0), IntType b = IntType(1), double theta = 0.99)
      : param_(a, b, theta) {}

  explicit zipfian_int_distribution(const param_type& p) : param_(p) {}

  void reset() {}

  result_type a() const { return param_.a(); }

  result_type b() const { return param_.b(); }

  double theta() const { return param_.theta(); }

  param_type param() const { return param_; }

  void param(const param_type& p) { param_ = p; }

  result_type min() const { return a(); }

  result_type max() const { return b(); }

  template <typename UniformRandomNumberGenerator>
  result_type operator()(UniformRandomNumberGenerator& urng) {
    return (*this)(urng, param_);
  }

  template <typename UniformRandomNumberGenerator>
  result_type operator()(UniformRandomNumberGenerator& urng, const param_type& p) {
    const double alpha = 1.0 / (1.0 - p.theta());

    const double n = static_cast<double>(p.b() - p.a() + 1);

    const double eta = (1.0 - std::pow(2.0 / n, 1.0 - p.theta())) / (1.0 - p.zeta2theta() / p.zeta());

    const double u = std::generate_canonical<double, std::numeric_limits<double>::digits>(urng);

    const double uz = u * p.zeta();

    if (uz < 1.0) {
      return p.a();
    }

    if (uz < 1.0 + std::pow(0.5, p.theta())) {
      return static_cast<result_type>(p.a() + 1);
    }

    const auto offset = static_cast<uint64_t>(n * std::pow(eta * u - eta + 1.0, alpha));

    auto out = static_cast<result_type>(p.a() + static_cast<result_type>(offset));

    if (out > p.b()) {
      out = p.b();
    }

    return out;
  }

  friend bool operator==(const zipfian_int_distribution& lhs, const zipfian_int_distribution& rhs) {
    return lhs.param_ == rhs.param_;
  }

private:
  param_type param_;
};

struct distribution_generator {
  enum class mode_t { exponential, uniform, zipfian };

  distribution_generator(uint64_t seed, uint64_t bound_exclusive, double lambda, double theta, mode_t mode)
      : bound_exclusive_(bound_exclusive), gen_(seed), uniform_dist_(0, bound_exclusive - 1), exp_dist_(lambda),
        zipf_dist_(0, static_cast<key_t>(bound_exclusive - 1), theta), mode_(mode) {
    assert(bound_exclusive_ > 0);
  }

  auto next() -> key_t {
    switch (mode_) {
    case mode_t::uniform:
      return static_cast<key_t>(uniform_dist_(gen_));

    case mode_t::exponential: {
      auto k = static_cast<uint64_t>(exp_dist_(gen_));

      if (k >= bound_exclusive_) {
        k = bound_exclusive_ - 1;
      }

      return static_cast<key_t>(k);
    }

    case mode_t::zipfian:
      return zipf_dist_(gen_);
    }

    return 0;
  }

  uint64_t bound_exclusive_;
  std::mt19937_64 gen_;
  std::uniform_int_distribution<uint64_t> uniform_dist_;
  std::exponential_distribution<double> exp_dist_;
  zipfian_int_distribution<key_t> zipf_dist_;
  mode_t mode_;
};

auto main(int argc, char** argv) -> int {
  cxxopts::Options options("cache_bench");

  options.add_options()("t,threads", "Number of threads", cxxopts::value<int>()->default_value("8"))(
      "o,ops", "Ops per thread", cxxopts::value<std::size_t>()->default_value("10000"))(
      "s,cache_size", "Cache capacity in 16 KiB pages", cxxopts::value<std::size_t>()->default_value("1000"))(
      "k,total_keys", "Total keyspace size; each key represents one 16 KiB page",
      cxxopts::value<std::size_t>()->default_value("2000"))("seed", "Base seed",
                                                            cxxopts::value<std::size_t>()->default_value("12345"))(
      "l,lambda", "Rate parameter (lambda) for exponential key distribution",
      cxxopts::value<double>()->default_value("0.01"))(
      "theta", "Skew parameter for zipfian key distribution; valid range is 0 < theta < 1",
      cxxopts::value<double>()->default_value("0.99"))("d,distribution",
                                                       "Key distribution: e=exponential, u=uniform, z=zipfian",
                                                       cxxopts::value<std::string>()->default_value("e"))(
      "w,warmup", "Warmup inserts count (default = cache_size)", cxxopts::value<std::size_t>()->default_value("0"))(
      "m,mapping_sleep_us", "Sleep duration inside mapping on cache miss, in microseconds",
      cxxopts::value<uint64_t>()->default_value("0"))(
      "mapping_spin_us", "Busy-spin duration inside mapping on cache miss, in microseconds",
      cxxopts::value<uint64_t>()->default_value("0"))(
      "labeled", "Print labeled output instead of CSV",
      cxxopts::value<bool>()->default_value("true")->implicit_value("false"));

  auto result = options.parse(argc, argv);

  const int threads_count_arg = result["threads"].as<int>();

  if (threads_count_arg <= 0) {
    spdlog::error("threads must be > 0");
    return 1;
  }

  const auto threads_count = static_cast<std::size_t>(threads_count_arg);

  const std::size_t ops_per_thread = result["ops"].as<std::size_t>();

  const std::size_t cache_size = result["cache_size"].as<std::size_t>();

  const std::size_t total_keys = result["total_keys"].as<std::size_t>();

  const std::size_t seed = result["seed"].as<std::size_t>();

  const double lambda = result["lambda"].as<double>();

  const double theta = result["theta"].as<double>();

  const std::string distribution = result["distribution"].as<std::string>();

  const auto mapping_sleep_us = result["mapping_sleep_us"].as<uint64_t>();

  const auto mapping_spin_us = result["mapping_spin_us"].as<uint64_t>();

  if (mapping_sleep_us > 0 && mapping_spin_us > 0) {
    spdlog::error("set at most one of mapping_sleep_us or mapping_spin_us");
    return 1;
  }

  g_mapping_sleep_us = mapping_sleep_us;

  g_mapping_spin_us = mapping_spin_us;

  g_timed_region.store(false, std::memory_order_relaxed);

  distribution_generator::mode_t distribution_mode;

  if (distribution == "e") {
    distribution_mode = distribution_generator::mode_t::exponential;
  } else if (distribution == "u") {
    distribution_mode = distribution_generator::mode_t::uniform;
  } else if (distribution == "z") {
    distribution_mode = distribution_generator::mode_t::zipfian;
  } else {
    spdlog::error("distribution must be one of: e, u, or z");
    return 1;
  }

  std::size_t warmup = result["warmup"].as<std::size_t>();

  if (warmup == 0) {
    warmup = cache_size;
  }

  if (ops_per_thread == 0) {
    spdlog::error("ops per thread must be > 0");
    return 1;
  }

  if (cache_size == 0) {
    spdlog::error("cache_size must be > 0");
    return 1;
  }

  if (total_keys == 0) {
    spdlog::error("total_keys must be > 0");
    return 1;
  }

  if (distribution_mode == distribution_generator::mode_t::exponential && !(lambda > 0.0 && std::isfinite(lambda))) {
    spdlog::error("lambda must be a finite positive number");
    return 1;
  }

  if (distribution_mode == distribution_generator::mode_t::zipfian &&
      !(theta > 0.0 && theta < 1.0 && std::isfinite(theta))) {
    spdlog::error("theta must be a finite number with 0 < theta < 1");
    return 1;
  }

  if (warmup == 0) {
    spdlog::error("warmup must be > 0 (or omit --warmup to default to cache_size)");
    return 1;
  }

  /*
   * Cache miss mapping.
   *
   * `out` is the page storage owned by the cache entry.
   *
   * We fill all 16 KiB so the memory is actually materialized/touched.
   * A key-derived value is used instead of zero so each page has
   * deterministic content associated with its key.
   *
   * No shared_ptr is involved. Alcami's lookup handle is responsible
   * for keeping the cache entry valid while it is being accessed.
   */
  auto mapping = [](key_t in, value_t& out) -> std::optional<std::error_code> {
    maybe_delay_mapping();

    const auto fill_byte = static_cast<std::uint8_t>(static_cast<std::uint64_t>(in) + 1);

    out.fill(fill_byte);

    return std::nullopt;
  };

  /*
   * cache_size is now a count of 16 KiB pages.
   *
   * Examples:
   *
   *   cache_size = 1,000
   *       => ~15.625 MiB page payload
   *
   *   cache_size = 10,000
   *       => ~156.25 MiB page payload
   */
  auto cache = alc::make_cache<key_t, value_t>(cache_size, mapping, alc::policies::lru);

  const std::size_t warmup_keys = std::min(warmup, total_keys);

  std::atomic<std::size_t> warmup_misses{0};

  // Warmup should not include artificial
  // miss sleep/spin.
  g_timed_region.store(false, std::memory_order_relaxed);

  for (key_t k = 0; k < static_cast<key_t>(warmup_keys); k++) {
    /*
     * The returned Alcami handle protects the
     * cached 16 KiB page for the lifetime of `h`.
     */
    auto h = cache.lookup(k);
    (void)h;
  }

  /*
   * Generate all requested keys before the timed
   * region so RNG cost is not included in the
   * cache benchmark.
   */
  std::vector<std::vector<key_t>> keys(threads_count);

  std::mt19937_64 thread_seed_rng(seed);

  std::uniform_int_distribution<uint64_t> thread_seed_dist(0, std::numeric_limits<uint64_t>::max());

  for (std::size_t t = 0; t < threads_count; t++) {
    keys[t].reserve(ops_per_thread);

    uint64_t thread_seed = thread_seed_dist(thread_seed_rng);

    distribution_generator gen(thread_seed, static_cast<uint64_t>(total_keys), lambda, theta, distribution_mode);

    for (std::size_t i = 0; i < ops_per_thread; i++) {
      keys[t].push_back(gen.next());
    }
  }

  std::atomic<bool> start{false};

  std::vector<std::thread> threads;

  threads.reserve(threads_count);

  for (std::size_t i = 0; i < threads_count; i++) {
    threads.emplace_back([&, i]() -> void {
      while (!start.load(std::memory_order_acquire)) {
      }

      for (auto k : keys[i]) {
        /*
         * On a hit this obtains an Alcami
         * handle referring to the cached
         * 16 KiB page.
         *
         * We deliberately don't scan the
         * page contents here. That keeps
         * this primarily a cache/handle
         * benchmark rather than a memory
         * bandwidth benchmark.
         */
        auto h = cache.lookup(k);

        (void)h;
      }
    });
  }

  auto t0 = std::chrono::high_resolution_clock::now();

  g_timed_region.store(true, std::memory_order_release);

  start.store(true, std::memory_order_release);

  for (auto& t : threads) {
    t.join();
  }

  auto t1 = std::chrono::high_resolution_clock::now();

  g_timed_region.store(false, std::memory_order_relaxed);

  const std::size_t total_ops = threads_count * ops_per_thread;

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

  const double secs = static_cast<double>(ns) / 1e9;

  const double m_ops = (static_cast<double>(total_ops) / secs) * 1e-6;

  const double ns_per_op = static_cast<double>(ns) / static_cast<double>(total_ops);

  const bool labeled = result["labeled"].as<bool>();

  if (labeled) {
    std::cout << "ops_per_thread=" << ops_per_thread

              << ",\nthreads=" << threads_count

              << ",\ncache_size=" << cache_size

              << ",\ntotal_keys=" << total_keys

              << ",\ndistribution=" << distribution

              << ",\nlambda=" << lambda

              << ",\ntheta=" << theta

              << ",\nwarmup=" << warmup_keys

              << ",\nmapping_sleep_us=" << mapping_sleep_us

              << ",\nmapping_spin_us=" << mapping_spin_us

              << ",\nsecs=" << secs

              << ",\nmops=" << m_ops

              << ",\nns_per_op=" << ns_per_op

              << ",\nwarmup_misses=" << warmup_misses.load(std::memory_order_relaxed)

              << ",\nhit_percent=" << cache.hit_percent()

              << '\n';
  } else {
    std::cout << ops_per_thread << ',' << threads_count << ',' << cache_size << ',' << total_keys << ',' << distribution
              << ',' << lambda << ',' << theta << ',' << warmup_keys << ',' << mapping_sleep_us << ','
              << mapping_spin_us << ',' << secs << ',' << m_ops << ',' << ns_per_op << ','
              << warmup_misses.load(std::memory_order_relaxed) << ',' << cache.hit_percent() << '\n';
  }

  return 0;
}
