#include <concepts>
#include <functional>
#include <ranges>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gsl/gsl>

#include <alcami/cache.h>
#include <alcami/common.h>
#include <alcami/predefined.h>
#include <alcami/summary.h>


namespace ranges = std::ranges;
namespace views = std::views;


namespace {

using size_t = alc::cache_size_t;
using key_t = size_t;
using value_t = key_t;

// using cache_factory_t = alc::cache_factory<key_t, value_t>;
const auto make_cache = alc::make_cache<key_t, value_t>;
const auto identity_mapping = alc::mapping_adapter<key_t>([](key_t k) { return k; });


// tests for cache_manager that are independent of the eviction policy
template <std::default_initializable PolicyFn>
requires std::invocable<PolicyFn>
class GenericCacheTest : public ::testing::Test {
public:
  // type parameter is a functor that returns the eviction policy to use with the test
  using policy_type = std::invoke_result_t<PolicyFn>;
  policy_type policy = std::invoke(PolicyFn{});
};


// NOTE: Add policies here!
//
// define functors that each return a policy to use as a test parameter
constexpr auto lru{[]() { return alc::policies::lru; }};
constexpr auto lfu{[]() { return alc::policies::lfu; }};


// NOTE: Add policies here!
//
// these must be types of default-initializable functors that return the policy
// this is because each parameter to the test must be specified as a type
using cache_test_policies = ::testing::Types<decltype(lru), decltype(lfu)>;


// NOTE: Add policies here!
//
// return what to name the test based on the type
class cache_test_names {
public:
  template <typename T>
  static auto GetName([[maybe_unused]] int i) -> std::string {
    if constexpr (std::same_as<T, decltype(lru)>) {
      return "lru";
    }

    if constexpr (std::same_as<T, decltype(lfu)>) {
      return "lfu";
    }
  }
};


TYPED_TEST_SUITE(GenericCacheTest, cache_test_policies, cache_test_names);


constexpr auto fixed_size{1};
constexpr auto fixed_cost{1};

// a simple summarizer for the purposes of writing generic tests
auto fixed_summarizer([[maybe_unused]] const value_t& v) { return alc::common_summary{fixed_size, fixed_cost}; }


// checks queries assuming an identity mapping
auto run_identity_test(auto& cache, ranges::range auto& queries) -> void {
  for (auto key : queries) {
    auto value{cache.lookup(key)};

    ASSERT_TRUE(value);

    // clang-tidy has trouble telling that value was checked
    if (value) {
      EXPECT_EQ(value->get(), key);
    }
  }
}


TYPED_TEST(GenericCacheTest, LessQueriesThanCapacity) {
  constexpr auto capacity{4};
  constexpr auto query_count{capacity - 1};

  // use identity mapping
  auto cache{make_cache(capacity, identity_mapping, this->policy, fixed_summarizer)};
  auto queries{views::iota(key_t{0}, gsl::narrow<key_t>(query_count)) | views::transform([&](auto i) { return i; })};

  run_identity_test(cache, queries);
}


TYPED_TEST(GenericCacheTest, MoreQueriesThanCapacity) {
  constexpr auto capacity{4};
  constexpr auto query_count{(2 * capacity) + 1};

  // use identity mapping
  auto cache{make_cache(capacity, identity_mapping, this->policy, fixed_summarizer)};
  auto queries{views::iota(key_t{0}, gsl::narrow<key_t>(query_count)) |
               views::transform([&](auto i) { return i % capacity; })};

  run_identity_test(cache, queries);
}

} // namespace
