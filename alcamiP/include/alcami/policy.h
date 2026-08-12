/// \file
///
/// Concepts and types for specifying eviction policies.

#ifndef ALCAMI_INCLUDE_POLICY_H
#define ALCAMI_INCLUDE_POLICY_H

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "summary.h"


namespace alc {


/// \defgroup policy Eviction Policies
///
/// \copydoc policy.h
///
/// Eviction policies defined by `alc::policy` objects. An `alc::policy` consists of an `alc::prioritizer`, an
/// `alc::combiner`, and an identity element. Conceptually, a policy operates on an _access list_. That is, a sequence
/// of keys ordered by their _logical access time_. The position of an element in the sequence is called the element's
/// _logical timestamp_. A policy assigns a priority to each key based on the logical timestamps of accesses to that
/// key. A **lower** priority indicates that a key is a better candidate for eviction. A cache implementation aims to
/// keep items with a **higher** priority in the cache.
///
/// ### Defining Policies
///
/// To assign a priority to each key, a policy makes use of three abstractions: a _prioritizer_, a _combiner_, and an
/// _identity element_. The prioritizer assigns a priority to an individual access -- that is, assigns a priority based
/// on a key's metadata and a timestamp. This metadata is called the key's _summary_, and is provided by a cache's
/// _summarizer_. The combiner combines the priorities of adjacent accesses to the same key into a new priority. The
/// identity element is a priority that acts as the identity element of the combiner. These three components are used
/// together to assign a single priority to each key.
///
/// Mathematically, the combiner is a [monoid](https://en.wikipedia.org/wiki/Monoid). In other words, the combiner must
/// be an associative binary operator with an identity element. However, the combiner does **not** need to be
/// commutative.
///
/// Let \f$ k \f$ be a key and let \f$ t_i \f$ denote the timestamps at which \f$ k \f$ was accessed for
/// \f$ 1 \leq i \leq n \f$. Let \f$ \sigma \f$ be the summarizer, \f$ \pi \f$ be the prioritizer,
/// \f$ \oplus \f$ be the combiner, and \f$ e \f$ be the identity element.
///
/// If we let \f$ s = \sigma(k) \f$, then the policy will assign \f$ k \f$ a priority of
/// \f[
///   e \oplus \pi(t_1, s) \oplus \pi(t_2, s) \oplus \pi(t_3, s) \oplus \ldots \oplus \pi(t_n, s).
/// \f]
///
/// ### Note on Consistency Models
///
/// The policy operates on an access sequence, which is a sequence of accesses going through a cache. However, the
/// [consistency model](https://en.wikipedia.org/wiki/Consistency_model) that the cache uses to produce this access
/// sequence depends on the cache's implementation. This means that a policy should ideally not depend on any particular
/// consistency model.
///
/// \{


/// Represents a logical timestamp.
using timestamp_t = std::int64_t;


/// Specifies a type that can be used as a priority.
///
/// For performance reasons the type should be cheap to copy.
template <typename T>
concept priority = std::regular<T> && std::totally_ordered<T>;


/// Specifies a callable type that assigns a priority based on a logical timestamp and a summary.
template <typename F, typename S>
concept prioritizer = summary<S> && std::regular_invocable<F, alc::timestamp_t, S> &&
                      priority<std::invoke_result_t<F, alc::timestamp_t, S>>;


/// The type returned by the given prioritizer.
template <summary S, prioritizer<S> Prio>
using prioritizer_priority_t = std::invoke_result_t<Prio, alc::timestamp_t, S>;


/// Specifies a callable type that combines two priorities into a new priority.
///
/// Mathematically, this is modeled by a monoid for a totally ordered set. That means that the
/// function must be associative and that the function must have a suitable identity element.
template <typename F, typename P>
concept combiner = std::regular_invocable<F, P, P> && std::same_as<std::invoke_result_t<F, P, P>, P> && priority<P>;


/// Represents a caching policy.
///
/// Determines the best item to evict from a cache to make room for a new item. The priority and summary
/// types are assumed to be cheap-to-copy, while the prioritizer and combiner types are assumed to
/// be cheap to call. All function objects must be thread- and exception-safe.
///
/// Items with a lower priority should be evicted first. A cache implementation tries to keep the highest priority items
/// in the cache.
///
/// \tparam S Type of the policy's summary
/// \tparam Prio Type of the policy's prioritizer
/// \tparam Comb Type of the policy's combiner
///
/// \see See `predefined.h` or `alc::policies` for predefined policies.
template <summary S, prioritizer<S> Prio, combiner<prioritizer_priority_t<S, Prio>> Comb>
struct policy {
  /// Type of the summary.
  using summary_type = S;

  /// Type of the prioritizer.
  using prioritizer_type = Prio;

  /// Type of the priority.
  using priority_type = prioritizer_priority_t<S, Prio>;

  /// Type of the combiner.
  using combiner_type = Comb;

  /// Creates a new policy. In most cases, use `alc::make_policy` instead of invoking the constructor directly. (This
  /// allows for template argument deduction.) You only need to construct a new policy if defining a custom policy.
  /// Predefined policies are available in `predefined.h` under the `alc::policies` namespace.
  ///
  /// \see `alc::make_policy`
  /// \see `alc::policies`
  constexpr policy(priority_type id, prioritizer_type prio, combiner_type comb) noexcept
      : identity{id}, prioritizer{prio}, combiner{comb} {}

  /// Identity element for the prioritizer.
  priority_type identity;

  /// Function object that assigns priorities to accesses.
  prioritizer_type prioritizer;

  /// Function object that combines two priorities.
  combiner_type combiner;
};


namespace detail {
/// Implementation of `alc::make_policy`. Should not be used directly.
template <summary S>
struct make_policy_impl {
  template <prioritizer<S> Prio, combiner<prioritizer_priority_t<S, Prio>> Comb>
  [[nodiscard]] auto operator()(prioritizer_priority_t<S, Prio> id, Prio prio = {}, Comb comb = {}) const {
    return policy<S, Prio, Comb>{id, prio, comb};
  }
};
} // namespace detail


/// Function object used to create a `policy`.
///
/// \tparam S Type of the metadata used by the policy's prioritizer
///
/// ---
///
/// **Call Signature**
///
/// ```cpp
/// template < prioritizer<S> Prio,
///            combiner<prioritizer_priority_t<S, Prio>> Comb >
/// [[nodiscard]]
/// auto make_policy<S>( prioritizer_priority_t<S, Prio> id,
///                      Prio prio = {}, Comb comb = {} )
///    -> policy < S, Prio, Comb >;
/// ```
///
/// \param id Identity element to use
/// \param prio Prioritizer to use
/// \param comb Combiner to use
///
/// \hideinitializer
///
template <summary S>
inline const detail::make_policy_impl<S> make_policy{};


/// Concept abstracting `alc::policy`. Template arguments of this concept should normally be `alc::policy`.
template <typename T>
concept eviction_policy =
    requires(T t) {
      typename T::summary_type;
      typename T::prioritizer_type;
      typename T::priority_type;
      typename T::combiner_type;
      { t.identity } -> std::convertible_to<typename T::priority_type>;
      { t.prioritizer } -> std::convertible_to<typename T::prioritizer_type>;
      { t.combiner } -> std::convertible_to<typename T::combiner_type>;
    } &&                                                                   //
    summary<typename T::summary_type> &&                                   //
    prioritizer<typename T::prioritizer_type, typename T::summary_type> && //
    combiner<typename T::combiner_type, typename T::priority_type> &&      //
    std::same_as<typename T::priority_type,
                 prioritizer_priority_t<typename T::summary_type, typename T::prioritizer_type>>;


/// \}


} // namespace alc


#endif // ALCAMI_INCLUDE_POLICY_H
