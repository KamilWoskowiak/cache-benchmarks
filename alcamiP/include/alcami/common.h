/// \file
///
/// Common default types for ALCaMI concepts.

#ifndef ALCAMI_INCLUDE_COMMON_H
#define ALCAMI_INCLUDE_COMMON_H

#include <cassert>
#include <cstdint>
#include <stdexcept>

#include "policy.h"
#include "summary.h"
#include "utils.h"


namespace alc {

/// A simple priority type.
///
/// \ingroup predef
using common_priority_t = ssize_t;


/// A simple summary type.
///
/// \ingroup predef
class common_summary {
public:
  /// Size type.
  using size_type = std::int64_t;

  /// Cost type.
  using cost_type = std::int64_t;

  /// Creates a summary with a size and cost of one.
  constexpr common_summary() noexcept : size_{1}, cost_{1} {}

  /// Creates a summary with the given size and cost. Both must be non-zero.
  constexpr common_summary(size_type s, cost_type c) : size_{s}, cost_{c} {
    if (s == 0 || c == 0) {
      throw std::invalid_argument{"Size and cost cannot be zero"};
    }
  }

  /// Conversion to a blank summary.
  operator blank_summary_t() const noexcept { return blank_summary; }

  /// Returns the size.
  [[nodiscard]] constexpr auto size() const noexcept -> size_type {
    assert(size_ != 0);
    return size_;
  }

  /// Returns the cost.
  [[nodiscard]] constexpr auto cost() const noexcept -> cost_type {
    assert(cost_ != 0);
    return cost_;
  }

private:
  size_type size_;
  cost_type cost_;
};


/// A summarizer that provides an empty summary.
///
/// \ingroup cache
template <typename K>
inline constexpr auto blank_summarizer = [](const K&) { return blank_summary; };


/// Type of `alc::blank_summarizer`.
///
/// \ingroup cache
template <typename K>
using blank_summarizer_t = std::remove_const_t<decltype(blank_summarizer<K>)>;


static_assert(priority<common_priority_t>);
static_assert(summary<common_summary>);

} // namespace alc


#endif // ALCAMI_INCLUDE_COMMON_H
