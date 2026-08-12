///
/// \file
///
/// Provides the cache manager along with supporting concepts, functions, and types.

#ifndef ALCAMI_INCLUDE_CACHE_H
#define ALCAMI_INCLUDE_CACHE_H

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

#include <gtl/phmap.hpp>

#include "common.h"
#include "shard.h"

namespace alc {


/// \defgroup cache Caching
///
/// \copydoc cache.h
///
/// The `cache_manager` class acts as an associative array that caches values. To use `cache_manager`, you must provide:
///
/// * A _mapping_, which is a function object that the cache uses to look up values. The mapping can be any type that
///   satisfies the `alc::mapping` concept.
/// * A _policy_, which specifies the eviction policy that the cache follows. The policy should of type `alc::policy`.
/// * A _summarizer_. Every policy depends on some metadata about keys in the cache, called the _summary_. The
///   summarizer transforms a key into the summary type expected by the policy. The summarizer can be any type that
///   satisfies the `alc::summarizer` concept, provided that the types are compatible with the policy.
///
/// To create a `cache_manager` object, use the `alc::make_cache` function object.
///
/// ```cpp
/// auto my_capacity = 8;
/// auto my_mapping = alc::mapping_adapter<int>([](int i) { return 2 * i; });
/// auto my_policy = alc::policies::lru;
/// auto my_summary = alc::blank_summarizer<int>;
///
/// auto my_cache = alc::make_cache<int, int>(my_capacity, my_mapping,
///                                           my_policy, my_summary);
/// ```
///
/// The `cache_manager` has a `lookup` method, which returns a `std::optional<cache_handle>`. The value returned by
/// `lookup` is only empty if the cache was unable to load the value. The handle acts as a pointer to the value in the
/// cache. The value referenced by the handle will only be evicted after the handle's destructor is called.
///
/// ```cpp
/// auto h = my_cache.lookup(7);
/// assert(h.has_value());
/// assert(**h == 14);
/// ```
///
/// \{

struct cache_options {
  size_t append_log_capacity{64};
  size_t append_log_buffer_count{8};
  size_t shard_power{0};
  size_t evictions_per_cycle{8};
};

template <searchable K, storable V, mapping<K, V> Map, eviction_policy Pol,
          summarizer<K, typename Pol::summary_type> Summ>
class cache_manager {
public:
  /// Type of the key used to look up values.
  using key_type = K;

  /// Type of the cached data.
  using value_type = V;

  /// Type of the function object used to load the cache's contents.
  using mapping_type = Map;

  /// Type of the eviction policy.
  using policy_type = Pol;

  /// Type that summarizes a value's contents.
  using summary_type = typename Pol::summary_type;

  /// Type of the function object that extracts the summary from a value.
  using summarizer_type = Summ;

  /// Type that can store the maximum capacity of the cache.
  using size_type = cache_size_t;

  /// Type that manages a reference to an item stored in the cache.
  using handle_type = cache_handle<value_type>;

  /// Type used to denote a single shard of the cache
  using shard_type = shard<K, V, Map, Pol, Summ>;

  cache_manager(size_type capacity, mapping_type mapping, policy_type pol, summarizer_type summarizer = {},
                cache_options options = {})
      : capacity_{capacity}, shard_count_{size_type{1} << options.shard_power}, shard_mask_{shard_count_ - 1},
        shards_{std::allocator_traits<shard_allocator_type>::allocate(shard_allocator_, shard_count_)} {
    size_type base_capacity = capacity_ / shard_count_;
    size_type remainder = capacity_ % shard_count_;

    for (size_t constructed{0}; constructed < shard_count_; ++constructed) {
      const size_type shard_capacity = base_capacity + (constructed < remainder ? 1 : 0);
      std::allocator_traits<shard_allocator_type>::construct(
          shard_allocator_, shards_ + constructed, shard_capacity, mapping, pol, summarizer,
          options.append_log_capacity, options.append_log_buffer_count, options.evictions_per_cycle);
    }
  }

  cache_manager(const cache_manager&) = delete;
  auto operator=(const cache_manager&) -> cache_manager& = delete;

  cache_manager(cache_manager&& other) noexcept
      : capacity_{std::exchange(other.capacity_, 0)}, shard_count_{std::exchange(other.shard_count_, 0)},
        shard_mask_{std::exchange(other.shard_mask_, 0)}, shards_{std::exchange(other.shards_, nullptr)} {}

  auto operator=(cache_manager&& other) noexcept -> cache_manager& {
    if (this != &other) {
      m_destroy_shards_();
      capacity_ = std::exchange(other.capacity_, 0);
      shard_count_ = std::exchange(other.shard_count_, 0);
      shard_mask_ = std::exchange(other.shard_mask_, 0);
      shards_ = std::exchange(other.shards_, nullptr);
    }

    return *this;
  }

  ~cache_manager() noexcept { m_destroy_shards_(); }

  [[nodiscard]]
  auto capacity() const noexcept -> size_type {
    return capacity_;
  }

  [[nodiscard]]
  auto hit_percent() const noexcept -> double {
    double hit_percent = 0.0;

    for (size_type i = 0; i < shard_count_; ++i) {
      hit_percent += shards_[i].hit_percent();
    }

    return hit_percent / static_cast<double>(shard_count_);
  }

  [[nodiscard]]
  auto lookup(key_type key) const -> std::optional<handle_type> {
    const std::size_t hash = gtl::priv::hash_default_hash<key_type>{}(key);
    return shards_[hash & shard_mask_].lookup(key);
  }

private:
  using shard_allocator_type = std::allocator<shard_type>;

  auto m_destroy_shards_() noexcept -> void {
    if (!shards_) {
      return;
    }

    for (size_type i = shard_count_; i != 0; --i) {
      std::allocator_traits<shard_allocator_type>::destroy(shard_allocator_, shards_ + i - 1);
    }

    std::allocator_traits<shard_allocator_type>::deallocate(shard_allocator_, shards_, shard_count_);
    shards_ = nullptr;
    shard_count_ = 0;
    shard_mask_ = 0;
    capacity_ = 0;
  }

  size_type capacity_{};
  size_type shard_count_{};
  size_type shard_mask_{};
  [[no_unique_address]]
  shard_allocator_type shard_allocator_{};
  shard_type* shards_{};
};


namespace detail {
/// Implementation of `alc::make_cache`. Should not be used directly.
///
/// We implement `alc::make_cache` as a _niebloid_, which allows us to take K and V while deducing the other template
/// parameters.
template <searchable K, storable V>
class make_cache_impl {
public:
  template <mapping<K, V> Map, eviction_policy Pol,
            summarizer<K, typename Pol::summary_type> Summ = blank_summarizer_t<K>>
  [[nodiscard]]
  auto operator()(cache_size_t capacity, Map mapping, Pol pol, Summ summarizer = {}, cache_options options = {}) const {
    return cache_manager<K, V, Map, Pol, Summ>{capacity, mapping, pol, summarizer, options};
  }

  template <mapping<K, V> Map, eviction_policy Pol>
  [[nodiscard]]
  auto operator()(cache_size_t capacity, Map mapping, Pol pol, cache_options options) const {
    return cache_manager<K, V, Map, Pol, blank_summarizer_t<K>>{capacity, mapping, pol, {}, options};
  }
};
} // namespace detail


/// Function object used to create an `alc::cache_manager`.
///
/// \tparam K Key type
/// \tparam V Value type
///
/// ---
///
/// **Call Signature**
///
/// ```cpp
/// template < mapping<K, V> Map, eviction_policy Pol,
///            summarizer<K, typename Pol::summary_type>
///                Summ = blank_summarizer_t<K> >
/// [[nodiscard]]
/// auto make_cache<K, V>( cache_size_t capacity, Map map,
///                        Pol pol, Summ summ = {}, cache_options options = {} )
///     -> cache_manager< K, V, Map, Pol, Summ >;
/// ```
///
/// \param capacity Maximum number of handles to distinct elements
/// \param mapping Returns the value to associate with a given key
/// \param pol Determines the cache's eviction policy
/// \param summarizer Converts a key to the metadata expected by the policy
///
/// \hideinitializer
///
template <searchable K, storable V>
inline const auto make_cache = detail::make_cache_impl<K, V>{};


/// \}


} // namespace alc

#endif // ALCAMI_INCLUDE_CACHE_H
