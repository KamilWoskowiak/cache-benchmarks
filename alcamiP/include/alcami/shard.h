///
/// \file
///
/// Provides the cache manager along with supporting concepts, functions, and types.

#ifndef ALCAMI_INCLUDE_SHARD_H
#define ALCAMI_INCLUDE_SHARD_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/heap/d_ary_heap.hpp>
#include <gsl/narrow>

#include <tbb/concurrent_queue.h>
#include <tbb/spin_mutex.h>
#include <tbb/spin_rw_mutex.h>

#include <gtl/phmap.hpp>

#include "alcami/common.h"
#include "append_log.h"
#include "gtl/phmap_fwd_decl.hpp"
#include "mapping.h"
#include "policy.h"
#include "summary.h"

namespace alc {

/// Type that can store the maximum capacity of a `alc::cache_manager`.
using cache_size_t = std::size_t;

namespace detail {
/// Type that stores the lifetime of each handle managed by `alc::cache_manager`.
using ref_count_t = std::atomic<std::size_t>;
} // namespace detail

/// References a **lifetime-constrained** piece of cached data.
///
/// The handle does not own the referenced data and must be outlived by the owner. If so, then references to the data
/// are invalidated only once the handle is destructed.
///
/// A handle cannot be null. A handle cannot be default constructed. A handle cannot be copied.
///
/// \tparam T Type of the referenced data
template <storable V>
class cache_handle {
public:
  /// Type of the referenced data.
  using value_type = V;

  /// The default constructor is disabled since a handle cannot be null.
  ///
  /// If you need a logically empty handle to be a valid state, try using `std::optional`.
  cache_handle() = delete;
  cache_handle(const cache_handle&) = delete;
  auto operator=(const cache_handle&) -> cache_handle& = delete;

  ~cache_handle() noexcept {
    if (ref_count_) {
      ref_count_->fetch_sub(1, std::memory_order_release);
    }
  }

  cache_handle(cache_handle&& other) noexcept
      : value_ptr_{std::exchange(other.value_ptr_, nullptr)}, ref_count_{std::exchange(other.ref_count_, nullptr)} {}

  auto operator=(cache_handle&& other) noexcept -> cache_handle& {
    if (this != &other) {
      if (ref_count_) {
        ref_count_->fetch_sub(1, std::memory_order_release);
      }

      value_ptr_ = std::exchange(other.value_ptr_, nullptr);
      ref_count_ = std::exchange(other.ref_count_, nullptr);
    }

    return *this;
  }

  /// Returns a reference to the underlying data if valid, otherwise is undefined behavior.
  ///
  /// If the handle outlives the owner of the underlying data, then the reference is invalid. The reference is
  /// invalidated when the handle is destructed.
  [[nodiscard]]
  auto get() const noexcept -> const value_type& {
    return *value_ptr_;
  }

  /// Alias for `cache_handle::get`.
  [[nodiscard]]
  auto operator*() const noexcept -> const value_type& {
    return *value_ptr_;
  }

  /// Alias for `cache_handle::get`.
  [[nodiscard]]
  auto operator->() const noexcept -> const value_type* {
    return value_ptr_;
  }

private:
  /// Grants the owning `cache_manager` access to the private constructor so it alone can mint handles.
  template <searchable K_, storable V_, mapping<K_, V_> Map_, eviction_policy Pol_,
            summarizer<K_, typename Pol_::summary_type> Summ_, statistics_mode Stats_>
  friend class shard;

  /// Creates a handle from non-owning pointers to a cache slot and its reference counter.
  /// The owning cache must increment the counter before constructing the handle.
  cache_handle(const value_type* value, detail::ref_count_t* ref_count) noexcept
      : value_ptr_{value}, ref_count_{ref_count} {}

  /// Non-owning pointer to the value stored in the owning cache's slot.
  const value_type* value_ptr_{};

  /// Non-owning pointer to the owning slot's atomic reference counter.
  detail::ref_count_t* ref_count_{};
};

/// An associative array that caches values according to an eviction policy.
///
/// \tparam K Key type
/// \tparam V Value type
/// \tparam Map Type of the function object that loads the cache's contents
/// \tparam Pol Type of the eviction policy (i.e., an `alc::policy`)
/// \tparam Summ Type of the function object that returns a key's metadata
///
/// \see `alc::policy`
template <searchable K, storable V, mapping<K, V> Map, eviction_policy Pol,
          summarizer<K, typename Pol::summary_type> Summ, statistics_mode Stats = void>
class shard {
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

  /// Type of the priority used by the eviction policy.
  using priority_type = typename policy_type::priority_type;

  // Users should be able to use a handle as a pointer.
  static_assert(std::indirectly_readable<handle_type>);

  /// Creates a new cache. In most cases, use `alc::make_cache` instead of invoking the constructor directly. (This
  /// allows for template argument deduction.)
  ///
  /// \see `alc::make_cache`
  shard(size_type capacity, mapping_type mapping, policy_type pol, summarizer_type summarizer,
        size_type append_log_capacity, size_type append_log_buffer_count, size_type eviction_batch_size)
      : capacity_{capacity}, invalid_index_{capacity + 1}, mapping_{mapping}, summarizer_{summarizer}, policy_{pol},
        entry_values_(capacity), entry_ref_counts_(capacity), entry_metadata_(capacity),
        entry_occupied_(capacity, false), access_log_(append_log_buffer_count, append_log_capacity),
        eviction_batch_size_(eviction_batch_size), heap_(heap_compare{&entry_metadata_}), entry_heap_handles_(capacity),
        entry_heap_prio_stale(capacity, false) {
    resident_.reserve(capacity_);

    dirty_slots_stack_.reserve(capacity_);
    deferred_slots_.reserve(capacity_);

    for (std::size_t slot = 0; slot < capacity_; ++slot) {
      entry_metadata_[slot].key = key_type{};
      entry_metadata_[slot].priority = policy_.identity;

      entry_ref_counts_[slot].value.store(0, std::memory_order_relaxed);
      entry_heap_handles_[slot].handle = heap_.push(slot);
    }
  }

  shard(const shard&) = delete;
  auto operator=(const shard&) -> shard& = delete;

  shard(shard&& other)
      : capacity_{other.capacity_}, invalid_index_{other.invalid_index_}, mapping_{std::move(other.mapping_)},
        summarizer_{std::move(other.summarizer_)}, policy_{std::move(other.policy_)},
        resident_{std::move(other.resident_)}, entry_values_{std::move(other.entry_values_)},
        entry_ref_counts_{std::move(other.entry_ref_counts_)}, entry_metadata_{std::move(other.entry_metadata_)},
        entry_occupied_{std::move(other.entry_occupied_)}, access_log_{std::move(other.access_log_)},
        clock_{other.clock_}, eviction_batch_size_{other.eviction_batch_size_}, heap_{heap_compare{&entry_metadata_}},
        entry_heap_handles_(capacity_), entry_heap_prio_stale{std::move(other.entry_heap_prio_stale)},
        dirty_slots_stack_{std::move(other.dirty_slots_stack_)}, deferred_slots_{std::move(other.deferred_slots_)},
        lookup_count_{other.lookup_count_.load(std::memory_order_relaxed)},
        hit_count_{other.hit_count_.load(std::memory_order_relaxed)} {
    assert(other.misses_.empty());
    assert(other.waiters_.load(std::memory_order_relaxed) == 0);
    assert(!other.drain_active_.load(std::memory_order_relaxed));

    m_rebuild_after_move_(other);
  }

  auto operator=(shard&&) -> shard& = delete;

  ~shard() noexcept = default;

  /// Returns the number of slots in the cache. This is the maximum number of handles that may reference distinct keys.
  [[nodiscard]]
  auto capacity() const noexcept -> size_type {
    return capacity_;
  }

  /// Returns the percentage of lookups that were cache hits.
  [[nodiscard]]
  auto hit_percent() const noexcept -> double {
    const std::size_t lookups = lookup_count_.load(std::memory_order_relaxed);

    if (lookups == 0) {
      return 0.0;
    }

    const std::size_t hits = hit_count_.load(std::memory_order_relaxed);
    /// Narrows, but okay because we are okay with being lossy (probably wont happen)
    return (100.0 * gsl::narrow_cast<double>(hits)) / gsl::narrow_cast<double>(lookups);
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
  auto lookup(const key_type& key) const -> std::optional<handle_type> {
    if constexpr (statistics_enabled) {
      lookup_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t slot = invalid_index_;

    const bool found = resident_.if_contains(key, [this, &slot](const typename resident_map_type::value_type& kv) {
      slot = kv.second;
      entry_ref_counts_[slot].value.fetch_add(1, std::memory_order_relaxed);
    });

    if (found) [[likely]] {
      if constexpr (statistics_enabled) {
        hit_count_.fetch_add(1, std::memory_order_relaxed);
      }

      return m_make_pinned_handle_(slot);
    }

    miss_entry* miss = nullptr;
    bool owns_miss = false;

    {
      oneapi::tbb::spin_mutex::scoped_lock miss_lock(miss_mutex_);

      slot = invalid_index_;

      const bool became_resident =
          resident_.if_contains(key, [this, &slot](const typename resident_map_type::value_type& kv) {
            slot = kv.second;
            entry_ref_counts_[slot].value.fetch_add(1, std::memory_order_relaxed);
          });

      if (became_resident) {
        miss_lock.release();

        if constexpr (statistics_enabled) {
          hit_count_.fetch_add(1, std::memory_order_relaxed);
        }

        return m_make_pinned_handle_(slot);
      }

      auto miss_it = misses_.find(key);

      if (miss_it == misses_.end()) {
        auto [it, inserted] = misses_.try_emplace(key);
        assert(inserted);

        miss = &it->second;
        owns_miss = true;
      } else {
        miss = &miss_it->second;

        const miss_status status = miss->status.load(std::memory_order_acquire);

        if (status == miss_status::loading) {
          ++miss->waiters;
        } else if (status == miss_status::failed) {
          return std::nullopt;
        } else {
          assert(false);
          return std::nullopt;
        }
      }
    }

    if (!owns_miss) {
      return m_wait_for_miss_(key, miss);
    }

    return m_load_miss_(key, miss);
  }

private:
  static constexpr bool statistics_enabled = std::same_as<Stats, statistics>;
  static constexpr std::size_t num_submap_pow = 8;

  /// Branching factor used by the eviction heap.
  static constexpr std::size_t heap_arity = 4;

  /// Object for lookups waiting for a in-progress entry.
  enum class miss_status : std::uint8_t {
    loading,
    ready,
    failed,
  };

  /// Metadata stored for each cache slot.
  struct metadata {
    key_type key{};
    priority_type priority{};
  };

  /// Compares slot indices by their current priorities.
  struct heap_compare {
    auto operator()(std::size_t a, std::size_t b) const noexcept -> bool {
      return (*entries_metadata)[a].priority > (*entries_metadata)[b].priority;
    }

    const std::vector<metadata>* entries_metadata;
  };

  struct miss_entry {
    std::atomic<miss_status> status{miss_status::loading};
    std::size_t slot{};
    std::size_t waiters{};
  };

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

  /// Ref count storage for one cache slot. Keeps neighboring counters off the same cache line.
  struct alignas(std::hardware_destructive_interference_size) ref_count {
    detail::ref_count_t value{0};
  };

  /// Index and optional latch stored for each key. The latch is populated while the index is invalid.
  /// Map from key to its slot and, while loading, the latch shared by waiting lookups.
  using resident_map_type =
      gtl::parallel_flat_hash_map<key_type, std::size_t, hasher, gtl::priv::hash_default_eq<key_type>,
                                  gtl::priv::Allocator<std::pair<const key_type, std::size_t>>, num_submap_pow,
                                  oneapi::tbb::spin_rw_mutex>;

  using miss_map_type = gtl::node_hash_map<key_type, miss_entry, gtl::priv::hash_default_hash<key_type>,
                                           gtl::priv::hash_default_eq<key_type>>;

  /// Lock-free log of recently accessed slots.
  using log_type = append_log<std::size_t>;

  /// Heap of slot indices ordered by eviction priority.
  using heap_type = boost::heap::d_ary_heap<std::size_t, boost::heap::arity<heap_arity>, boost::heap::mutable_<true>,
                                            boost::heap::compare<heap_compare>>;

  using heap_handle_type = typename heap_type::handle_type;

  /// Stores a slot's mutable-heap handle.
  struct heap_link {
    heap_handle_type handle{};
  };

  /// Maximum number of slots the cache may occupy.
  size_type capacity_{};

  /// Sentinel stored while a key is being loaded.
  std::size_t invalid_index_{};

  /// Loads a value for a key on a cache miss.
  mapping_type mapping_{};

  /// Converts a key into policy summary data.
  summarizer_type summarizer_{};

  /// Policy used to rank and combine slot priorities.
  policy_type policy_{};

  /// Maps each cached key to its slot and in-progress-load latch.
  mutable resident_map_type resident_;

  mutable oneapi::tbb::spin_mutex miss_mutex_;
  mutable miss_map_type misses_;

  /// Cached values, indexed by slot. Hot on successful lookups.
  alignas(std::hardware_destructive_interference_size) mutable std::vector<value_type> entry_values_;

  /// Live handle count for each slot. Each element is cache-line aligned to avoid false sharing.
  alignas(std::hardware_destructive_interference_size) mutable std::vector<ref_count> entry_ref_counts_;

  /// Per-slot key and priority data. Hot during access-log drains and eviction.
  alignas(std::hardware_destructive_interference_size) mutable std::vector<metadata> entry_metadata_;

  /// True when a slot currently stores a mapped key.
  mutable std::vector<bool> entry_occupied_;

  /// Deferred accesses used to update slot priorities.
  mutable log_type access_log_;

  /// Guards access-log priority merges.
  mutable oneapi::tbb::spin_mutex log_mutex_;

  /// Monotonic counter used when assigning priorities.
  mutable timestamp_t clock_{0};

  /// Maximum slots produced by one eviction pass.
  mutable std::size_t eviction_batch_size_{8};

  /// Guards heap changes and eviction passes.
  mutable oneapi::tbb::spin_mutex evict_mutex_;

  /// Heap used to choose eviction slots.
  /// TODO: Boost does not use an array-based heap when `mutable_<true>` is enabled.
  mutable heap_type heap_;

  /// Heap handles for each slot.
  mutable std::vector<heap_link> entry_heap_handles_;

  /// True when a slot needs a heap update.
  mutable std::vector<bool> entry_heap_prio_stale;

  /// Slots with changed priorities.
  mutable std::vector<std::size_t> dirty_slots_stack_;

  /// Slots removed from the heap until the next drain.
  mutable std::vector<std::size_t> deferred_slots_;

  /// Count of miss handlers waiting for a slot.
  alignas(std::hardware_destructive_interference_size) mutable std::atomic<std::size_t> waiters_{0};

  /// Reserved flag for active drain coordination.
  alignas(std::hardware_destructive_interference_size) mutable std::atomic<bool> drain_active_{false};

  /// Queue of slots ready for reuse.
  mutable oneapi::tbb::concurrent_bounded_queue<std::size_t> free_slots_;

  /// Total lookup calls.
  alignas(std::hardware_destructive_interference_size) mutable std::atomic<std::size_t> lookup_count_{0};

  /// Total cache hits.
  alignas(std::hardware_destructive_interference_size) mutable std::atomic<std::size_t> hit_count_{0};

  [[no_unique_address]]
  hasher hash_{};

  static auto m_spin_pause_() noexcept -> void {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
  }

  /// Waits for the owner of an invalid-index entry to finish, then lets the caller retry the map lookup.
  [[nodiscard]]
  auto m_wait_for_miss_(const key_type& key, miss_entry* miss) const -> std::optional<handle_type> {
    assert(miss != nullptr);

    while (miss->status.load(std::memory_order_relaxed) == miss_status::loading) {
      m_spin_pause_();
    }

    const miss_status status = miss->status.load(std::memory_order_acquire);
    std::size_t slot = invalid_index_;

    if (status == miss_status::ready) {
      slot = miss->slot;
    }

    {
      oneapi::tbb::spin_mutex::scoped_lock lock(miss_mutex_);

      assert(miss->waiters > 0);
      --miss->waiters;

      if (miss->waiters == 0) {
        const auto it = misses_.find(key);
        assert(it != misses_.end());
        assert(&it->second == miss);
        misses_.erase(it);
      }
    }

    if (status != miss_status::ready) {
      return std::nullopt;
    }

    return m_make_pinned_handle_(slot);
  }

  [[nodiscard]]
  auto m_load_miss_(const key_type& key, miss_entry* miss) const -> std::optional<handle_type> {
    assert(miss != nullptr);

    waiters_.fetch_add(1, std::memory_order_acq_rel);
    m_produce_slots_();

    value_type loaded_value{};
    auto error = mapping_(key, loaded_value);

    if (error.has_value()) {
      m_fail_miss_(key, miss);
      // TODO: we need a way to return std::error to the user
      return std::nullopt;
    }

    std::size_t free_slot{invalid_index_};
    free_slots_.pop(free_slot);

    if (free_slot == invalid_index_) {
      m_fail_miss_(key, miss);
      return std::nullopt;
    }

    return m_fill_miss_(key, free_slot, std::move(loaded_value), miss);
  }

  /// Publishes a loaded slot or clears a failed load. Wakes every waiter holding the entry's shared latch.
  /// Cancels a failed load and clears its invalid-index entry.
  auto m_fail_miss_(const key_type& key, miss_entry* miss) const -> void {
    oneapi::tbb::spin_mutex::scoped_lock lock(miss_mutex_);

    assert(miss != nullptr);
    assert(miss->status.load(std::memory_order_relaxed) == miss_status::loading);

    miss->status.store(miss_status::failed, std::memory_order_release);

    if (miss->waiters == 0) {
      const auto it = misses_.find(key);
      assert(it != misses_.end());
      assert(&it->second == miss);
      misses_.erase(it);
    }
  }

  /// Produces reusable slots for waiting misses.
  auto m_produce_slots_() const -> void {
    oneapi::tbb::spin_mutex::scoped_lock lock(evict_mutex_);

    const std::size_t pending = waiters_.load(std::memory_order_acquire);

    if (pending == 0) {
      return;
    }

    const std::size_t batch = std::min(pending, eviction_batch_size_);
    waiters_.fetch_sub(batch, std::memory_order_acq_rel);

    m_drain_heap_();

    for (std::size_t i = 0; i < batch; ++i) {
      free_slots_.push(m_evict_slot_());
    }
  }

  /// Logs a hit and returns a handle for an already-pinned slot.
  [[nodiscard]]
  auto m_make_pinned_handle_(std::size_t slot) const -> std::optional<handle_type> {
    (void)access_log_.merge_with(
        slot, [this](log_type::iterator begin, log_type::iterator end) { m_merge_accesses_(begin, end); });

    return handle_type{&entry_values_[slot], &entry_ref_counts_[slot].value};
  }

  /// Stores a loaded value in a slot and returns a handle.
  [[nodiscard]]
  auto m_fill_miss_(const key_type& key, std::size_t slot, value_type value, miss_entry* miss) const
      -> std::optional<handle_type> {
    metadata& entry = entry_metadata_[slot];

    entry.key = key;
    entry.priority = policy_.identity;
    entry_values_[slot] = std::move(value);

    {
      oneapi::tbb::spin_mutex::scoped_lock lock(evict_mutex_);
      entry_occupied_[slot] = true;
      deferred_slots_.push_back(slot);
    }

    {
      oneapi::tbb::spin_mutex::scoped_lock miss_lock(miss_mutex_);

      assert(miss != nullptr);
      assert(miss->status.load(std::memory_order_relaxed) == miss_status::loading);
      assert(entry_ref_counts_[slot].value.load(std::memory_order_relaxed) == 0);

      entry_ref_counts_[slot].value.store(miss->waiters + 1, std::memory_order_relaxed);

      const bool inserted = resident_.lazy_emplace_l(
          key, [slot](typename resident_map_type::value_type& kv) { assert(kv.second == slot); },
          [&key, slot](const auto& ctor) { ctor(key, slot); });

      assert(inserted);

      miss->slot = slot;
      miss->status.store(miss_status::ready, std::memory_order_release);

      if (miss->waiters == 0) {
        const auto miss_it = misses_.find(key);
        assert(miss_it != misses_.end());
        assert(&miss_it->second == miss);
        misses_.erase(miss_it);
      }
    }

    return m_make_pinned_handle_(slot);
  }

  /// Iterates through access log and updates slot priorities.
  auto m_merge_accesses_(typename log_type::iterator begin, typename log_type::iterator end) const -> void {
    if (begin == end) {
      return;
    }

    oneapi::tbb::spin_mutex::scoped_lock lock(log_mutex_);

    for (auto it = begin; it != end; ++it) {
      const std::size_t slot = *it;
      metadata& entry = entry_metadata_[slot];

      summary_type summary = summarizer_(entry.key);
      priority_type access_prio = policy_.prioritizer(clock_++, summary);
      entry.priority = policy_.combiner(entry.priority, access_prio);

      if (!entry_heap_prio_stale[slot]) {
        entry_heap_prio_stale[slot] = true;
        dirty_slots_stack_.push_back(slot);
      }
    }
  }

  /// Drains access records and refreshes the heap.
  auto m_drain_heap_() const -> void {
    (void)access_log_.drain(
        [this](log_type::iterator begin, log_type::iterator end) { m_merge_accesses_(begin, end); });

    oneapi::tbb::spin_mutex::scoped_lock lock(log_mutex_);

    for (const std::size_t slot : deferred_slots_) {
      if (!entry_occupied_[slot]) {
        continue;
      }

      heap_link& meta = entry_heap_handles_[slot];
      meta.handle = heap_.push(slot);
    }

    deferred_slots_.clear();

    for (const std::size_t slot : dirty_slots_stack_) {
      entry_heap_prio_stale[slot] = false;

      if (!entry_occupied_[slot]) {
        continue;
      }

      heap_link& meta = entry_heap_handles_[slot];
      heap_.update(meta.handle);
    }

    dirty_slots_stack_.clear();
  }

  /// Returns an empty or evicted slot for reuse, or `invalid_index_` if no slot is available.
  auto m_evict_slot_() const -> std::size_t {
    while (!heap_.empty()) {
      const std::size_t slot = heap_.top();
      heap_.pop();

      metadata& slot_entry = entry_metadata_[slot];

      // Empty slot. It is already removed from the heap and will be reinserted after the miss owner fills it.
      if (!entry_occupied_[slot]) {
        return slot;
      }

      const bool erased =
          resident_.erase_if(slot_entry.key, [this, slot](const typename resident_map_type::value_type& kv) {
            return kv.second == slot && entry_ref_counts_[slot].value.load(std::memory_order_acquire) == 0;
          });

      if (erased) {
        entry_occupied_[slot] = false;
        slot_entry.key = key_type{};
        slot_entry.priority = policy_.identity;
        return slot;
      }

      deferred_slots_.push_back(slot);
    }

    return invalid_index_;
  }

  auto m_rebuild_after_move_(shard& other) -> void {
    std::vector<bool> excluded(capacity_, false);

    for (const std::size_t slot : deferred_slots_) {
      if (slot < capacity_) {
        excluded[slot] = true;
      }
    }

    std::size_t slot = invalid_index_;
    while (other.free_slots_.try_pop(slot)) {
      free_slots_.push(slot);

      if (slot < capacity_) {
        excluded[slot] = true;
      }
    }

    std::fill(entry_heap_prio_stale.begin(), entry_heap_prio_stale.end(), false);
    dirty_slots_stack_.clear();

    for (std::size_t i = 0; i < capacity_; ++i) {
      if (!excluded[i]) {
        entry_heap_handles_[i].handle = heap_.push(i);
      }
    }
  }
};

} // namespace alc

#endif // ALCAMI_INCLUDE_SHARD_H
