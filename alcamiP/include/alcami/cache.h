///
/// \file
///
/// Provides the cache manager along with supporting concepts, functions, and types.

#ifndef ALCAMI_INCLUDE_CACHE_H
#define ALCAMI_INCLUDE_CACHE_H

#include <cstddef>
#include <gsl/util>
#include <optional>

#include <gtl/phmap.hpp>

#include "alcami/rapidhash.h"
#include "common.h"
#include "options.h"
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

template <searchable K, storable V, mapping<K, V> Map, eviction_policy Pol,
          summarizer<K, typename Pol::summary_type> Summ, statistics_mode Stats = void>
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
  using shard_type = shard<K, V, Map, Pol, Summ, Stats>;

  cache_manager(size_type capacity, mapping_type mapping, policy_type pol, summarizer_type summarizer = {},
                cache_options options = {})
      : capacity_{capacity}, options_{options} {

    shards_.reserve(m_shard_count_());

    for (std::size_t i = 0; i < m_shard_count_(); ++i) {
      shards_.emplace_back(capacity_, mapping, pol, summarizer, options_.append_log_capacity,
                           options_.append_log_buffer_count, options_.evictions_per_cycle);
    }
  }

  cache_manager(const cache_manager&) = delete;

  auto operator=(const cache_manager&) -> cache_manager& = delete;

  cache_manager(cache_manager&&) noexcept = default;

  auto operator=(cache_manager&&) noexcept -> cache_manager& = default;

  ~cache_manager() noexcept = default;

  [[nodiscard]]
  auto capacity() const noexcept -> size_type {
    return capacity_;
  }

  [[nodiscard]]
  auto hit_percent() const noexcept -> double {
    if constexpr (!statistics_enabled) {
      return 0.0;
    } else {
      double total = 0.0;

      for (auto& shard : shards_) {
        total += shard.hit_percent();
      }

      return total / gsl::narrow_cast<double>(shards_.size());
    }
  }

  [[nodiscard]]
  auto average_drain_size() const noexcept -> double {
    if constexpr (!statistics_enabled) {
      return 0.0;
    } else {
      double total = 0.0;

      for (auto& shard : shards_) {
        total += shard.average_drain_size();
      }

      return total / gsl::narrow_cast<double>(shards_.size());
    }
  }

  /// Returns a **lifetime-constrained** reference to the data that the mapping associates with the given key.
  ///
  /// The data referenced by the returned handle lives only as long as the handle, and the handle lives only as long
  /// as this object.
  ///
  /// Returns no value if all slots in the cache are being accessed.
  ///
  /// \see `capacity()`
  [[nodiscard]]
  auto lookup(key_type key) const -> std::optional<handle_type> {
    const std::size_t hash = hash_(key);
    size_t index = hash & (m_shard_count_() - 1);
    return shards_[index].lookup(key);
  }

private:
  static constexpr bool statistics_enabled = std::same_as<Stats, statistics>;

  struct hasher {
    template <typename T>
    auto operator()(const T& key) const -> std::size_t {
      if constexpr (std::integral<T>) {
        return gsl::narrow_cast<std::size_t>(rapidhash(&key, sizeof(key)));

      } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {

        std::string_view view{key};

        return gsl::narrow_cast<std::size_t>(rapidhash(view.data(), view.size()));

      } else {
        return std::hash<T>{}(key);
      }
    }
  };

  [[nodiscard]]
  auto m_shard_count_() const noexcept -> std::size_t {
    return std::size_t{1} << options_.shard_power;
  }

  std::vector<shard_type> shards_{};
  [[no_unique_address]]
  hasher hash_{};
  size_type capacity_{};
  cache_options options_{};
};


namespace detail {
/// Implementation of `alc::make_cache`. Should not be used directly.
///
/// We implement `alc::make_cache` as a _niebloid_, which allows us to take K and V while deducing the other template
/// parameters.
template <searchable K, storable V, statistics_mode Stats = void>
class make_cache_impl {
public:
  template <mapping<K, V> Map, eviction_policy Pol,
            summarizer<K, typename Pol::summary_type> Summ = blank_summarizer_t<K>>
  [[nodiscard]]
  auto operator()(cache_size_t capacity, Map mapping, Pol pol, Summ summarizer = {}, cache_options options = {}) const {
    return cache_manager<K, V, Map, Pol, Summ, Stats>{capacity, mapping, pol, summarizer, options};
  }

  template <mapping<K, V> Map, eviction_policy Pol>
  [[nodiscard]]
  auto operator()(cache_size_t capacity, Map mapping, Pol pol, cache_options options) const {
    return cache_manager<K, V, Map, Pol, blank_summarizer_t<K>, Stats>{capacity, mapping, pol, {}, options};
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
