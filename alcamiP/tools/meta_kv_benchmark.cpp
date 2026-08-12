#include <algorithm>
#include <charconv>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include <alcami/cache.h>
#include <alcami/predefined.h>

using raw_key_type = std::uint64_t;
using usecase_type = std::uint32_t;
using score_type = std::int64_t;

static_assert(sizeof(raw_key_type) == 8);
static_assert(std::is_unsigned_v<raw_key_type>);

/*
 * Cache identity + per-request metadata.
 *
 * Only id participates in cache identity.
 * usecase is metadata available to the summarizer.
 */
struct cache_key {
  raw_key_type id{};
  usecase_type usecase{};

  friend auto operator==(const cache_key& lhs, const cache_key& rhs) noexcept -> bool { return lhs.id == rhs.id; }
};

/*
 * Hash only the actual Meta key.
 *
 * usecase intentionally does not participate in identity.
 */
template <>
struct std::hash<cache_key> {
  auto operator()(const cache_key& key) const noexcept -> std::size_t { return std::hash<raw_key_type>{}(key.id); }
};

/*
 * This is the PRIORITY type P.
 *
 * It doubles as the per-entry policy state.
 *
 * No external frequency map is needed.
 */
struct cache_priority {
  alc::timestamp_t last_access{};
  std::uint8_t frequency{};
  std::int8_t usecase_class{};
  score_type score{};

  /*
   * Alcami requires priorities to be std::totally_ordered.
   *
   * score is the primary eviction ordering.
   */
  friend auto operator<=>(const cache_priority& lhs, const cache_priority& rhs) noexcept {

    if (auto c = lhs.score <=> rhs.score; c != 0) {
      return c;
    }

    if (auto c = lhs.last_access <=> rhs.last_access; c != 0) {
      return c;
    }

    if (auto c = lhs.frequency <=> rhs.frequency; c != 0) {
      return c;
    }

    return lhs.usecase_class <=> rhs.usecase_class;
  }

  friend auto operator==(const cache_priority& lhs, const cache_priority& rhs) noexcept -> bool = default;
};

static_assert(std::regular<cache_priority>);
static_assert(std::totally_ordered<cache_priority>);

struct TraceRow {
  std::uint64_t op_time{};
  raw_key_type key{};
  std::string_view op;
  std::uint64_t op_count{};
  usecase_type usecase{};
};

auto next_field(std::string_view& line) -> std::string_view {
  const auto pos = line.find(',');

  if (pos == std::string_view::npos) {
    const auto result = line;
    line = {};
    return result;
  }

  const auto result = line.substr(0, pos);
  line.remove_prefix(pos + 1);

  return result;
}

template <typename T>
auto parse_decimal(std::string_view s, T& value) -> bool {

  const auto* first = s.data();
  const auto* last = s.data() + s.size();

  const auto [ptr, ec] = std::from_chars(first, last, value);

  return ec == std::errc{} && ptr == last;
}

auto parse_hex_key(std::string_view s, raw_key_type& value) -> bool {

  const auto* first = s.data();
  const auto* last = s.data() + s.size();

  const auto [ptr, ec] = std::from_chars(first, last, value, 16);

  return ec == std::errc{} && ptr == last;
}

auto parse_row(std::string_view line, TraceRow& row) -> bool {

  /*
   * CSV:
   *
   * 0  op_time
   * 1  key
   * 2  key_size
   * 3  op
   * 4  op_count
   * 5  size
   * 6  cache_hits
   * 7  ttl
   * 8  usecase
   * 9  sub_usecase
   */

  const auto op_time = next_field(line);

  const auto key = next_field(line);

  next_field(line); // key_size

  const auto op = next_field(line);

  const auto op_count = next_field(line);

  next_field(line); // size
  next_field(line); // cache_hits
  next_field(line); // ttl

  const auto usecase = next_field(line);

  // sub_usecase intentionally ignored.

  if (!parse_decimal(op_time, row.op_time)) {
    return false;
  }

  if (!parse_hex_key(key, row.key)) {
    return false;
  }

  if (!parse_decimal(op_count, row.op_count)) {
    return false;
  }

  if (!parse_decimal(usecase, row.usecase)) {
    return false;
  }

  row.op = op;

  return true;
}

auto is_lookup(std::string_view op) -> bool { return op == "GET" || op == "GET_LEASE"; }

auto main(int argc, char** argv) -> int {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " <trace.csv> <cache_capacity>\n";

    return EXIT_FAILURE;
  }

  const std::string filename = argv[1];

  std::size_t capacity{};

  {
    const std::string_view arg = argv[2];

    const auto* first = arg.data();
    const auto* last = arg.data() + arg.size();

    const auto [ptr, ec] = std::from_chars(first, last, capacity);

    if (ec != std::errc{} || ptr != last || capacity == 0) {

      std::cerr << "invalid cache capacity: " << arg << '\n';

      return EXIT_FAILURE;
    }
  }

  std::ifstream file(filename);

  if (!file) {
    std::cerr << "could not open " << filename << '\n';

    return EXIT_FAILURE;
  }

  /*
   * Express policy bonuses relative to cache capacity.
   *
   * At capacity 500,000:
   *
   * frequency 1:          +0
   * frequency 2:          +250,000
   * frequency 3:          +500,000
   * frequency 4+:       +1,000,000
   *
   * New key from 366387042:
   *                         0 metadata penalty
   *
   * New key from 2777918327:
   *                      -500,000
   *
   * New key from others:
   *                      -250,000
   */
  const auto unit = static_cast<score_type>(capacity);

  /*
   * Map an anonymized usecase ID into a small semantic class.
   *
   * +1 = favorable population
   *  0 = unknown/default
   * -1 = churn-heavy population
   */
  auto classify_usecase = [](usecase_type usecase) -> std::int8_t {
    switch (usecase) {
    case 366387042u:
      return 1;

    case 2777918327u:
      return -1;

    default:
      return 0;
    }
  };

  /*
   * Calculate the actual eviction score from accumulated state.
   */
  auto calculate_score = [unit](alc::timestamp_t timestamp, std::uint8_t frequency,
                                std::int8_t usecase_class) -> score_type {
    /*
     * Recency component.
     */
    const auto recency = static_cast<score_type>(timestamp);

    /*
     * Bounded frequency protection.
     *
     * We intentionally saturate frequency at 4 so an historically
     * hot item cannot accumulate infinite protection.
     */
    score_type frequency_bonus = 0;

    switch (frequency) {
    case 0:
    case 1:
      frequency_bonus = 0;
      break;

    case 2:
      frequency_bonus = unit / 2;
      break;

    case 3:
      frequency_bonus = unit;
      break;

    default:
      frequency_bonus = unit * 2;
      break;
    }

    /*
     * usecase is primarily an ADMISSION PRIOR.
     *
     * If we haven't actually seen reuse yet, population-level
     * information matters strongly.
     *
     * Once this particular key has demonstrated reuse, direct
     * evidence about the key becomes more important than the
     * population it came from.
     */
    score_type metadata_bias = 0;

    if (frequency <= 1) {
      switch (usecase_class) {
      case 1:
        /*
         * 366387042:
         *
         * Better observed reuse characteristics.
         */
        metadata_bias = 0;
        break;

      case -1:
        /*
         * 2777918327:
         *
         * Much larger unique-key population and poorer reuse.
         * Put first accesses into strong probation.
         */
        metadata_bias = -unit;
        break;

      default:
        metadata_bias = -(unit / 2);
        break;
      }
    } else {
      /*
       * Once actual reuse exists, dramatically weaken the
       * population-level prior.
       */
      switch (usecase_class) {
      case 1:
        metadata_bias = 0;
        break;

      case -1:
        metadata_bias = -(unit / 8);
        break;

      default:
        metadata_bias = -(unit / 16);
        break;
      }
    }

    return recency + frequency_bonus + metadata_bias;
  };

  /*
   * ============================
   * PRIORITIZER
   * ============================
   *
   * S = usecase_type
   *
   * prio_f(timestamp, summary) -> cache_priority
   *
   * One invocation represents ONE access.
   */
  auto prio_f = [classify_usecase, calculate_score](alc::timestamp_t timestamp,
                                                    usecase_type usecase) -> cache_priority {
    const auto usecase_class = classify_usecase(usecase);

    constexpr std::uint8_t frequency = 1;

    return cache_priority{
        .last_access = timestamp,
        .frequency = frequency,
        .usecase_class = usecase_class,
        .score = calculate_score(timestamp, frequency, usecase_class),
    };
  };

  /*
   * ============================
   * COMBINER
   * ============================
   *
   * Combines access-history summaries for the SAME cache key.
   *
   * There are no side tables.
   *
   * Frequency, recency, and metadata state are carried entirely
   * by cache_priority.
   */
  auto comb_f = [calculate_score](cache_priority lhs, cache_priority rhs) -> cache_priority {
    /*
     * frequency == 0 represents the monoid identity.
     */
    if (lhs.frequency == 0) {
      return rhs;
    }

    if (rhs.frequency == 0) {
      return lhs;
    }

    /*
     * Saturating frequency:
     *
     * min(lhs + rhs, 4)
     *
     * This is associative.
     */
    const auto frequency = static_cast<std::uint8_t>(
        std::min<unsigned>(static_cast<unsigned>(lhs.frequency) + static_cast<unsigned>(rhs.frequency), 4U));

    /*
     * Latest logical access.
     *
     * max is associative.
     */
    const auto last_access = std::max(lhs.last_access, rhs.last_access);

    /*
     * The trace normally gives a stable usecase for a key.
     *
     * max provides deterministic associative combination if
     * different metadata does appear:
     *
     *     good > neutral > bad
     */
    const auto usecase_class = std::max(lhs.usecase_class, rhs.usecase_class);

    return cache_priority{
        .last_access = last_access,
        .frequency = frequency,
        .usecase_class = usecase_class,
        .score = calculate_score(last_access, frequency, usecase_class),
    };
  };

  /*
   * Identity for comb_f.
   *
   * comb(identity, p) == p
   * comb(p, identity) == p
   */
  const cache_priority identity{
      .last_access = std::numeric_limits<alc::timestamp_t>::lowest(),

      .frequency = 0,

      .usecase_class = 0,

      .score = std::numeric_limits<score_type>::lowest(),
  };

  /*
   * IMPORTANT:
   *
   * make_policy<S>
   *
   * S is the SUMMARY type, which is usecase_type.
   *
   * cache_priority is inferred as P from prio_f's return type.
   */
  auto policy = alc::make_policy<usecase_type>(identity, prio_f, comb_f);

  /*
   * ============================
   * SUMMARIZER
   * ============================
   *
   * sigma(key) -> usecase_type
   *
   * This is how per-request usecase metadata reaches prio_f.
   */
  auto summarizer = [](const cache_key& key) -> usecase_type { return key.usecase; };

  /*
   * Mapping execution means the value was not available in cache.
   */
  std::uint64_t misses = 0;

  auto map_f = alc::mapping_adapter<cache_key>([&](const cache_key& key) {
    ++misses;

    /*
     * Value contents don't matter to this benchmark.
     */
    return key.id;
  });

  auto cache = alc::make_cache<cache_key, raw_key_type>(capacity, map_f, policy, summarizer);

  std::string line;

  /*
   * Skip CSV header.
   */
  if (!std::getline(file, line)) {
    std::cerr << "trace is empty\n";
    return EXIT_FAILURE;
  }

  std::uint64_t accesses = 0;
  std::uint64_t processed_rows = 0;
  std::uint64_t ignored_rows = 0;
  std::uint64_t bad_rows = 0;

  while (std::getline(file, line)) {
    TraceRow row;

    if (!parse_row(line, row)) {
      ++bad_rows;

      if (bad_rows <= 10) {
        std::cerr << "bad row: " << line << '\n';
      }

      continue;
    }

    /*
     * Match the Quick Cache benchmark exactly.
     */
    if (!is_lookup(row.op)) {
      ++ignored_rows;
      continue;
    }

    /*
     * usecase travels with this ACCESS.
     *
     * It does not determine cache identity.
     */
    const cache_key key{
        .id = row.key,
        .usecase = row.usecase,
    };

    /*
     * Replay op_count exactly as we do for Quick Cache.
     */
    for (std::uint64_t i = 0; i < row.op_count; ++i) {

      auto handle = cache.lookup(key);

      (void)handle;

      ++accesses;
    }

    ++processed_rows;

    if (processed_rows % 1'000'000 == 0) {

      const auto hits = accesses - misses;

      const double hit_rate = accesses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(accesses);

      std::cerr << "\rprocessed " << processed_rows << " lookup rows"
                << " | accesses " << accesses << " | hit rate " << hit_rate * 100.0 << "%" << std::flush;
    }
  }

  std::cerr << '\n';

  const std::uint64_t hits = accesses - misses;

  const double hit_rate = accesses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(accesses);

  const double miss_rate = accesses == 0 ? 0.0 : static_cast<double>(misses) / static_cast<double>(accesses);

  std::cout << '\n'
            << "=== Meta KV Cache Benchmark ===\n"
            << "policy:         "
               "recency + bounded frequency + usecase admission\n"
            << "trace:          " << filename << '\n'
            << "capacity:       " << capacity << '\n'
            << '\n'
            << "frequency bonuses:\n"
            << "  first:        0\n"
            << "  second:       " << unit / 2 << '\n'
            << "  third:        " << unit << '\n'
            << "  fourth+:      " << unit * 2 << '\n'
            << '\n'
            << "first-access bias:\n"
            << "  366387042:    0\n"
            << "  2777918327:   " << -unit << '\n'
            << "  other:        " << -(unit / 2) << '\n'
            << '\n'
            << "lookup rows:    " << processed_rows << '\n'
            << "ignored rows:   " << ignored_rows << '\n'
            << "bad rows:       " << bad_rows << '\n'
            << '\n'
            << "accesses:       " << accesses << '\n'
            << "hits:           " << hits << '\n'
            << "misses:         " << misses << '\n'
            << '\n'
            << "hit rate:       " << hit_rate * 100.0 << "%\n"
            << "miss rate:      " << miss_rate * 100.0 << "%\n";

  return EXIT_SUCCESS;
}
