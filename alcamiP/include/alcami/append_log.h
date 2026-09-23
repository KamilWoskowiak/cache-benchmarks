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
#include <tbb/spin_mutex.h>
#include <utility>

namespace alc {

template <typename T>
struct slot {
  T value;
  std::atomic<size_t> turn{0};
};

struct alignas(std::hardware_destructive_interference_size) metadata {
  size_t tail{0};
  tbb::spin_mutex mutex; // TODO i want to get rid of this
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

    metadata_buffers_ = new metadata[buffer_count_];
    slots_ = new slot<T>[capacity_];
  }

  append_log(const append_log&) = delete;
  auto operator=(const append_log&) -> append_log& = delete;

  append_log(append_log&& other) noexcept
      : buffer_count_{other.buffer_count_}, buffer_capacity_{other.buffer_capacity_}, capacity_{other.capacity_},
        buffer_capacity_mask_{other.buffer_capacity_mask_}, capacity_mask_{other.capacity_mask_},
        buffer_capacity_shift_{other.buffer_capacity_shift_}, capacity_shift_{other.capacity_shift_},
        head_{other.head_.load(std::memory_order_relaxed)},
        metadata_buffers_{std::exchange(other.metadata_buffers_, nullptr)},
        slots_{std::exchange(other.slots_, nullptr)} {
    other.head_.store(0, std::memory_order_relaxed);
  }

  auto operator=(append_log&& other) noexcept -> append_log& {
    if (this == &other) {
      return *this;
    }

    delete[] metadata_buffers_;
    delete[] slots_;

    buffer_count_ = other.buffer_count_;
    buffer_capacity_ = other.buffer_capacity_;
    capacity_ = other.capacity_;

    buffer_capacity_mask_ = other.buffer_capacity_mask_;
    capacity_mask_ = other.capacity_mask_;

    buffer_capacity_shift_ = other.buffer_capacity_shift_;
    capacity_shift_ = other.capacity_shift_;

    head_.store(other.head_.load(std::memory_order_relaxed), std::memory_order_relaxed);

    metadata_buffers_ = std::exchange(other.metadata_buffers_, nullptr);

    slots_ = std::exchange(other.slots_, nullptr);

    other.head_.store(0, std::memory_order_relaxed);

    return *this;
  }

  ~append_log() {
    delete[] metadata_buffers_;
    delete[] slots_;
  }

  /// True if there was a drain
  template <typename F>
  [[nodiscard]]
  auto merge_with(T value, F&& function) const noexcept -> bool {
    size_t ticket = head_.fetch_add(1, std::memory_order_relaxed);

    size_t turnhalf = ticket >> capacity_shift_;
    size_t index = ticket & capacity_mask_;
    size_t turn = 2 * turnhalf;

    slot_type& slot = slots_[index];

    while (slot.turn.load(std::memory_order_acquire) != turn) {
      // yield
    }

    slot.value = std::move(value);
    slot.turn.store(turn + 1, std::memory_order_release);

    size_t offset = index & buffer_capacity_mask_;

    if (offset != buffer_capacity_mask_) {
      return false;
    }

    size_t buffer_index = index >> buffer_capacity_shift_;

    metadata_type& meta = metadata_buffers_[buffer_index];
    meta.mutex.lock();

    size_t begin = buffer_index << buffer_capacity_shift_;
    size_t tail = begin + meta.tail;
    size_t end = begin + buffer_capacity_;

    function(iterator{slots_ + tail, turn + 1}, iterator{slots_ + end, turn + 1});

    for (size_t i = tail; i < end; ++i) {
      slots_[i].turn.store(turn + 2, std::memory_order_release);
    }

    meta.tail = 0;
    meta.mutex.unlock();

    return true;
  }

  /// True if there was some element which was drained
  template <typename F>
  [[nodiscard]]
  auto drain(F&& function) const noexcept -> bool {
    size_t head = head_.load(std::memory_order_relaxed);

    if (head == 0) {
      return false;
    }

    size_t ticket = head - 1;

    size_t turnhalf = ticket >> capacity_shift_;
    size_t index = ticket & capacity_mask_;
    size_t turn = 2 * turnhalf;

    size_t buffer_index = index >> buffer_capacity_shift_;

    size_t offset = index & buffer_capacity_mask_;

    size_t begin = buffer_index << buffer_capacity_shift_;

    size_t end = begin + offset + 1;

    metadata_type& meta = metadata_buffers_[buffer_index];

    meta.mutex.lock();

    if (slots_[index].turn.load(std::memory_order_acquire) > turn + 1) {
      meta.mutex.unlock();
      return false;
    }

    size_t tail = begin + meta.tail;

    if (tail >= end) {
      meta.mutex.unlock();
      return false;
    }

    function(iterator{slots_ + tail, turn + 1}, iterator{slots_ + end, turn + 1});

    for (size_t i = tail; i < end; ++i) {
      slots_[i].turn.store(turn + 2, std::memory_order_release);
    }

    meta.tail = offset + 1;

    meta.mutex.unlock();
    return true;
  }

private:
  size_t buffer_count_{1};
  size_t buffer_capacity_{1};
  size_t capacity_{1};

  size_t buffer_capacity_mask_{0};
  size_t capacity_mask_{0};

  size_t buffer_capacity_shift_{0};
  size_t capacity_shift_{0};

  alignas(std::hardware_destructive_interference_size) mutable std::atomic<size_t> head_{0};

  metadata* metadata_buffers_{nullptr};
  slot<T>* slots_{nullptr};
};

} // namespace alc

#endif // ALCAMI_INCLUDE_APPEND_LOG_H
