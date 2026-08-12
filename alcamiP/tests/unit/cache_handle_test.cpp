#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <alcami/cache.h>
#include <alcami/predefined.h>

namespace {

using size_t = alc::cache_size_t;
using key_t = size_t;
using value_t = key_t;

const auto identity_mapping = alc::mapping_adapter<key_t>([](key_t k) { return k; });

TEST(CacheHandleTest, StandardLookupReturnsValue) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  auto h1 = cache.lookup(4);
  EXPECT_TRUE(h1.has_value());
  ASSERT_EQ(**h1, 4);

  {
    auto h2 = cache.lookup(2);
    EXPECT_TRUE(h2.has_value());
    ASSERT_EQ(**h2, 2);
  }

  auto h3 = cache.lookup(3);
  EXPECT_TRUE(h3.has_value());
  ASSERT_EQ(**h3, 3);
}

TEST(CacheHandleTest, EvictionNotPossible) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  auto h1 = cache.lookup(4);
  EXPECT_TRUE(h1.has_value());
  ASSERT_EQ(**h1, 4);

  auto h2 = cache.lookup(5);
  EXPECT_TRUE(h2.has_value());
  ASSERT_EQ(**h2, 5);

  auto h3 = cache.lookup(4);
  EXPECT_TRUE(h3.has_value());
  ASSERT_EQ(**h3, 4);

  auto h4 = cache.lookup(12);
  EXPECT_FALSE(h4.has_value());
}

TEST(CacheHandleTest, MultipleHandlesPreventEvictionUntilAllDestroyed) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  auto h1 = cache.lookup(4);
  auto h2 = cache.lookup(5);
  auto h3 = cache.lookup(4);
  auto h4 = cache.lookup(7);

  ASSERT_TRUE(h1.has_value());
  ASSERT_TRUE(h2.has_value());
  ASSERT_TRUE(h3.has_value());
  ASSERT_FALSE(h4.has_value());

  h1.reset();
  h3.reset();

  auto h5 = cache.lookup(7);

  ASSERT_TRUE(h5.has_value());
  EXPECT_EQ(**h5, 7);
}

TEST(CacheHandleTest, GetDerefArrowReturnsConsistentValue) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  auto h1 = *(cache.lookup(4));

  ASSERT_EQ(h1.get(), 4);

  ASSERT_EQ(*h1, 4);

  const value_t* p = h1.operator->();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 4);

  EXPECT_EQ(p, &(*h1));
}

TEST(CacheHandleTest, MoveConstructor) {
  constexpr auto cache_size{1};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  {
    auto h1 = cache.lookup(4);
    ASSERT_TRUE(h1.has_value());
    EXPECT_EQ(**h1, 4);

    auto h2 = std::move(*h1);

    EXPECT_EQ(*h2, 4);
  }

  // Check to see if old handle can be evicted after moving (ref count is correct)
  auto h3 = cache.lookup(2);
  ASSERT_TRUE(h3.has_value());
  EXPECT_EQ(**h3, 2);
}

TEST(CacheHandleTest, MoveAssignment) {
  constexpr auto cache_size{2};

  [[maybe_unused]]
  auto cache{alc::make_cache<key_t, value_t>(cache_size, identity_mapping, alc::policies::lfu)};

  auto h1 = cache.lookup(1);
  auto h2 = cache.lookup(2);
  ASSERT_TRUE(h1.has_value());
  ASSERT_TRUE(h2.has_value());

  *h2 = std::move(*h1);
  EXPECT_EQ(**h2, 1);

  // Check to see if h1 can be evicted after moving (ref count is correct)
  auto h3 = cache.lookup(3);
  ASSERT_TRUE(h3.has_value());
  EXPECT_EQ(**h3, 3);
}

} // namespace
