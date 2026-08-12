/// \file
///
/// Concepts for specifying value summaries.

#ifndef ALCAMI_INCLUDE_SUMMARY_H
#define ALCAMI_INCLUDE_SUMMARY_H

#include <concepts>
#include <type_traits>

#include "mapping.h"


namespace alc::detail {
struct blank_summary_impl {};
} // namespace alc::detail


namespace alc {

/// Includes the information about key (i.e., the metadata) used by an eviction policy.
///
/// \ingroup policy
template <typename T>
concept summary = std::semiregular<T>;


/// A summary containing no information.
///
/// \ingroup policy
/// \hideinitializer
inline constexpr detail::blank_summary_impl blank_summary{};


/// Type of `alc::blank_summary`.
///
/// \ingroup policy
using blank_summary_t = detail::blank_summary_impl;


/// Specifies a callable object that returns a summary of a key's relevant information (i.e., the key's metadata).
///
/// The summarizer is used by a cache to convert a key into the type expected by an eviction policy.
///
/// \ingroup cache
template <typename F, typename K, typename S>
concept summarizer = std::semiregular<F> && searchable<K> && summary<S> && std::regular_invocable<F, K> &&
                     std::convertible_to<std::invoke_result_t<F, K>, S>;


static_assert(summary<blank_summary_t>);

} // namespace alc


#endif // ALCAMI_INCLUDE_SUMMARY_H
