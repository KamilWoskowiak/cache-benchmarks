/// \file
///
/// Append log data structure.

#ifndef ALCAMI_INCLUDE_APPEND_LOG_H
#define ALCAMI_INCLUDE_APPEND_LOG_H

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace alc {

template <typename T>
struct slot {
  T value;
  std::atomic<size_t> turn{0};
};

struct alignas(std::hardware_destructive_interference_size) metadata {
  /// Monotonic claim cursor local to this physical buffer.
  ///
  /// If buffer_capacity == B:
  ///
  /// generation 0: [0, B)
  /// generation 1: [B, 2B)
  /// generation 2: [2B, 3B)
  ///
  /// Unlike the previous tail field this is never reset to zero, which avoids
  /// an ABA problem when drainers race across circular-buffer generations.
  std::atomic<size_t> claimed{0};
};

template <typename T, typename Allocator = std::allocator<T>>
class append_log {
public:
  using slot_type = slot<T>;
  using metadata_type = metadata;

  class iterator {
  public:
    iterator() = default;

    [[nodiscard]]
    auto operator*() const -> T& {
      while (slot_->turn.load(std::memory_order_acquire) != turn_) {
        // yield
      }

      return slot_->value;
    }

    [[nodiscard]]
    auto operator->() const -> T* {
      return &**this;
    }

    auto operator++() -> iterator& {
      ++slot_;
      return *this;
    }

    [[nodiscard]]
    auto operator==(const iterator& other) const noexcept -> bool {
      return slot_ == other.slot_;
    }

    [[nodiscard]]
    auto operator!=(const iterator& other) const noexcept -> bool {
      return !(*this == other);
    }

  private:
    friend class append_log;

    iterator(slot_type* slot, size_t turn) noexcept : slot_(slot), turn_(turn) {}

    slot_type* slot_{nullptr};
    size_t turn_{0};
  };

  append_log(size_t buffer_count, size_t buffer_capacity)
      : buffer_count_(buffer_count), buffer_capacity_(buffer_capacity), capacity_(buffer_count * buffer_capacity),
        buffer_capacity_mask_(buffer_capacity - 1), capacity_mask_(capacity_ - 1),
        buffer_capacity_shift_(std::countr_zero(buffer_capacity)), capacity_shift_(std::countr_zero(capacity_)) {
    if (!std::has_single_bit(buffer_count_) || !std::has_single_bit(buffer_capacity_)) {
      throw std::invalid_argument("buffer_count and buffer_capacity must be powers of two");
    }

    metadata_buffers_ = new metadata_type[buffer_count_];
    slots_ = new slot_type[capacity_];
  }

  append_log(const append_log&) = delete;

  auto operator=(const append_log&) -> append_log& = delete;

  append_log(append_log&&) = delete;

  auto operator=(append_log&&) -> append_log& = delete;

  ~append_log() {
    delete[] metadata_buffers_;
    delete[] slots_;
  }

  /// Append one value.
  ///
  /// Returns true if this call also claimed and drained a range.
  ///
  /// The supplied function may execute concurrently with functions from other
  /// merge_with()/drain() calls, including for disjoint ranges belonging to
  /// the same physical buffer.
  template <typename F>
  [[nodiscard]]
  auto merge_with(T value, F&& function) const noexcept -> bool {
    const size_t ticket = head_.fetch_add(1, std::memory_order_relaxed);

    const size_t turnhalf = ticket >> capacity_shift_;

    const size_t index = ticket & capacity_mask_;

    const size_t turn = 2 * turnhalf;

    slot_type& target_slot = slots_[index];

    while (target_slot.turn.load(std::memory_order_acquire) != turn) {
      // yield
    }

    target_slot.value = std::move(value);

    target_slot.turn.store(turn + 1, std::memory_order_release);

    const size_t offset = index & buffer_capacity_mask_;

    if (offset != buffer_capacity_mask_) {
      return false;
    }

    const size_t buffer_index = index >> buffer_capacity_shift_;

    // This ticket is the final ticket in this physical buffer for this
    // generation.
    //
    // ticket + 1 is therefore sufficient to expose the entire generation to
    // drain_buffer().
    //
    // Deliberately don't reload head_ here. It may have moved far into future
    // generations because ticket reservation happens before slot publication.
    return drain_buffer(buffer_index, ticket + 1, std::forward<F>(function));
  }

  /// Drain a not-yet-claimed range from the physical buffer containing the
  /// newest reserved ticket.
  ///
  /// Returns true if at least one element was claimed.
  ///
  /// The supplied function may execute concurrently with functions from other
  /// merge_with()/drain() calls.
  template <typename F>
  [[nodiscard]]
  auto drain(F&& function) const noexcept -> bool {
    const size_t head = head_.load(std::memory_order_relaxed);

    if (head == 0) {
      return false;
    }

    const size_t ticket = head - 1;

    const size_t index = ticket & capacity_mask_;

    const size_t buffer_index = index >> buffer_capacity_shift_;

    return drain_buffer(buffer_index, head, std::forward<F>(function));
  }

private:
  /// Attempt to CAS-claim one contiguous range from one physical buffer.
  ///
  /// reserved_head is an exclusive global ticket bound:
  ///
  ///     [0, reserved_head)
  ///
  /// contains all tickets known to have been reserved by producers.
  ///
  /// A single claim never crosses a buffer generation boundary. This is
  /// important because head_ counts reservations rather than publications and
  /// may therefore run arbitrarily far ahead of the slots which are currently
  /// usable.
  template <typename F>
  [[nodiscard]]
  auto drain_buffer(size_t buffer_index, size_t reserved_head, F&& function) const noexcept -> bool {
    metadata_type& meta = metadata_buffers_[buffer_index];

    size_t first = meta.claimed.load(std::memory_order_relaxed);

    for (;;) {
      // first is a monotonic cursor local to this physical buffer.
      //
      // For example, with buffer_capacity == 8:
      //
      //   first ==  3 -> generation 0, offset 3
      //   first == 11 -> generation 1, offset 3
      //   first == 19 -> generation 2, offset 3
      const size_t generation = first >> buffer_capacity_shift_;

      const size_t offset = first & buffer_capacity_mask_;

      // Global ticket corresponding to offset zero of this physical buffer in
      // this generation.
      //
      // generation * total_capacity
      //   + buffer_index * buffer_capacity
      const size_t global_buffer_begin = (generation << capacity_shift_) + (buffer_index << buffer_capacity_shift_);

      // There are no reserved tickets beyond our current claim cursor.
      if (reserved_head <= global_buffer_begin + offset) {
        return false;
      }

      // Determine how much of this generation has been reserved.
      //
      // Cap it at buffer_capacity_ so a CAS claim can never cross into the
      // following generation.
      size_t reserved_in_generation = reserved_head - global_buffer_begin;

      if (reserved_in_generation > buffer_capacity_) {
        reserved_in_generation = buffer_capacity_;
      }

      const size_t target = (generation << buffer_capacity_shift_) + reserved_in_generation;

      if (target <= first) {
        return false;
      }

      // Atomically assign [first, target) to this caller.
      //
      // claimed itself doesn't protect T. slot.turn provides the publication
      // and reuse synchronization for the actual values.
      if (meta.claimed.compare_exchange_weak(first, target, std::memory_order_relaxed, std::memory_order_relaxed)) {
        const size_t physical_buffer_begin = buffer_index << buffer_capacity_shift_;

        const size_t physical_begin = physical_buffer_begin + offset;

        const size_t physical_end = physical_buffer_begin + reserved_in_generation;

        const size_t ready_turn = 2 * generation + 1;

        const size_t free_turn = ready_turn + 1;

        // We exclusively own this logical range now.
        //
        // Some producer may only have RESERVED one of these tickets without
        // having PUBLISHED its value yet. iterator::operator*() therefore
        // still waits on slot.turn, just as it did in the mutex version.
        function(
            iterator{
                slots_ + physical_begin,
                ready_turn,
            },
            iterator{
                slots_ + physical_end,
                ready_turn,
            });

        // Release the physical slots for the following generation.
        for (size_t i = physical_begin; i < physical_end; ++i) {
          slots_[i].turn.store(free_turn, std::memory_order_release);
        }

        return true;
      }

      // compare_exchange_weak writes the current value of meta.claimed into
      // `first` when it fails, so recompute generation/offset/target and try
      // again.
    }
  }

  size_t buffer_count_{1};
  size_t buffer_capacity_{1};
  size_t capacity_{1};

  size_t buffer_capacity_mask_{0};
  size_t capacity_mask_{0};

  size_t buffer_capacity_shift_{0};
  size_t capacity_shift_{0};

  alignas(std::hardware_destructive_interference_size) mutable std::atomic<size_t> head_{0};

  metadata_type* metadata_buffers_{nullptr};
  slot_type* slots_{nullptr};
};

} // namespace alc

#endif // ALCAMI_INCLUDE_APPEND_LOG_H
