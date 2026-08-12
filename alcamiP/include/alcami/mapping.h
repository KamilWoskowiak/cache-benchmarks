/// \file
///
/// Concepts for specifying mappings from keys to values.

#ifndef ALCAMI_INCLUDE_MAPPING_H
#define ALCAMI_INCLUDE_MAPPING_H

#include <concepts>
#include <functional>
#include <optional>
#include <system_error>


namespace alc {

/// \addtogroup cache
/// \{

/// Specifies a type that can be used as the key in a cache.
///
/// Cache implementations expect that this type is cheap to copy.
template <typename T>
concept searchable = std::regular<T> && std::regular_invocable<std::hash<T>, T>;

/// Specifies a type that can be used as the value in a cache.
template <typename T>
concept storable = std::semiregular<T>;

/// Specifies a callable type that associates some `alc::storable` data with a `alc::searchable` via an out-parameter.
/// This tells a cache implementation how to load data into the cache.
///
/// Returns a `std::error_code` if an error occurs, or no value otherwise. The contents of the error code are
/// communicated back to the user in the case of an error. Cache implementations may expect that invoking values of this
/// type will never throw.
template <typename F, typename K, typename V>
concept mapping = searchable<K> && storable<V> && std::regular_invocable<F, K, V&> &&
                  std::convertible_to<std::invoke_result_t<F, K, V&>, std::optional<std::error_code>>;

/// \}

} // namespace alc


#endif // ALCAMI_INCLUDE_MAPPING_H
