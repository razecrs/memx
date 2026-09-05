#!/bin/sh
# Median ns/operation per allocation pattern, for MemX and any allocators
# supplied as LD_PRELOAD shared objects.
#
#   tools/compare_allocators.sh BUILD_DIR [NAME=/path/to/lib.so ...]
#
# Every allocator is measured through the same system benchmark binary. A
# sample is accepted only when the child exits successfully and emits one
# complete, internally consistent result record.
set -eu

build=${1:-build-current}
if [ "$#" -gt 0 ]; then
    shift
fi

bench="$build/memx_pattern_bench_system"
shim="$build/libmemx_malloc_shim.so"
[ -x "$bench" ] || { echo "missing $bench; build the benchmarks first" >&2; exit 1; }
[ -f "$shim" ] || { echo "missing $shim; build the benchmarks first" >&2; exit 1; }

live=${MEMX_LIVE:-50000}
rounds=${MEMX_ROUNDS:-3}
samples=${MEMX_SAMPLES:-9}
patterns=${MEMX_PATTERNS:-"mixed small large lifo fifo churn realloc"}

case "$live" in ''|*[!0-9]*) echo "MEMX_LIVE must be a positive integer" >&2; exit 2;; esac
case "$rounds" in ''|*[!0-9]*) echo "MEMX_ROUNDS must be a positive integer" >&2; exit 2;; esac
case "$samples" in ''|*[!0-9]*) echo "MEMX_SAMPLES must be a positive integer" >&2; exit 2;; esac
[ "$live" -ge 1 ] || { echo "MEMX_LIVE must be a positive integer" >&2; exit 2; }
[ "$rounds" -ge 1 ] || { echo "MEMX_ROUNDS must be a positive integer" >&2; exit 2; }
[ "$samples" -ge 1 ] || { echo "MEMX_SAMPLES must be a positive integer" >&2; exit 2; }

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/memx-compare.XXXXXX") || exit 1
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

median_file() {
    sort -n "$1" | awk -v n="$samples" 'BEGIN { count = 0 }
        { values[++count] = $1 }
        END {
            if (count != n) exit 1
            if (n % 2) printf "%.1f", values[(n + 1) / 2]
            else printf "%.1f", (values[n / 2] + values[n / 2 + 1]) / 2
        }'
}

die_sample() {
    echo "benchmark sample failed for allocator '$1', pattern '$2': $3" >&2
    return 1
}

# Run one allocator/pattern pair. The expected operations/checksum file is
# shared between all calls, so an allocator cannot silently change the work
# while still contributing a timing to the table.
run_one() {
    preload=$1
    pattern=$2
    [ "$preload" = none ] && preload=
    values="$tmp_dir/$pattern.$(printf '%s' "$preload" | cksum | cut -d' ' -f1)"
    expected="$tmp_dir/$pattern.expected"
    : >"$values"

    for _ in 1 2; do
        if ! env LD_PRELOAD="$preload" "$bench" "$pattern" "$live" "$rounds" \
            >"$tmp_dir/warmup.out" 2>"$tmp_dir/warmup.err"; then
            die_sample "$preload" "$pattern" "warmup exited nonzero"
            return 1
        fi
        if [ -s "$tmp_dir/warmup.err" ]; then
            die_sample "$preload" "$pattern" "loader or benchmark diagnostics during warmup"
            return 1
        fi
    done

    i=0
    while [ "$i" -lt "$samples" ]; do
        output="$tmp_dir/sample.out"
        error="$tmp_dir/sample.err"
        if ! env LD_PRELOAD="$preload" "$bench" "$pattern" "$live" "$rounds" \
            >"$output" 2>"$error"; then
            die_sample "$preload" "$pattern" "measured run exited nonzero"
            return 1
        fi
        if [ -s "$error" ]; then
            die_sample "$preload" "$pattern" "loader or benchmark diagnostics"
            return 1
        fi
        if ! parsed=$(awk -v pat="$pattern" -v lv="$live" -v rd="$rounds" '
            BEGIN { good = 0 }
            NF == 7 && $1 == "pattern=" pat && $2 == "live=" lv && $3 == "rounds=" rd {
                if (split($4, a, "=") != 2 || split($5, b, "=") != 2 ||
                    split($6, c, "=") != 2 || split($7, d, "=") != 2) next
                if (a[1] == "operations" && b[1] == "elapsed_ns" &&
                    c[1] == "ns_per_operation" && d[1] == "checksum" &&
                    a[2] ~ /^[1-9][0-9]*$/ && b[2] ~ /^[1-9][0-9]*$/ &&
                    c[2] ~ /^[0-9]+[.]?[0-9]*$/ && d[2] ~ /^[0-9]+$/) {
                    print a[2], d[2], c[2]
                    good++
                }
            }
            END { if (NR != 1 || good != 1) exit 1 }
        ' "$output"); then
            die_sample "$preload" "$pattern" "expected one valid result record"
            return 1
        fi
        set -- $parsed
        if [ ! -f "$expected" ]; then
            printf '%s %s\n' "$1" "$2" >"$expected"
        elif [ "$(cat "$expected")" != "$1 $2" ]; then
            die_sample "$preload" "$pattern" "operations/checksum changed"
            return 1
        fi
        printf '%s\n' "$3" >>"$values"
        i=$((i + 1))
    done
    if ! median_file "$values"; then
        die_sample "$preload" "$pattern" "invalid sample count"
        return 1
    fi
}

names="memx"
libs="$shim"
for spec in "$@"; do
    case "$spec" in
        *=*) name=${spec%%=*}; lib=${spec#*=};;
        *) echo "allocator must be NAME=/path/to/lib.so: $spec" >&2; exit 2;;
    esac
    case "$name" in ''|*[!A-Za-z0-9_-]*) echo "invalid allocator name: $name" >&2; exit 2;; esac
    [ -f "$lib" ] || { echo "missing preload library for $name: $lib" >&2; exit 2; }
    case "$lib" in *[[:space:]]*) echo "preload path contains whitespace: $lib" >&2; exit 2;; esac
    names="$names ${spec%%=*}"
    libs="$libs ${spec#*=}"
done
names="$names glibc"
libs="$libs none"

printf '%-9s' pattern
for n in $names; do printf ' %9s' "$n"; done
printf '\n'
for pattern in $patterns; do
    case "$pattern" in
        mixed|small|large|lifo|fifo|churn|realloc) : ;;
        *) echo "invalid pattern: $pattern" >&2; exit 2;;
    esac
    printf '%-9s' "$pattern"
    for lib in $libs; do
        result="$tmp_dir/result"
        if ! run_one "$lib" "$pattern" >"$result"; then
            exit 1
        fi
        printf ' %9s' "$(cat "$result")"
    done
    printf '\n'
done
