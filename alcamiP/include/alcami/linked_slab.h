///
/// \file
///
/// Stable slab storage with asynchronous value publication.
///

#ifndef ALCAMI_INCLUDE_LINKED_SLAB_H
#define ALCAMI_INCLUDE_LINKED_SLAB_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace alc {

/// Stable slab storage whose entries may be allocated before their value is constructed.
///
/// `insert()` reserves a slab entry and returns a generation-bearing token immediately. The
/// entry starts in the `loading` state with no value. Its owner later calls `emplace()` to
/// construct and publish the value, or `fail()` to wake waiters without publishing a value.
///
/// Waiters use `wait()` rather than inspecting the `std::optional` directly. The state and
/// generation live in one atomic control word, so a waiter cannot accidentally sleep on a
/// recycled slot belonging to a later generation.
///
/// The backing vector is sized once in the constructor and never resized. Consequently,
/// addresses of published values remain stable until their token is removed.
///
/// `insert()` and `remove()` serialize freelist mutation internally. Publication and waiting
/// are lock-free apart from the implementation of `std::atomic::wait`.
template <typename T>
class linked_slab {
public:
  using value_type = T;
  using token_type = std::uint64_t;
  using size_type = std::size_t;

  static constexpr token_type invalid_token = 0;

  enum class wait_result : std::uint8_t {
    ready,
    failed,
    stale,
  };

  explicit linked_slab(size_type capacity) : vec_(capacity), next_free_(capacity == 0 ? 0U : 1U) {
    assert(capacity <= max_index_count_);

    for (size_type i = 0; i < capacity; ++i) {
      vec_[i].free_next = (i + 1 < capacity) ? static_cast<std::uint32_t>(i + 2) : 0U;
    }
  }

  linked_slab(const linked_slab&) = delete;
  linked_slab(linked_slab&&) = delete;
  auto operator=(const linked_slab&) -> linked_slab& = delete;
  auto operator=(linked_slab&&) -> linked_slab& = delete;
  ~linked_slab() = default;

  /// Allocates an empty slab entry in the loading state.
  ///
  /// Returns zero when the physical slab storage is exhausted.
  [[nodiscard]]
  auto insert() -> token_type {
    std::lock_guard lock(free_mutex_);

    if (next_free_ == 0) {
      return invalid_token;
    }

    const std::uint32_t one_based_index = next_free_;
    slab_entry& entry = vec_[one_based_index - 1];
    next_free_ = entry.free_next;

    const std::uint32_t old_generation = generation_from_control_(entry.control.load(std::memory_order_relaxed));
    std::uint32_t generation = old_generation + 1U;

    // Generation zero is never emitted by a valid token.
    if (generation == 0U) {
      generation = 1U;
    }

    assert(!entry.value.has_value());

    const token_type token = make_token_(one_based_index, generation);

    entry.next = token;
    entry.prev = token;
    entry.free_next = 0;
    entry.control.store(make_control_(generation, entry_state::loading), std::memory_order_release);

    size_.fetch_add(1, std::memory_order_relaxed);
    return token;
  }

  /// Constructs and publishes the value for a loading token.
  ///
  /// A release-store publishes all writes to the value before waiters observe `ready`.
  template <typename... Args>
  auto emplace(token_type token, Args&&... args) -> value_type& {
    slab_entry& entry = entry_for_(token);
    const std::uint32_t generation = token_generation(token);

    assert(entry.control.load(std::memory_order_acquire) == make_control_(generation, entry_state::loading));
    assert(!entry.value.has_value());

    value_type& value = entry.value.emplace(std::forward<Args>(args)...);

    entry.control.store(make_control_(generation, entry_state::ready), std::memory_order_release);
    entry.control.notify_all();

    return value;
  }

  /// Marks a loading entry as failed and wakes every waiter.
  auto fail(token_type token) noexcept -> void {
    if (!valid_index_(token)) {
      return;
    }

    slab_entry& entry = vec_[token_index(token)];
    const std::uint32_t generation = token_generation(token);
    const std::uint64_t expected = make_control_(generation, entry_state::loading);

    if (entry.control.load(std::memory_order_acquire) != expected) {
      return;
    }

    entry.control.store(make_control_(generation, entry_state::failed), std::memory_order_release);
    entry.control.notify_all();
  }

  /// Waits until `token` becomes ready, fails, or is recycled.
  ///
  /// Because generation and state share one atomic control word, recycling a loading entry
  /// changes the value waited on even if the next generation is also in the loading state.
  [[nodiscard]]
  auto wait(token_type token) const noexcept -> wait_result {
    if (!valid_index_(token)) {
      return wait_result::stale;
    }

    const slab_entry& entry = vec_[token_index(token)];
    const std::uint32_t generation = token_generation(token);

    while (true) {
      const std::uint64_t control = entry.control.load(std::memory_order_acquire);

      if (generation_from_control_(control) != generation) {
        return wait_result::stale;
      }

      switch (state_from_control_(control)) {
      case entry_state::ready:
        return wait_result::ready;
      case entry_state::failed:
      case entry_state::vacant:
        return wait_result::failed;
      case entry_state::loading:
        entry.control.wait(control, std::memory_order_acquire);
        break;
      }
    }
  }

  /// Returns the published value at `token`, or nullptr if it is loading, failed, vacant, or stale.
  [[nodiscard]]
  auto get(token_type token) noexcept -> value_type* {
    if (!is_ready_(token)) {
      return nullptr;
    }

    return &*vec_[token_index(token)].value;
  }

  /// Returns the published value at `token`, or nullptr if it is loading, failed, vacant, or stale.
  [[nodiscard]]
  auto get(token_type token) const noexcept -> const value_type* {
    if (!is_ready_(token)) {
      return nullptr;
    }

    return &*vec_[token_index(token)].value;
  }

  /// Returns a published value without a nullable result.
  ///
  /// \pre `get(token) != nullptr`.
  [[nodiscard]]
  auto get_unchecked(token_type token) noexcept -> value_type& {
    value_type* value = get(token);
    assert(value);
    return *value;
  }

  /// Returns a published value without a nullable result.
  ///
  /// \pre `get(token) != nullptr`.
  [[nodiscard]]
  auto get_unchecked(token_type token) const noexcept -> const value_type& {
    const value_type* value = get(token);
    assert(value);
    return *value;
  }

  /// Returns true while the exact generation represented by `token` is allocated.
  [[nodiscard]]
  auto contains(token_type token) const noexcept -> bool {
    if (!valid_index_(token)) {
      return false;
    }

    const std::uint64_t control = vec_[token_index(token)].control.load(std::memory_order_acquire);
    return generation_from_control_(control) == token_generation(token) &&
           state_from_control_(control) != entry_state::vacant;
  }

  /// Returns true only when `token` has a published value.
  [[nodiscard]]
  auto ready(token_type token) const noexcept -> bool {
    return is_ready_(token);
  }

  /// Links a ready, isolated entry immediately before `target_head`.
  ///
  /// If `target_head` is zero, `token` remains an isolated one-element list.
  [[nodiscard]]
  auto link(token_type token, token_type target_head = invalid_token) -> token_type {
    std::lock_guard lock(free_mutex_);

    assert(is_ready_(token));
    slab_entry& entry = vec_[token_index(token)];
    assert(entry.next == token);
    assert(entry.prev == token);

    if (target_head == invalid_token) {
      return token;
    }

    assert(target_head != token);
    assert(is_ready_(target_head));

    slab_entry& head = vec_[token_index(target_head)];
    const token_type tail_token = head.prev;
    assert(is_ready_(tail_token));

    slab_entry& tail = vec_[token_index(tail_token)];
    assert(tail.next == target_head);

    entry.prev = tail_token;
    entry.next = target_head;
    tail.next = token;
    head.prev = token;

    return target_head;
  }

  /// Unlinks a ready entry from its circular list without removing it.
  ///
  /// Returns the following token, or zero when `token` was already alone.
  [[nodiscard]]
  auto unlink(token_type token) -> token_type {
    std::lock_guard lock(free_mutex_);
    assert(is_ready_(token));
    return unlink_locked_(token);
  }

  /// Removes an allocated entry and returns its physical slot to the freelist.
  ///
  /// Loading and failed entries are expected to be isolated. Ready entries are unlinked first.
  /// Waiters are notified before the slot can be reused with a new generation.
  [[nodiscard]]
  auto remove(token_type token) -> bool {
    std::lock_guard lock(free_mutex_);

    if (!valid_index_(token)) {
      return false;
    }

    slab_entry& entry = vec_[token_index(token)];
    const std::uint32_t generation = token_generation(token);
    const std::uint64_t control = entry.control.load(std::memory_order_acquire);

    if (generation_from_control_(control) != generation || state_from_control_(control) == entry_state::vacant) {
      return false;
    }

    if (state_from_control_(control) == entry_state::ready) {
      (void)unlink_locked_(token);
    } else {
      assert(entry.next == token);
      assert(entry.prev == token);
    }

    entry.value.reset();
    entry.next = invalid_token;
    entry.prev = invalid_token;

    entry.control.store(make_control_(generation, entry_state::vacant), std::memory_order_release);
    entry.control.notify_all();

    entry.free_next = next_free_;
    next_free_ = static_cast<std::uint32_t>(token_index(token) + 1);
    size_.fetch_sub(1, std::memory_order_relaxed);

    return true;
  }

  /// Number of allocated entries, including entries still loading.
  [[nodiscard]]
  auto size() const noexcept -> size_type {
    return size_.load(std::memory_order_relaxed);
  }

  /// Number of physical slots owned by the slab.
  [[nodiscard]]
  auto capacity() const noexcept -> size_type {
    return vec_.size();
  }

  /// True when no physical slab slot is currently free.
  [[nodiscard]]
  auto full() const noexcept -> bool {
    return size() == capacity();
  }

  /// Converts a generation-bearing token to its zero-based physical slot index.
  [[nodiscard]]
  static constexpr auto token_index(token_type token) noexcept -> size_type {
    const std::uint32_t one_based_index = static_cast<std::uint32_t>(token & index_mask_);
    return one_based_index == 0 ? 0 : static_cast<size_type>(one_based_index - 1);
  }

  /// Returns the generation encoded in a token.
  [[nodiscard]]
  static constexpr auto token_generation(token_type token) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(token >> generation_shift_);
  }

private:
  enum class entry_state : std::uint8_t {
    vacant = 0,
    loading = 1,
    ready = 2,
    failed = 3,
  };

  static constexpr unsigned state_bits_ = 2;
  static constexpr unsigned generation_shift_ = 32;
  static constexpr token_type index_mask_ = 0xFFFF'FFFFULL;
  static constexpr size_type max_index_count_ = std::numeric_limits<std::uint32_t>::max();

  struct slab_entry {
    // High bits: generation. Low two bits: entry_state.
    std::atomic<std::uint64_t> control{make_control_(0, entry_state::vacant)};
    std::optional<value_type> value;

    token_type next{invalid_token};
    token_type prev{invalid_token};

    // One-based physical index used only while this entry is vacant.
    std::uint32_t free_next{0};
  };

  [[nodiscard]]
  static constexpr auto make_token_(std::uint32_t one_based_index, std::uint32_t generation) noexcept -> token_type {
    return (static_cast<token_type>(generation) << generation_shift_) | one_based_index;
  }

  [[nodiscard]]
  static constexpr auto make_control_(std::uint32_t generation, entry_state state) noexcept -> std::uint64_t {
    return (static_cast<std::uint64_t>(generation) << state_bits_) | static_cast<std::uint64_t>(state);
  }

  [[nodiscard]]
  static constexpr auto generation_from_control_(std::uint64_t control) noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(control >> state_bits_);
  }

  [[nodiscard]]
  static constexpr auto state_from_control_(std::uint64_t control) noexcept -> entry_state {
    return static_cast<entry_state>(control & ((std::uint64_t{1} << state_bits_) - 1));
  }

  [[nodiscard]]
  auto valid_index_(token_type token) const noexcept -> bool {
    if (token == invalid_token || token_generation(token) == 0) {
      return false;
    }

    const std::uint32_t one_based_index = static_cast<std::uint32_t>(token & index_mask_);
    return one_based_index != 0 && one_based_index <= vec_.size();
  }

  [[nodiscard]]
  auto is_ready_(token_type token) const noexcept -> bool {
    if (!valid_index_(token)) {
      return false;
    }

    const std::uint64_t control = vec_[token_index(token)].control.load(std::memory_order_acquire);
    return generation_from_control_(control) == token_generation(token) &&
           state_from_control_(control) == entry_state::ready;
  }

  auto entry_for_(token_type token) -> slab_entry& {
    assert(valid_index_(token));
    return vec_[token_index(token)];
  }

  [[nodiscard]]
  auto unlink_locked_(token_type token) -> token_type {
    slab_entry& entry = vec_[token_index(token)];
    const token_type prev_token = entry.prev;
    const token_type next_token = entry.next;

    if (next_token == token) {
      assert(prev_token == token);
      return invalid_token;
    }

    assert(is_ready_(prev_token));
    assert(is_ready_(next_token));

    slab_entry& previous = vec_[token_index(prev_token)];
    slab_entry& next_entry = vec_[token_index(next_token)];

    assert(previous.next == token);
    assert(next_entry.prev == token);

    previous.next = next_token;
    next_entry.prev = prev_token;
    entry.next = token;
    entry.prev = token;

    return next_token;
  }

  std::vector<slab_entry> vec_;

  // Protects freelist and circular-list mutation. Value publication uses the atomic control
  // word and does not take this lock.
  mutable std::mutex free_mutex_;
  std::uint32_t next_free_{0};
  std::atomic<size_type> size_{0};
};

} // namespace alc

#endif // ALCAMI_INCLUDE_LINKED_SLAB_H
