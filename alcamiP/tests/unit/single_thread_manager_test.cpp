#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <alcami/cache.h>
#include <alcami/predefined.h>


namespace {

using size_t = alc::cache_size_t;
using key_t = size_t;
using value_t = key_t;
using load_count_t = int;

const auto identity_mapping = alc::mapping_adapter<key_t>([](key_t k) { return k; });


struct loadCounter {
  loadCounter() = default;
  loadCounter(std::shared_ptr<std::unordered_map<key_t, load_count_t>> counter) : counts{std::move(counter)} {}

  // NOTE: this needs to be const for loadCounter to be regular_invokable
  auto operator()(key_t key) const -> value_t {
    ++(*counts)[key];
    return key;
  }

  // NOTE: make this mutable so loadCounter is regular_invokable, assume that we never use loadCounter concurrently
  mutable std::shared_ptr<std::unordered_map<key_t, int>> counts;
};


[[nodiscard]]
auto make_count_mapping(std::shared_ptr<std::unordered_map<key_t, int>> counter) {
  return alc::mapping_adapter<key_t>(loadCounter{std::move(counter)});
}


TEST(SingleThreadCacheManager, CorrectCapacityValue) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  ASSERT_EQ(cache.capacity(), cache_size);

  auto h1 = cache.lookup(0);
  auto h2 = cache.lookup(1);
  auto h3 = cache.lookup(2);

  ASSERT_EQ(**h1, 0);
  ASSERT_EQ(**h2, 1);
  ASSERT_FALSE(h3.has_value());
}

// NOTE: removed budget, so I think we can remove this test ??
//
// TEST(SingleThreadCacheManager, CorrectBudgetValue) {
//     constexpr auto cache_size{2};
//
//     [[maybe_unused]]
//     auto cache{alc::cache_factory<key_t, value_t>::create(cache_size, std::identity{}, alc::lfu(), 200)};
//
//     ASSERT_EQ(cache.budget(), (size_t)200);
//
//     [[maybe_unused]]
//     auto cache2{alc::cache_factory<key_t, value_t>::create(cache_size, std::identity{}, alc::lfu())};
//
//     ASSERT_EQ(cache2.budget(), (size_t)0);
// }

TEST(SingleThreadCacheManager, CacheMissesThenLoads) {
  constexpr auto cache_size{4};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  EXPECT_EQ(**(cache.lookup(0)), 0);
  EXPECT_EQ(**(cache.lookup(1)), 1);
  EXPECT_EQ(**(cache.lookup(2)), 2);
  EXPECT_EQ(**(cache.lookup(3)), 3);
  EXPECT_EQ(**(cache.lookup(4)), 4);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
  EXPECT_EQ((*loads)[3], 1);
  EXPECT_EQ((*loads)[4], 1);
}

TEST(SingleThreadCacheManager, HitsReturnWithoutRemapping) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  EXPECT_EQ(**(cache.lookup(0)), 0);
  EXPECT_EQ(**(cache.lookup(1)), 1);
  EXPECT_EQ(**(cache.lookup(0)), 0);
  EXPECT_EQ(**(cache.lookup(1)), 1);
  EXPECT_EQ(**(cache.lookup(1)), 1);
  EXPECT_EQ(**(cache.lookup(2)), 2);
  EXPECT_EQ(**(cache.lookup(2)), 2);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
}

TEST(SingleThreadCacheManager, NulloptWhenCap0) {
  constexpr auto cache_size{0};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  EXPECT_EQ(cache.lookup(0), std::nullopt);
  EXPECT_EQ(cache.lookup(1), std::nullopt);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
}

TEST(SingleThreadCacheManager, NulloptWhenNoEvictableSpots) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  auto h1 = cache.lookup(0);
  ASSERT_TRUE(h1.has_value());
  EXPECT_EQ(**h1, 0);

  auto h2 = cache.lookup(1);
  ASSERT_TRUE(h2.has_value());
  EXPECT_EQ(**h2, 1);

  auto h3 = cache.lookup(2);
  EXPECT_EQ(h3, std::nullopt);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
}

TEST(SingleThreadCacheManager, ValuePreservedUntilEvicted) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  ASSERT_TRUE(cache.lookup(0).has_value());
  ASSERT_TRUE(cache.lookup(0).has_value());

  ASSERT_TRUE(cache.lookup(1).has_value());

  ASSERT_TRUE(cache.lookup(2).has_value());

  ASSERT_TRUE(cache.lookup(0).has_value());

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
}

TEST(SingleThreadCacheManager, Evicts0RefEntry) {
  constexpr auto cache_size{2};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  auto h1 = cache.lookup(0);
  ASSERT_TRUE(h1.has_value());
  EXPECT_EQ(**h1, 0);

  {
    auto h2 = cache.lookup(1);
    ASSERT_TRUE(h2.has_value());
    EXPECT_EQ(**h2, 1);

    auto h3 = cache.lookup(1);
    ASSERT_TRUE(h3.has_value());
    EXPECT_EQ(**h3, 1);
  }

  {
    auto h3 = cache.lookup(1);
    ASSERT_TRUE(h3.has_value());
    EXPECT_EQ(**h3, 1);
  }

  auto h4 = cache.lookup(2);
  ASSERT_TRUE(h4.has_value());
  EXPECT_EQ(**h4, 2);

  auto h5 = cache.lookup(0);
  ASSERT_TRUE(h5.has_value());
  EXPECT_EQ(**h5, 0);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
}

TEST(SingleThreadCacheManager, HeapOrdering) {
  constexpr auto cache_size{3};

  auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  ASSERT_TRUE(cache.lookup(0).has_value()); // 0 -> 1
  ASSERT_TRUE(cache.lookup(1).has_value()); // 1 -> 1
  ASSERT_TRUE(cache.lookup(1).has_value()); // 1 -> 2
  ASSERT_TRUE(cache.lookup(2).has_value()); // 2 -> 1
  ASSERT_TRUE(cache.lookup(1).has_value()); // 1 -> 2
  ASSERT_TRUE(cache.lookup(0).has_value()); // 0 -> 2

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);

  // Priority should be 2 < 0 < 1, we should evict in that order

  auto h1 = cache.lookup(4);
  ASSERT_TRUE(h1.has_value());
  auto h2 = cache.lookup(2);

  EXPECT_EQ((*loads)[0], 1);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 2);

  auto h3 = cache.lookup(0);

  EXPECT_EQ((*loads)[0], 2);
  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 2);
}

// NOTE: removed budget, so I think we can remove this test ??
//
// TEST(SingleThreadCacheManager, MissWhenOverBudgetNothingEvictable) {
//     constexpr auto cache_size{2};
//     constexpr alc::price_t item_cost{3};
//     constexpr alc::price_t budget{5};
//
//     auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();
//
//     auto policy = alc::policy_factory<alc::common_priority_t, alc::common_summary>::create(
//         alc::common_priority_t{0},
//         [](alc::timestamp_t t) { return static_cast<alc::common_priority_t>(t); },
//         [](alc::common_priority_t a, alc::common_priority_t b) { return std::max(a, b); },
//         [](const alc::common_summary& s) { return s.cost(); }
//     );
//     auto summarizer = [](key_t) { return alc::common_summary{1, item_cost}; };
//
//     auto cache = alc::cache_factory<key_t, value_t>::create(
//         cache_size, loadCounter{loads}, policy, budget, summarizer);
//
//     auto h0 = cache.lookup(0);
//     ASSERT_TRUE(h0.has_value());
//     EXPECT_EQ(**h0, 0);
//
//     auto h1 = cache.lookup(1);
//     EXPECT_EQ(h1, std::nullopt);
//
//     EXPECT_EQ((*loads)[0], 1);
//     EXPECT_EQ((*loads)[1], 0);
// }

// NOTE: removed budget, so I think we can remove this test ??
//
// TEST(SingleThreadCacheManager, EvictEnoughToMaintainBudget) {
//     constexpr auto cache_size{1};
//     constexpr alc::price_t budget{4};
//
//     auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();
//
//     auto policy = alc::policy_factory<alc::common_priority_t, alc::common_summary>::create(
//         alc::common_priority_t{0},
//         [](alc::timestamp_t t) { return static_cast<alc::common_priority_t>(t); },
//         [](alc::common_priority_t a, alc::common_priority_t b) { return std::max(a, b); },
//         [](const alc::common_summary& s) { return s.cost(); }
//     );
//     auto summarizer = [](key_t k) {
//         if (k == 0) return alc::common_summary{1, 3};
//         if (k == 7) return alc::common_summary{1, 2};
//         return alc::common_summary{1, 1};
//     };
//
//     auto cache = alc::cache_factory<key_t, value_t>::create(
//         cache_size, loadCounter{loads}, policy, budget, summarizer);
//
//     auto h0 = cache.lookup(0);
//     ASSERT_TRUE(h0.has_value());
//     EXPECT_EQ(**h0, 0);
//
//     auto h7_fail = cache.lookup(7);
//     EXPECT_EQ(h7_fail, std::nullopt);
//
//     h0.reset();
//
//     auto h7 = cache.lookup(7);
//     ASSERT_TRUE(h7.has_value());
//     EXPECT_EQ(**h7, 7);
//
//     h7.reset();
//
//     auto h0_again = cache.lookup(0);
//     ASSERT_TRUE(h0_again.has_value());
//     EXPECT_EQ(**h0_again, 0);
//
//     EXPECT_EQ((*loads)[0], 2);
//     EXPECT_EQ((*loads)[7], 1);
// }

// NOTE: removed budget, so I think we can remove this test ??
//
// TEST(SingleThreadCacheManager, PriceOf0DoesNotChangeBudget) {
//     constexpr auto cache_size{3};
//     constexpr alc::price_t budget{2};
//
//     auto loads = std::make_shared<std::unordered_map<key_t, load_count_t>>();
//
//     auto policy = alc::policy_factory<alc::common_priority_t, alc::common_summary>::create(
//         alc::common_priority_t{0},
//         [](alc::timestamp_t t) { return static_cast<alc::common_priority_t>(t); },
//         [](alc::common_priority_t a, alc::common_priority_t b) { return std::max(a, b); },
//         [](const alc::common_summary& s) {
//             return (s.size() == -1) ? alc::price_t{0} : alc::price_t{s.cost()};
//         }
//     );
//     auto summarizer = [](key_t k) {
//         if (k == 2)  return alc::common_summary{1, 2};
//         if (k == 99) return alc::common_summary{1, 1};
//         return alc::common_summary{-1, 1};
//     };
//
//     auto cache = alc::cache_factory<key_t, value_t>::create(
//         cache_size, loadCounter{loads}, policy, budget, summarizer);
//
//     auto h2 = cache.lookup(2);
//     ASSERT_TRUE(h2.has_value());
//     EXPECT_EQ(**h2, 2);
//
//     {
//         auto h10 = cache.lookup(10);
//         auto h11 = cache.lookup(11);
//         ASSERT_TRUE(h10.has_value());
//         ASSERT_TRUE(h11.has_value());
//         EXPECT_EQ(**h10, 10);
//         EXPECT_EQ(**h11, 11);
//     }
//
//     auto h99 = cache.lookup(99);
//     EXPECT_EQ(h99, std::nullopt);
//
//     EXPECT_EQ((*loads)[2], 1);
//     EXPECT_EQ((*loads)[10], 1);
//     EXPECT_EQ((*loads)[11], 1);
//     EXPECT_EQ((*loads)[99], 0);
// }

} // namespace
