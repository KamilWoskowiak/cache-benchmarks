///
/// \file
///
/// Provides configurable parameters which can be passed to the cache manager on construction.

#ifndef ALCAMI_INCLUDE_OPTIONS_H
#define ALCAMI_INCLUDE_OPTIONS_H

#include <cstddef>

namespace alc {

struct cache_options {
  /// Specifies the number of slots per append log buffer
  size_t append_log_capacity{64};

  /// Specifies the number of buffers
  size_t append_log_buffer_count{8};

  /// The number of shards the cache will have, 2^shard_power
  size_t shard_power{0};

  /// The maximal number of evictions that can happen at any one eviction cycle.
  size_t evictions_per_cycle{8};
};

} // namespace alc

#endif // ALCAMI_INCLUDE_OPTIONS_H
