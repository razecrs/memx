#!/bin/sh
# Regression checks for tools/compare_allocators.sh.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build-current"}
tool="$root/tools/compare_allocators.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/memx-compare-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

[ -x "$tool" ] || { echo "comparison tool is not executable" >&2; exit 1; }
[ -x "$build/memx_pattern_bench_system" ] || {
    echo "build benchmark targets before running this test" >&2
    exit 1
}
[ -f "$build/libmemx_malloc_shim.so" ] || {
    echo "build the MemX preload shim before running this test" >&2
    exit 1
}

success=$(MEMX_SAMPLES=3 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=lifo \
    "$tool" "$build")
case "$success" in
    *pattern*|*lifo*) : ;;
    *) echo "valid comparison did not produce a result table" >&2; exit 1;;
esac

if MEMX_SAMPLES=1 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=not-a-pattern \
    "$tool" "$build" >"$tmp/invalid.out" 2>"$tmp/invalid.err"
then
    echo "invalid benchmark unexpectedly succeeded" >&2
    exit 1
fi
if MEMX_SAMPLES=1 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=../escape \
    "$tool" "$build" > /dev/null 2>"$tmp/path.err"
then
    echo "path-like pattern unexpectedly accepted" >&2
    exit 1
fi
if grep -q '0\.0' "$tmp/invalid.out"; then
    echo "failed benchmark was converted into a zero ranking" >&2
    exit 1
fi

even=$(MEMX_SAMPLES=2 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=lifo \
    "$tool" "$build")
case "$even" in *lifo*) : ;; *) echo "even sample median failed" >&2; exit 1;; esac

if MEMX_SAMPLES=1 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=lifo \
    "$tool" "$build" missing=/no/such/library.so > /dev/null 2>"$tmp/lib.err"
then
    echo "missing preload library unexpectedly accepted" >&2
    exit 1
fi
printf '%s\n' 'not an ELF shared object' >"$tmp/text.so"
if MEMX_SAMPLES=1 MEMX_LIVE=4 MEMX_ROUNDS=1 MEMX_PATTERNS=lifo \
    "$tool" "$build" text="$tmp/text.so" > /dev/null 2>"$tmp/text.err"
then
    echo "existing non-ELF preload unexpectedly accepted" >&2
    exit 1
fi

# Build a tiny valid preload and result fixture so parser failures are tested
# independently of the real allocator. The fixture deliberately supports bad
# records and cross-allocator checksum drift.
cc=${CC:-cc}
"$cc" -shared -fPIC -x c -o "$tmp/libfixture.so" - <<'EOF'
int memx_compare_fixture_symbol;
EOF
cp "$tmp/libfixture.so" "$tmp/libmemx_malloc_shim.so"
"$cc" -x c -O0 -o "$tmp/memx_pattern_bench_system" - <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *mode = getenv("FIXTURE_MODE");
    unsigned long checksum = (getenv("LD_PRELOAD") && *getenv("LD_PRELOAD")) ? 1 : 2;
    unsigned long operations = (getenv("LD_PRELOAD") && *getenv("LD_PRELOAD")) ? 4 : 5;
    if (!mode || strcmp(mode, "good") == 0) { operations = 4; checksum = 1; }
    if (mode && (strcmp(mode, "drift") == 0 || strcmp(mode, "measured-failure") == 0)) {
        FILE *state = fopen(getenv("FIXTURE_STATE"), "r+");
        unsigned long count = 0;
        if (state) { (void)fscanf(state, "%lu", &count); rewind(state); }
        else state = fopen(getenv("FIXTURE_STATE"), "w+");
        if (!state) return 2;
        fprintf(state, "%lu", count + 1); fclose(state);
        if (strcmp(mode, "measured-failure") == 0) {
            if (count >= 2) return 7;
            mode = "good";
        }
        operations = count % 2U ? 4 : 5; checksum = count % 2U ? 1 : 2;
    }
    if (!mode || strcmp(mode, "good") == 0 || strcmp(mode, "mismatch") == 0 || strcmp(mode, "drift") == 0) {
        printf("pattern=%s live=%s rounds=%s operations=%lu elapsed_ns=10 ns_per_operation=2.500 checksum=%lu\n", argv[1], argv[2], argv[3], operations, checksum);
        return 0;
    }
    if (strcmp(mode, "duplicate") == 0) {
        printf("pattern=%s live=%s rounds=%s operations=4 elapsed_ns=10 ns_per_operation=2.500 checksum=1\n", argv[1], argv[2], argv[3]);
        printf("pattern=%s live=%s rounds=%s operations=4 elapsed_ns=11 ns_per_operation=2.750 checksum=1\n", argv[1], argv[2], argv[3]);
        return 0;
    }
    if (strcmp(mode, "malformed") == 0) {
        puts("not a benchmark result");
        return 0;
    }
    if (strcmp(mode, "extra-equals") == 0) {
        printf("pattern=%s live=%s rounds=%s operations=4=garbage elapsed_ns=10 ns_per_operation=2.500 checksum=1\n", argv[1], argv[2], argv[3]);
        return 0;
    }
    return 0;
}
EOF

fixture_run() {
    mode=$1
    if FIXTURE_MODE="$mode" MEMX_SAMPLES=1 MEMX_LIVE=4 MEMX_ROUNDS=1 \
        MEMX_PATTERNS=lifo "$tool" "$tmp" >"$tmp/$mode.out" 2>"$tmp/$mode.err"; then
        echo "fixture mode unexpectedly succeeded: $mode" >&2
        exit 1
    fi
    if grep -q '0\.0' "$tmp/$mode.out"; then
        echo "fixture failure became a zero ranking: $mode" >&2
        exit 1
    fi
}

fixture_run duplicate
fixture_run malformed
fixture_run extra-equals
fixture_run mismatch
FIXTURE_STATE="$tmp/measured-state"
export FIXTURE_STATE
fixture_run measured-failure
if FIXTURE_MODE=drift FIXTURE_STATE="$tmp/state" MEMX_SAMPLES=2 MEMX_LIVE=4 MEMX_ROUNDS=1 \
    MEMX_PATTERNS=lifo "$tool" "$tmp" >"$tmp/drift.out" 2>"$tmp/drift.err"; then
    echo "cross-sample drift unexpectedly accepted" >&2
    exit 1
fi

echo "compare_allocators regression tests passed"
