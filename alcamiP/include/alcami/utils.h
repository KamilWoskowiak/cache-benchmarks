/// \file
///
/// Miscellaneous utilities.

#ifndef ALCAMI_INCLUDE_UTILS_H
#define ALCAMI_INCLUDE_UTILS_H

#include <concepts>
#include <cstddef>
#include <optional>
#include <system_error>
#include <type_traits>

#include "alcami/mapping.h"


namespace alc {


/// \defgroup misc Miscellaneous
///
/// General definitions and utilities.


/// Signed size type.
///
/// \ingroup misc
using ssize_t = std::ptrdiff_t;


namespace detail {
/// Implementation of `alc::mapping_adapter`. Should not be used directly.
template <searchable K>
struct mapping_adapter_impl {
  template <std::regular_invocable<K> Fn>
  requires storable<std::invoke_result_t<Fn, K>>
  [[nodiscard]] auto operator()(Fn f) const -> mapping<K, std::invoke_result_t<Fn, K>> auto {
    return [f = std::move(f)](K k, std::invoke_result_t<Fn, K>& v) -> std::optional<std::error_code> {
      v = std::invoke(f, k);
      return std::nullopt;
    };
  }
};
} // namespace detail


/// Function object used to turn a function (or function object) that returns a value into its corresponding
/// `alc::mapping`.
///
/// \tparam K Key type
///
/// ---
///
/// **Call Signature**
///
/// ```cpp
/// template <std::regular_invocable<K> Fn>
/// requires storable<std::invoke_result_t<Fn, K>>
/// [[nodiscard]]
/// auto mapping_adapter<K>(Fn f)
///     -> mapping<K, std::invoke_result_t<Fn, K>> auto;
/// ```
///
/// \param f Function (or function object) following the form f(K) -> V for some type V.
///
/// \returns Mapping from K to V.
///
/// \ingroup cache
/// \hideinitializer
///
template <searchable K>
inline const auto mapping_adapter = detail::mapping_adapter_impl<K>{};


} // namespace alc

#endif // ALCAMI_INCLUDE_UTILS_H
