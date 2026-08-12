/// \file
///
/// Predefined eviction policies.

#ifndef ALCAMI_INCLUDE_PREDEFINED_H
#define ALCAMI_INCLUDE_PREDEFINED_H

#include <algorithm>
#include <limits>

#include "common.h"
#include "policy.h"


/// \copydoc predefined.h
namespace alc::policies {


/// Summary type for `alc::policies::lru`.
///
/// \ingroup predef
using lru_summ_t = blank_summary_t;

/// Summary type for `alc::policies::lfu`.
///
/// \ingroup predef
using lfu_summ_t = blank_summary_t;

/// Summary type for `alc::policies::fifo`.
///
/// \ingroup predef
using fifo_summ_t = blank_summary_t;

/// Summary type for `alc::policies::fqsz`.
///
/// \ingroup predef
using fqsz_summ_t = ssize_t;


namespace detail {

using lru_prio_t = common_priority_t;
inline const lru_prio_t lru_id{std::numeric_limits<lru_prio_t>::min()};
inline auto lru_comb(lru_prio_t p, lru_prio_t q) -> lru_prio_t { return std::max(p, q); }
inline auto lru_prio(timestamp_t t, lru_summ_t) -> lru_prio_t { return t; }


using lfu_prio_t = common_priority_t;
inline const lfu_prio_t lfu_id{0};
inline auto lfu_comb(lfu_prio_t p, lfu_prio_t q) -> lfu_prio_t { return p + q; }
inline auto lfu_prio(timestamp_t, lfu_summ_t) -> lfu_prio_t { return 1; }


using fifo_prio_t = timestamp_t;
inline const auto fifo_id = std::numeric_limits<fifo_prio_t>::max();
inline auto fifo_comb(fifo_prio_t p, fifo_prio_t q) -> fifo_prio_t { return std::min(p, q); }
inline auto fifo_prio(timestamp_t t, fifo_summ_t) -> fifo_prio_t { return t; }


using fqsz_prio_t = common_priority_t;
inline const auto fqsz_id = std::numeric_limits<fqsz_prio_t>::min();
inline auto fqsz_comb(fqsz_prio_t p, fqsz_prio_t q) -> fqsz_prio_t { return std::max(p, q) + 1; }
inline auto fqsz_prio(timestamp_t, fqsz_summ_t s) -> fqsz_prio_t { return 1 - s; }

} // namespace detail


/// \defgroup predef Predefined Policies
///
/// \copydoc predefined.h


/// Eviction policy for least-recently-used.
///
/// \ingroup predef
inline const auto lru = make_policy<lru_summ_t>(detail::lru_id, detail::lru_prio, detail::lru_comb);

/// Eviction policy for least-frequently-used.
///
/// \ingroup predef
inline const auto lfu = make_policy<lfu_summ_t>(detail::lfu_id, detail::lfu_prio, detail::lfu_comb);

/// Eviction policy for first-in-first-out.
///
/// \ingroup predef
inline const auto fifo = make_policy<fifo_summ_t>(detail::fifo_id, detail::fifo_prio, detail::fifo_comb);

/// Eviction policy for frequency-size.
///
/// \ingroup predef
inline const auto fqsz = make_policy<fqsz_summ_t>(detail::fqsz_id, detail::fqsz_prio, detail::fqsz_comb);

/// \}


} // namespace alc::policies


#endif // ALCAMI_INCLUDE_PREDEFINED_H
