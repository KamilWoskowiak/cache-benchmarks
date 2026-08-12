#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <alcami/cache.h>
#include <alcami/predefined.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using size_t = alc::cache_size_t;
using key_t = size_t;
using value_t = key_t;

struct loadCounter {
  loadCounter() : counts{std::make_shared<std::unordered_map<key_t, int>>()}, mtx{std::make_shared<std::mutex>()} {}

  loadCounter(std::shared_ptr<std::unordered_map<key_t, int>> counter)
      : counts{std::move(counter)}, mtx{std::make_shared<std::mutex>()} {}

  loadCounter(std::shared_ptr<std::unordered_map<key_t, int>> counter, std::shared_ptr<std::mutex> m)
      : counts{std::move(counter)}, mtx{std::move(m)} {}

  // this needs to be const for loadCounter to be regular_invokable
  auto operator()(key_t key) const -> value_t {
    std::lock_guard<std::mutex> lock(*mtx);
    ++(*counts)[key];
    return key;
  }

  // make this mutable so loadCounter is regular_invokable, assume concurrency is handled internally
  mutable std::shared_ptr<std::unordered_map<key_t, int>> counts;
  mutable std::shared_ptr<std::mutex> mtx;
};

struct slowLoadCounter {
  slowLoadCounter(std::shared_ptr<std::unordered_map<key_t, int>> counter, std::shared_ptr<std::mutex> m,
                  std::chrono::milliseconds delay = std::chrono::milliseconds{50})
      : counts{std::move(counter)}, mtx{std::move(m)}, sleep_for{delay} {}

  // const so this remains regular_invokable-friendly
  auto operator()(key_t key) const -> value_t {
    std::this_thread::sleep_for(sleep_for);

    std::lock_guard<std::mutex> lock(*mtx);
    ++(*counts)[key];
    return key;
  }

  mutable std::shared_ptr<std::unordered_map<key_t, int>> counts;
  mutable std::shared_ptr<std::mutex> mtx;
  std::chrono::milliseconds sleep_for;
};

[[nodiscard]]
auto make_count_mapping(std::shared_ptr<std::unordered_map<key_t, int>> counter) {
  return alc::mapping_adapter<key_t>(loadCounter{std::move(counter)});
}

[[nodiscard]]
auto make_count_mapping(std::shared_ptr<std::unordered_map<key_t, int>> counter, std::shared_ptr<std::mutex> m) {
  return alc::mapping_adapter<key_t>(loadCounter{std::move(counter), std::move(m)});
}

[[nodiscard]]
auto make_slow_count_mapping(std::shared_ptr<std::unordered_map<key_t, int>> counter, std::shared_ptr<std::mutex> m,
                             std::chrono::milliseconds delay = std::chrono::milliseconds{50}) {
  return alc::mapping_adapter<key_t>(slowLoadCounter{std::move(counter), std::move(m), delay});
}

[[nodiscard]]
auto slow_mapping_test_delays() -> std::vector<std::chrono::milliseconds> {
  return {
      std::chrono::milliseconds{0},    std::chrono::milliseconds{1},   std::chrono::milliseconds{5},
      std::chrono::milliseconds{10},   std::chrono::milliseconds{25},  std::chrono::milliseconds{50},
      std::chrono::milliseconds{100},  std::chrono::milliseconds{250}, std::chrono::milliseconds{500},
      std::chrono::milliseconds{1000},
  };
}

TEST(ConcurrentCacheManager, ConcurentHitsSameKeyValidHandle) {
  constexpr auto cache_size{4};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu);

  auto warm = cache.lookup(42);
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(**warm, 42);

  std::atomic<int> ok{0};
  std::barrier start(16);

  std::vector<std::thread> threads;
  threads.reserve(16);

  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&] {
      start.arrive_and_wait();

      auto h = cache.lookup(42);
      if (h && **h == 42) {
        ok.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(ok.load(), 16);
  EXPECT_EQ((*loads)[42], 1);
}

TEST(ConcurrentCacheManager, ConcurrentDoubleMissSingleLoad) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu);

  std::atomic<int> ok{0};
  std::barrier start(2);

  auto f = [&](int) {
    start.arrive_and_wait();

    auto h = cache.lookup(7);
    if (h && **h == 7) {
      ok.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread a(f, 0);
  std::thread b(f, 1);

  a.join();
  b.join();

  EXPECT_EQ(ok.load(), 2);
  EXPECT_EQ((*loads)[7], 1);
}

TEST(ConcurrentCacheManager, ConcurrentMissDifferentKeysRespectCapacity) {
  constexpr auto cache_size{1};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu);

  std::optional<alc::cache_handle<value_t>> h0;
  std::optional<alc::cache_handle<value_t>> h1;

  std::barrier start(2);

  std::thread t0([&] {
    start.arrive_and_wait();
    h0 = cache.lookup(0);
  });

  std::thread t1([&] {
    start.arrive_and_wait();
    h1 = cache.lookup(1);
  });

  t0.join();
  t1.join();

  const bool s0 = h0.has_value();
  const bool s1 = h1.has_value();

  EXPECT_TRUE(s0 ^ s1);

  if (s0) {
    EXPECT_EQ(**h0, 0);
  }

  if (s1) {
    EXPECT_EQ(**h1, 1);
  }

  EXPECT_LE((*loads)[0], 1);
  EXPECT_LE((*loads)[1], 1);
}

TEST(ConcurrentCacheManager, ConcurrentEvictionReferenceSafety) {
  constexpr auto cache_size{1};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu);

  auto hold = cache.lookup(100);
  ASSERT_TRUE(hold.has_value());
  EXPECT_EQ(**hold, 100);

  std::optional<alc::cache_handle<value_t>> miss;

  std::thread t([&] { miss = cache.lookup(101); });

  t.join();

  EXPECT_EQ(miss, std::nullopt);

  hold.reset();

  auto h2 = cache.lookup(101);
  ASSERT_TRUE(h2.has_value());
  EXPECT_EQ(**h2, 101);
}

TEST(ConcurrentCacheManager, ConcurrentHeapUpdates) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu);

  std::atomic<int> ok{0};
  std::barrier start(32);

  std::vector<std::thread> threads;
  threads.reserve(32);

  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();

      for (int r = 0; r < 200; ++r) {
        auto h = cache.lookup((i % 2) ? 5 : 6);
        if (h) {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_GT(ok.load(), 0);
  EXPECT_EQ((*loads)[5] + (*loads)[6], 2);
}

struct loadCounterWithMutex {
  loadCounterWithMutex(std::shared_ptr<std::unordered_map<key_t, int>> c, std::shared_ptr<std::mutex> m)
      : counts{std::move(c)}, mtx{std::move(m)} {}

  auto operator()(key_t key) -> value_t {
    std::lock_guard<std::mutex> lock(*mtx);
    ++(*counts)[key];
    return key;
  }

  std::shared_ptr<std::unordered_map<key_t, int>> counts;
  std::shared_ptr<std::mutex> mtx;
};

TEST(ConcurrentCacheManager, SimultaneousEvictionAndAccessConsistency) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();
  auto mtx = std::make_shared<std::mutex>();

  auto cache = alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads, mtx), alc::policies::lfu);

  EXPECT_EQ(**cache.lookup(0), 0);
  EXPECT_EQ(**cache.lookup(0), 0);
  EXPECT_EQ(**cache.lookup(0), 0);
  EXPECT_EQ(**cache.lookup(1), 1);

  std::barrier start(2);

  std::thread t_eviction([&] {
    start.arrive_and_wait();
    EXPECT_EQ(**cache.lookup(2), 2);
  });

  std::thread t_access([&] {
    start.arrive_and_wait();
    EXPECT_EQ(**cache.lookup(1), 1);
  });

  t_eviction.join();
  t_access.join();

  EXPECT_EQ(**cache.lookup(1), 1);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[2], 1);
  EXPECT_GE((*loads)[1], 1);
  EXPECT_LE((*loads)[1], 2);
}

TEST(ConcurrentCacheManager, SlowMappingConcurrentSameKeySingleLoadAcrossDelays) {
  constexpr auto cache_size{2};

  for (const auto delay : slow_mapping_test_delays()) {
    SCOPED_TRACE(testing::Message() << "delay_ms=" << delay.count());

    auto loads = std::make_shared<std::unordered_map<key_t, int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto cache =
        alc::make_cache<key_t, value_t>(cache_size, make_slow_count_mapping(loads, mtx, delay), alc::policies::lfu);

    std::atomic<int> ok{0};
    std::barrier start(8);

    std::vector<std::thread> threads;
    threads.reserve(8);

    for (int i = 0; i < 8; ++i) {
      threads.emplace_back([&] {
        start.arrive_and_wait();

        auto h = cache.lookup(123);
        if (h && **h == 123) {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    EXPECT_EQ(ok.load(), 8);
    EXPECT_EQ((*loads)[123], 1);
  }
}

TEST(ConcurrentCacheManager, SlowMappingMissDoesNotDuplicateWhileLoadInProgressAcrossDelays) {
  constexpr auto cache_size{2};

  for (const auto delay : slow_mapping_test_delays()) {
    SCOPED_TRACE(testing::Message() << "delay_ms=" << delay.count());

    auto loads = std::make_shared<std::unordered_map<key_t, int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto cache =
        alc::make_cache<key_t, value_t>(cache_size, make_slow_count_mapping(loads, mtx, delay), alc::policies::lfu);

    std::atomic<int> ok{0};
    std::barrier start(4);

    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int i = 0; i < 4; ++i) {
      threads.emplace_back([&] {
        start.arrive_and_wait();

        auto h = cache.lookup(77);
        if (h && **h == 77) {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    EXPECT_EQ(ok.load(), 4);
    EXPECT_EQ((*loads)[77], 1);

    auto h = cache.lookup(77);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(**h, 77);
    EXPECT_EQ((*loads)[77], 1);
  }
}

TEST(ConcurrentCacheManager, SlowMappingConcurrentEvictionAndAccessConsistencyAcrossDelays) {
  constexpr auto cache_size{2};

  for (const auto delay : slow_mapping_test_delays()) {
    SCOPED_TRACE(testing::Message() << "delay_ms=" << delay.count());

    auto loads = std::make_shared<std::unordered_map<key_t, int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto cache =
        alc::make_cache<key_t, value_t>(cache_size, make_slow_count_mapping(loads, mtx, delay), alc::policies::lfu);

    EXPECT_EQ(**cache.lookup(0), 0);
    EXPECT_EQ(**cache.lookup(0), 0);
    EXPECT_EQ(**cache.lookup(0), 0);
    EXPECT_EQ(**cache.lookup(1), 1);

    std::barrier start(2);

    std::thread t_eviction([&] {
      start.arrive_and_wait();

      auto h = cache.lookup(2);
      ASSERT_TRUE(h.has_value());
      EXPECT_EQ(**h, 2);
    });

    std::thread t_access([&] {
      start.arrive_and_wait();

      auto h = cache.lookup(1);
      ASSERT_TRUE(h.has_value());
      EXPECT_EQ(**h, 1);
    });

    t_eviction.join();
    t_access.join();

    auto h = cache.lookup(1);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(**h, 1);

    EXPECT_EQ((*loads)[0], 1);
    EXPECT_EQ((*loads)[2], 1);
    EXPECT_GE((*loads)[1], 1);
    EXPECT_LE((*loads)[1], 2);
  }
}

TEST(ConcurrentCacheManager, SlowMappingManyThreadsTwoKeysSingleLoadPerKeyAcrossDelays) {
  constexpr auto cache_size{2};

  for (const auto delay : slow_mapping_test_delays()) {
    SCOPED_TRACE(testing::Message() << "delay_ms=" << delay.count());

    auto loads = std::make_shared<std::unordered_map<key_t, int>>();
    auto mtx = std::make_shared<std::mutex>();

    auto cache =
        alc::make_cache<key_t, value_t>(cache_size, make_slow_count_mapping(loads, mtx, delay), alc::policies::lfu);

    std::atomic<int> ok{0};
    std::barrier start(16);

    std::vector<std::thread> threads;
    threads.reserve(16);

    for (int i = 0; i < 16; ++i) {
      threads.emplace_back([&, i] {
        start.arrive_and_wait();

        const auto key = static_cast<key_t>((i % 2) ? 5 : 6);
        auto h = cache.lookup(key);

        if (h && **h == key) {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    EXPECT_EQ(ok.load(), 16);
    EXPECT_EQ((*loads)[5], 1);
    EXPECT_EQ((*loads)[6], 1);
  }
}

// Removed budget, so this test should probably be removed?
//
// TEST(ConcurrentCacheManager, ConcurrentThreadsInsertPricesWhileUnderBudget) {
//   constexpr auto cache_size{64};
//   constexpr alc::price_t budget{128};
//
//   auto loads = std::make_shared<std::unordered_map<key_t, int>>();
//
//   auto policy = alc::policy_factory<alc::common_priority_t, alc::blank_summary_t>::create(
//       alc::common_priority_t{0},
//       [](alc::timestamp_t t) { return static_cast<alc::common_priority_t>(t); },
//       [](alc::common_priority_t a, alc::common_priority_t b) { return std::max(a, b); },
//       []() { return alc::price_t{1}; }
//   );
//
//   auto cache{alc::cache_factory<key_t, value_t>::create(cache_size, loadCounter{loads}, policy, budget)};
//
//   std::atomic<int> ok{0};
//   std::barrier start(32);
//
//   std::vector<std::thread> threads;
//   threads.reserve(32);
//   for (int i = 0; i < 32; ++i) {
//     threads.emplace_back([&, i] {
//       start.arrive_and_wait();
//       auto h = cache.lookup(static_cast<key_t>(200 + i));
//       if (h) ok.fetch_add(1, std::memory_order_relaxed);
//     });
//   }
//
//   for (auto& t : threads) t.join();
//
//   EXPECT_EQ(ok.load(), 32);
//   int loaded = 0;
//   for (int i = 0; i < 32; ++i) loaded += (*loads)[200 + i];
//   EXPECT_EQ(loaded, 32);
// }

} // namespace
