#ifndef ALCAMI_MINI_MAP_H
#define ALCAMI_MINI_MAP_H

/// WIP the idea behind this is to combine priorities as we drain them from the append log. after we are done, we can
/// then take the mutex to update the global priorty values.

#include <cstddef>
#include <memory>
#include <vector>

namespace alc {

template <typename K, typename V>
struct kv {
  K key{};
  V value{};
};

template <typename K, typename V, typename Allocator = std::allocator<kv<K, V>>>
class mini_map {
public:
  using key_type = K;
  using value_type = V;
  using slot_type = kv<K, V>;
  using allocator_type = Allocator;
  using alloc_traits = std::allocator_traits<allocator_type>;

  explicit mini_map(std::size_t n) : slots_(alloc_traits::allocate(alloc_, n)), n_(n), occupied_(n) {
    if (slots_) {
      std::uninitialized_value_construct_n(slots_, n_);
    }
  }

  ~mini_map() {
    if (slots_) {
      std::destroy_n(slots_, n_);
      alloc_traits::deallocate(alloc_, slots_, n_);
    }
  }

  mini_map(const mini_map&) = delete;
  auto operator=(const mini_map&) -> mini_map& = delete;

  mini_map(mini_map&&) = delete;
  auto operator=(mini_map&&) -> mini_map& = delete;

  template <typename F>
  auto try_emplace_l(key_type key, value_type value, F&& f) -> bool {
    const size_t mask = n_ - 1;
    size_t index = key & mask;

    while (occupied_[index]) {
      if (slots_[index].key == key) {
        f(slots_[index].value);
        return true;
      }

      index = (index + 1) & mask;
    }

    slots_[index].key = key;
    slots_[index].value = std::move(value);
    occupied_[index] = true;

    return false;
  }

  class iterator {
  public:
    iterator() = default;

    [[nodiscard]]
    auto operator*() const -> slot_type& {
      return map_->slots_[index_];
    }

    [[nodiscard]]
    auto operator->() const -> slot_type* {
      return &map_->slots_[index_];
    }

    auto operator++() -> iterator& {
      ++index_;
      advance();
      return *this;
    }

    [[nodiscard]]
    auto operator==(const iterator& other) const noexcept -> bool {
      return map_ == other.map_ && index_ == other.index_;
    }

    [[nodiscard]]
    auto operator!=(const iterator& other) const noexcept -> bool {
      return !(*this == other);
    }

  private:
    friend class mini_map;

    iterator(mini_map* map, std::size_t index) noexcept : map_(map), index_(index) { advance(); }

    auto advance() noexcept -> void {
      while (index_ < map_->n_ && !map_->occupied_[index_]) {
        ++index_;
      }
    }

    mini_map* map_{nullptr};
    std::size_t index_{0};
  };

  [[nodiscard]]
  auto begin() noexcept -> iterator {
    return iterator{this, 0};
  }

  [[nodiscard]]
  auto end() noexcept -> iterator {
    return iterator{this, n_};
  }


private:
  [[no_unique_address]]
  allocator_type alloc_{};

  slot_type* slots_{nullptr};
  std::size_t n_{0};
  std::vector<bool> occupied_{};
};

} // namespace alc

#endif // ALCAMI_MINI_MAP_H
