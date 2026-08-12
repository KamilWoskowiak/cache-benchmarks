#include <algorithm>
#include <iterator>
#include <numeric>
#include <ranges>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <alcami/predefined.h>

namespace ranges = std::ranges;
namespace views = std::views;

namespace {

// Tag each access with its timestamp.
template <typename T>
[[nodiscard]] auto create_timestamped(const std::vector<T>& accesses) {
  std::vector<std::pair<alc::timestamp_t, char>> timestamped;
  ranges::transform(views::iota(0), accesses, std::back_inserter(timestamped),
                    [](auto i, auto e) { return std::make_pair(i, e); });

  return timestamped;
}


// Return type of create_timestamped.
template <typename T>
using timestamped_t = decltype(create_timestamped(std::vector<T>{}));


// Compute the priority of a particular key given the timestamped accesses to all keys.
template <typename T, typename Policy>
auto get_priority(const T& key, timestamped_t<T>& timestamped, Policy& policy) {
  auto priorities{timestamped | views::filter([&](auto& x) { return x.second == key; }) | views::elements<0> |
                  views::transform([&](auto i) { return policy.prioritizer(i, {}); })};

  auto combined{std::accumulate(priorities.begin(), priorities.end(), policy.identity, policy.combiner)};

  return combined;
}


TEST(LruPolicyTest, UniqueAccesses) {
  std::vector accesses{'a', 'b', 'c', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lru};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 0);
  EXPECT_EQ(b_prio, 1);
  EXPECT_EQ(c_prio, 2);
  EXPECT_EQ(d_prio, 3);
}


TEST(LruPolicyTest, ReportsLatestAccess) {
  // every element is accessed twice in a row
  std::vector accesses{'a', 'a', 'b', 'b', 'c', 'c', 'd', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lru};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 1);
  EXPECT_EQ(b_prio, 3);
  EXPECT_EQ(c_prio, 5);
  EXPECT_EQ(d_prio, 7);
}


TEST(LruPolicyTest, AccessedFirstAndLast) {
  // 'a' is accessed both first and last
  std::vector accesses{'a', 'b', 'c', 'd', 'a'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lru};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 4);
  EXPECT_EQ(b_prio, 1);
  EXPECT_EQ(c_prio, 2);
  EXPECT_EQ(d_prio, 3);
}


TEST(LfuPolicyTest, AccessedOnceEach) {
  std::vector accesses{'a', 'b', 'c', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lfu};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 1);
  EXPECT_EQ(b_prio, 1);
  EXPECT_EQ(c_prio, 1);
  EXPECT_EQ(d_prio, 1);
}


TEST(LfuPolicyTest, AccessedTwiceEach) {
  std::vector accesses{'a', 'a', 'b', 'b', 'c', 'c', 'd', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lfu};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 2);
  EXPECT_EQ(b_prio, 2);
  EXPECT_EQ(c_prio, 2);
  EXPECT_EQ(d_prio, 2);
}


TEST(LfuPolicyTest, RepeatsAccessSequence) {
  std::vector accesses{'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lfu};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 2);
  EXPECT_EQ(b_prio, 2);
  EXPECT_EQ(c_prio, 2);
  EXPECT_EQ(d_prio, 2);
}


TEST(LfuPolicyTest, AccessedAgainFirst) {
  // 'b' is accessed an additional time at the beginning
  std::vector accesses{'b', 'a', 'b', 'c', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lfu};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 1);
  EXPECT_EQ(b_prio, 2);
  EXPECT_EQ(c_prio, 1);
  EXPECT_EQ(d_prio, 1);
}


TEST(LfuPolicyTest, AccessedAgainLast) {
  // 'b' is accessed an additional time at the end
  std::vector accesses{'a', 'b', 'c', 'd', 'b'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::lfu};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_EQ(a_prio, 1);
  EXPECT_EQ(b_prio, 2);
  EXPECT_EQ(c_prio, 1);
  EXPECT_EQ(d_prio, 1);
}


TEST(FifoPolicyTest, AccessedOnce) {
  std::vector accesses{'a', 'b', 'c', 'd'};

  auto timestamped{create_timestamped(accesses)};
  auto policy{alc::policies::fifo};

  auto a_prio{get_priority('a', timestamped, policy)};
  auto b_prio{get_priority('b', timestamped, policy)};
  auto c_prio{get_priority('c', timestamped, policy)};
  auto d_prio{get_priority('d', timestamped, policy)};

  EXPECT_LT(a_prio, b_prio);
  EXPECT_LT(b_prio, c_prio);
  EXPECT_LT(c_prio, d_prio);
}

} // namespace
