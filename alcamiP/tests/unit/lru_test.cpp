#include <functional>
#include <unordered_map>

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


TEST(LruCache, StandardLookup) {
  constexpr auto cache_size{4};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lru)};

  EXPECT_EQ(**cache.lookup(0), 0);
  EXPECT_EQ(**cache.lookup(1), 1);
  EXPECT_EQ(**cache.lookup(2), 2);
  EXPECT_EQ(**cache.lookup(3), 3);
  EXPECT_EQ(**cache.lookup(4), 4);
  EXPECT_EQ(**cache.lookup(5), 5);
  EXPECT_EQ(**cache.lookup(6), 6);
}

TEST(LruCache, EvictsLeastRecentlyUsed) {
  constexpr auto cache_size{4};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lru)};

  EXPECT_EQ(**cache.lookup(1), 1);
  EXPECT_EQ(**cache.lookup(2), 2);
  EXPECT_EQ(**cache.lookup(3), 3);
  EXPECT_EQ(**cache.lookup(4), 4);

  EXPECT_EQ(**cache.lookup(1), 1);
  EXPECT_EQ(**cache.lookup(5), 5);

  EXPECT_EQ((*loads)[2], 1);
  EXPECT_EQ(**cache.lookup(2), 2);
  EXPECT_EQ((*loads)[2], 2);
}

TEST(LruCache, EvictsAvailableSlot) {
  constexpr auto cache_size{4};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lru)};


  auto h1 = cache.lookup(1);
  auto h2 = cache.lookup(2);

  {
    auto h3 = cache.lookup(3);
    auto h4 = cache.lookup(4);

    EXPECT_EQ(**h1, 1);
    EXPECT_EQ(**h2, 2);
    EXPECT_EQ(**h3, 3);
    EXPECT_EQ(**h4, 4);

    auto h5 = cache.lookup(5);

    EXPECT_FALSE(h5.has_value());

    ASSERT_TRUE((*loads)[1] == 1);
    ASSERT_TRUE((*loads)[2] == 1);
    ASSERT_TRUE((*loads)[3] == 1);
    ASSERT_TRUE((*loads)[4] == 1);
  }

  EXPECT_EQ(**cache.lookup(5), 5);
  EXPECT_EQ(**cache.lookup(1), 1);
  EXPECT_EQ(**cache.lookup(2), 2);

  ASSERT_TRUE((*loads)[1] == 1);
  ASSERT_TRUE((*loads)[2] == 1);
  ASSERT_TRUE((*loads)[3] == 1);
  ASSERT_TRUE((*loads)[4] == 1);
  ASSERT_TRUE((*loads)[5] == 2);
}


} // namespace
