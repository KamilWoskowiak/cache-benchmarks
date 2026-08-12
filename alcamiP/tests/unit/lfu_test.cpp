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


TEST(LfuCache, EvictsLeastFrequentlyUsed) {
  constexpr auto cache_size{4};

  auto loads = std::make_shared<std::unordered_map<key_t, int>>();

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, make_count_mapping(loads), alc::policies::lfu)};

  // Key -> Frequency

  EXPECT_EQ(**cache.lookup(2), 2); // 2 -> 1

  EXPECT_EQ(**cache.lookup(1), 1); // 1 -> 1
  EXPECT_EQ(**cache.lookup(1), 1); // 1 -> 2

  EXPECT_EQ(**cache.lookup(2), 2); // 2 -> 2

  EXPECT_EQ(**cache.lookup(4), 4); // 4 -> 1

  EXPECT_EQ(**cache.lookup(3), 3); // 3 -> 1

  EXPECT_EQ(**cache.lookup(3), 3); // 3 -> 2

  EXPECT_EQ(**cache.lookup(1), 1); // 1 -> 3

  // LRU should evict 2 but since we are using LFU it should evict 4

  EXPECT_EQ(**cache.lookup(5), 5); // 5 -> 1 (should evict 4)

  EXPECT_EQ(**cache.lookup(4), 4); // We load 4 a second time

  EXPECT_EQ((*loads)[1], 1);
  EXPECT_EQ((*loads)[2], 1);
  EXPECT_EQ((*loads)[3], 1);
  EXPECT_EQ((*loads)[4], 2);
  EXPECT_EQ((*loads)[5], 1);

}

} // namespace
