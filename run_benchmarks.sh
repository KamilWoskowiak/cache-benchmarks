#!/usr/bin/env bash
set -euo pipefail

# Run from the benchmark directory that contains:
#   ./alcami
#   ./tbb
#   ./moka
#   ./quic
#
# Output folder:
#   ./results_uniform_zipfian_sleep_spin_<timestamp>/
#
# Output file:
#   results.csv
#
# Edit the arrays and constants below to change what gets benchmarked.

OPS_LIST=(100000)
THREADS_LIST=(8)
CACHE_SIZES=(10000)
TOTAL_KEYS_LIST=(10000 10125 10250 10500 11000 11250 11500 12500 15000 30000 50000)

# Main distributions:
#   u = uniform
#   z = zipfian
DISTRIBUTIONS=(z)

# Delay modes:
#   sleep = mapping_sleep_us > 0, mapping_spin_us = 0
#   spin  = mapping_sleep_us = 0, mapping_spin_us > 0
DELAY_MODES=(sleep)

# Used only for Zipfian. Uniform runs use THETA_PLACEHOLDER.
THETAS=(0.95)
THETA_PLACEHOLDER=0.95

# Sleep duration inside mapping on cache miss, in microseconds.
SLEEP_US_LIST=(0 1 10 100)

# Spin duration inside mapping on cache miss, in microseconds.
SPIN_US_LIST=(1 10 100)

# Smoke-test parameters. These run before the full benchmark.
TEST_OPS=1000
TEST_THREADS=1
TEST_CACHE_SIZE=100
TEST_TOTAL_KEYS=100
TEST_SLEEP_US=1
TEST_SPIN_US=1

# CPU pinning for every benchmark process.
PIN_CORES="0-7"
TASKSET_BIN="taskset"

ALCAMI_BIN="./alcami/build/tools/benchmark"
TBB_BIN="./tbb/build/cache_bench_tbb"
MOKA_DIR="./moka"
QUIC_DIR="./quic"

RESULTS_DIR="./results_uniform_zipfian_sleep_spin_$(date +%Y%m%d_%H%M%S)"
RESULTS_CSV="$RESULTS_DIR/results.csv"

CSV_HEADER="program,ops_per_thread,threads,cache_size,total_keys,distribution,lambda,theta,warmup,mapping_sleep_us,mapping_spin_us,secs,mops,ns_per_op,warmup_misses"

PROGRAMS=(alcami tbb moka quic)

distribution_name() {
  local distribution="$1"

  case "$distribution" in
  u)
    echo "uniform"
    ;;
  z)
    echo "zipfian"
    ;;
  *)
    echo "unknown_distribution"
    ;;
  esac
}

theta_cases_count() {
  local count=0

  for distribution in "${DISTRIBUTIONS[@]}"; do
    if [[ "$distribution" == "z" ]]; then
      count=$((count + ${#THETAS[@]}))
    else
      count=$((count + 1))
    fi
  done

  echo "$count"
}

delay_cases_count() {
  local count=0

  for delay_mode in "${DELAY_MODES[@]}"; do
    case "$delay_mode" in
    sleep)
      count=$((count + ${#SLEEP_US_LIST[@]}))
      ;;
    spin)
      count=$((count + ${#SPIN_US_LIST[@]}))
      ;;
    *)
      echo "unknown delay mode: $delay_mode" >&2
      exit 1
      ;;
    esac
  done

  echo "$count"
}

DISTRIBUTION_THETA_CASES="$(theta_cases_count)"
DELAY_CASES="$(delay_cases_count)"

TOTAL_RUNS=$((\
  ${#PROGRAMS[@]} * \
  ${#OPS_LIST[@]} * \
  ${#CACHE_SIZES[@]} * \
  ${#TOTAL_KEYS_LIST[@]} * \
  ${#THREADS_LIST[@]} * \
  DISTRIBUTION_THETA_CASES * \
  DELAY_CASES))

DONE_RUNS=0

percent_done() {
  awk -v done="$DONE_RUNS" -v total="$TOTAL_RUNS" 'BEGIN { printf "%.2f", (done / total) * 100 }'
}

log_progress() {
  local label="$1"
  echo "[$(percent_done)%] $label"
}

require_path() {
  local path="$1"
  local kind="$2"

  if [[ "$kind" == "file" && ! -x "$path" ]]; then
    echo "Missing executable: $path"
    exit 1
  fi

  if [[ "$kind" == "dir" && ! -d "$path" ]]; then
    echo "Missing directory: $path"
    exit 1
  fi
}

require_command() {
  local command_name="$1"

  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing command: $command_name"
    exit 1
  fi
}

run_pinned() {
  "$TASKSET_BIN" -c "$PIN_CORES" "$@"
}

append_normalized_csv_line() {
  local program_name="$1"
  local raw_output="$2"
  local theta="$3"
  local csv_file="$4"

  awk -v program_name="$program_name" -v theta="$theta" -F',' '
    BEGIN { OFS = "," }

    NF == 14 {
      print program_name,$0
      next
    }

    NF == 13 {
      print program_name,$1,$2,$3,$4,$5,$6,theta,$7,$8,$9,$10,$11,$12,$13
      next
    }

    {
      print "unexpected CSV column count " NF ": " $0 > "/dev/stderr"
      exit 1
    }
  ' <<<"$raw_output" >>"$csv_file"
}

run_alcami_raw() {
  local ops="$1"
  local threads="$2"
  local cache_size="$3"
  local total_keys="$4"
  local distribution="$5"
  local theta="$6"
  local mapping_sleep_us="$7"
  local mapping_spin_us="$8"

  run_pinned "$ALCAMI_BIN" \
    --ops "$ops" \
    --threads "$threads" \
    --cache_size "$cache_size" \
    --total_keys "$total_keys" \
    --distribution "$distribution" \
    --lambda "0.01" \
    --theta "$theta" \
    --mapping_sleep_us "$mapping_sleep_us" \
    --mapping_spin_us "$mapping_spin_us" \
    --labeled=false
}

run_tbb_raw() {
  local ops="$1"
  local threads="$2"
  local cache_size="$3"
  local total_keys="$4"
  local distribution="$5"
  local theta="$6"
  local mapping_sleep_us="$7"
  local mapping_spin_us="$8"

  run_pinned "$TBB_BIN" \
    --ops "$ops" \
    --threads "$threads" \
    --cache_size "$cache_size" \
    --total_keys "$total_keys" \
    --distribution "$distribution" \
    --lambda "0.01" \
    --theta "$theta" \
    --mapping_sleep_us "$mapping_sleep_us" \
    --mapping_spin_us "$mapping_spin_us" \
    --labeled=false
}

run_moka_raw() {
  local ops="$1"
  local threads="$2"
  local cache_size="$3"
  local total_keys="$4"
  local distribution="$5"
  local theta="$6"
  local mapping_sleep_us="$7"
  local mapping_spin_us="$8"

  (
    cd "$MOKA_DIR"
    run_pinned cargo run --release --quiet -- \
      --ops "$ops" \
      --threads "$threads" \
      --cache_size "$cache_size" \
      --total_keys "$total_keys" \
      --distribution "$distribution" \
      --lambda "0.01" \
      --theta "$theta" \
      --mapping_sleep_us "$mapping_sleep_us" \
      --mapping_spin_us "$mapping_spin_us" \
      --labeled false
  )
}

run_quic_raw() {
  local ops="$1"
  local threads="$2"
  local cache_size="$3"
  local total_keys="$4"
  local distribution="$5"
  local theta="$6"
  local mapping_sleep_us="$7"
  local mapping_spin_us="$8"

  (
    cd "$QUIC_DIR"
    run_pinned cargo run --release --quiet -- \
      --ops "$ops" \
      --threads "$threads" \
      --cache_size "$cache_size" \
      --total_keys "$total_keys" \
      --distribution "$distribution" \
      --lambda "0.01" \
      --theta "$theta" \
      --mapping_sleep_us "$mapping_sleep_us" \
      --mapping_spin_us "$mapping_spin_us" \
      --labeled false
  )
}

run_program_raw() {
  local program_name="$1"
  local ops="$2"
  local threads="$3"
  local cache_size="$4"
  local total_keys="$5"
  local distribution="$6"
  local theta="$7"
  local mapping_sleep_us="$8"
  local mapping_spin_us="$9"

  case "$program_name" in
  alcami)
    run_alcami_raw "$ops" "$threads" "$cache_size" "$total_keys" "$distribution" "$theta" "$mapping_sleep_us" "$mapping_spin_us"
    ;;
  tbb)
    run_tbb_raw "$ops" "$threads" "$cache_size" "$total_keys" "$distribution" "$theta" "$mapping_sleep_us" "$mapping_spin_us"
    ;;
  moka)
    run_moka_raw "$ops" "$threads" "$cache_size" "$total_keys" "$distribution" "$theta" "$mapping_sleep_us" "$mapping_spin_us"
    ;;
  quic)
    run_quic_raw "$ops" "$threads" "$cache_size" "$total_keys" "$distribution" "$theta" "$mapping_sleep_us" "$mapping_spin_us"
    ;;
  *)
    echo "unknown program: $program_name"
    exit 1
    ;;
  esac
}

delay_values_for_mode() {
  local delay_mode="$1"

  case "$delay_mode" in
  sleep)
    printf '%s\n' "${SLEEP_US_LIST[@]}"
    ;;
  spin)
    printf '%s\n' "${SPIN_US_LIST[@]}"
    ;;
  *)
    echo "unknown delay mode: $delay_mode" >&2
    exit 1
    ;;
  esac
}

mapping_values_for_delay() {
  local delay_mode="$1"
  local delay_us="$2"

  case "$delay_mode" in
  sleep)
    echo "$delay_us 0"
    ;;
  spin)
    echo "0 $delay_us"
    ;;
  *)
    echo "unknown delay mode: $delay_mode" >&2
    exit 1
    ;;
  esac
}

run_preflight_one() {
  local program_name="$1"
  local distribution="$2"
  local theta="$3"
  local delay_mode="$4"

  local dist_name
  dist_name="$(distribution_name "$distribution")"

  local test_delay_us
  case "$delay_mode" in
  sleep)
    test_delay_us="$TEST_SLEEP_US"
    ;;
  spin)
    test_delay_us="$TEST_SPIN_US"
    ;;
  *)
    echo "unknown delay mode: $delay_mode" >&2
    exit 1
    ;;
  esac

  local mapping_sleep_us
  local mapping_spin_us
  read -r mapping_sleep_us mapping_spin_us < <(mapping_values_for_delay "$delay_mode" "$test_delay_us")

  echo "Testing $program_name distribution=$dist_name theta=$theta delay_mode=$delay_mode delay_us=$test_delay_us..."

  run_program_raw \
    "$program_name" \
    "$TEST_OPS" \
    "$TEST_THREADS" \
    "$TEST_CACHE_SIZE" \
    "$TEST_TOTAL_KEYS" \
    "$distribution" \
    "$theta" \
    "$mapping_sleep_us" \
    "$mapping_spin_us" >/dev/null
}

run_preflight_tests() {
  echo "=== Preflight test runs ==="

  for program_name in "${PROGRAMS[@]}"; do
    for distribution in "${DISTRIBUTIONS[@]}"; do
      for delay_mode in "${DELAY_MODES[@]}"; do
        if [[ "$distribution" == "z" ]]; then
          for theta in "${THETAS[@]}"; do
            run_preflight_one "$program_name" "$distribution" "$theta" "$delay_mode"
          done
        else
          run_preflight_one "$program_name" "$distribution" "$THETA_PLACEHOLDER" "$delay_mode"
        fi
      done
    done
  done

  echo "=== All preflight tests passed ==="
}

run_suite_for_distribution_and_delay() {
  local program_name="$1"
  local distribution="$2"
  local theta="$3"
  local delay_mode="$4"

  local dist_name
  dist_name="$(distribution_name "$distribution")"

  echo "=== Running $program_name $dist_name $delay_mode suite ==="

  for ops in "${OPS_LIST[@]}"; do
    for cache_size in "${CACHE_SIZES[@]}"; do
      for total_keys in "${TOTAL_KEYS_LIST[@]}"; do
        for threads in "${THREADS_LIST[@]}"; do
          for delay_us in $(delay_values_for_mode "$delay_mode"); do
            local mapping_sleep_us
            local mapping_spin_us
            read -r mapping_sleep_us mapping_spin_us < <(mapping_values_for_delay "$delay_mode" "$delay_us")

            log_progress "$program_name distribution=$dist_name theta=$theta delay_mode=$delay_mode delay_us=$delay_us ops=$ops threads=$threads cache_size=$cache_size total_keys=$total_keys"

            raw_output="$(
              run_program_raw \
                "$program_name" \
                "$ops" \
                "$threads" \
                "$cache_size" \
                "$total_keys" \
                "$distribution" \
                "$theta" \
                "$mapping_sleep_us" \
                "$mapping_spin_us"
            )"

            append_normalized_csv_line "$program_name" "$raw_output" "$theta" "$RESULTS_CSV"

            DONE_RUNS=$((DONE_RUNS + 1))
            sleep 5
          done
        done
      done
    done
  done
}

run_suite() {
  local program_name="$1"

  for distribution in "${DISTRIBUTIONS[@]}"; do
    for delay_mode in "${DELAY_MODES[@]}"; do
      if [[ "$distribution" == "z" ]]; then
        for theta in "${THETAS[@]}"; do
          run_suite_for_distribution_and_delay "$program_name" "$distribution" "$theta" "$delay_mode"
        done
      else
        run_suite_for_distribution_and_delay "$program_name" "$distribution" "$THETA_PLACEHOLDER" "$delay_mode"
      fi
    done
  done
}

echo "=== Checking inputs ==="
require_command "$TASKSET_BIN"
require_path "$ALCAMI_BIN" file
require_path "$TBB_BIN" file
require_path "$MOKA_DIR" dir
require_path "$QUIC_DIR" dir

mkdir -p "$RESULTS_DIR"
echo "$CSV_HEADER" >"$RESULTS_CSV"

echo "=== Results directory ==="
echo "$RESULTS_DIR"
echo "=== Results CSV ==="
echo "$RESULTS_CSV"
echo "=== CPU pinning ==="
echo "PIN_CORES=$PIN_CORES"
echo "THREADS_LIST=${THREADS_LIST[*]}"

run_preflight_tests

echo "=== Starting main benchmark ==="
echo "Total benchmark runs: $TOTAL_RUNS"

for program_name in "${PROGRAMS[@]}"; do
  run_suite "$program_name"
done

echo "=== Done ==="
echo "Generated file:"
echo "  $RESULTS_CSV"
