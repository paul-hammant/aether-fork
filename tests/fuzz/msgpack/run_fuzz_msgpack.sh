#!/bin/sh
set -e

# Run the coverage-guided fuzz harness for std.msgpack.
#
# Builds the harness with:
#   clang -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 against $(ae cflags)
# and runs it for FUZZ_SECONDS (default 60) on a copy of the seed corpus.
# Exits non-zero on a crash, leak, timeout or oracle failure, naming the
# reproducer.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
export AETHER_HOME="$ROOT"
AE="$ROOT/build/ae"

FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
CORPUS_DIR="$SCRIPT_DIR/corpus"
DICT_FILE="$SCRIPT_DIR/msgpack.dict"

if [ ! -x "$AE" ]; then
    echo "Error: $AE not built. Run 'make' first." >&2
    exit 1
fi

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "Error: clang ($CLANG) not found." >&2
    exit 1
fi

# Check if the compiler supports -fsanitize=fuzzer
CHECK_TMP="$(mktemp -d)"
check_supported=1
cat << 'EOF' > "$CHECK_TMP/check_fuzzer.c"
#include <stdint.h>
#include <stddef.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { return 0; }
EOF

if ! "$CLANG" -fsanitize=fuzzer "$CHECK_TMP/check_fuzzer.c" -o "$CHECK_TMP/check_fuzzer" >/dev/null 2>&1; then
    check_supported=0
fi
rm -rf "$CHECK_TMP"

if [ "$check_supported" -eq 0 ]; then
    echo "Error: compiler ($CLANG) lacks libFuzzer support (-fsanitize=fuzzer failed to link)." >&2
    echo "Install libclang-rt-<version>-dev (e.g. libclang-rt-18-dev on Ubuntu) or use a clang build with compiler-rt built-in." >&2
    exit 1
fi

# Everything goes under build/fuzz/msgpack and stays there: the harness binary
# (to reproduce with), the working corpus (a copy of the seeds, which grows
# run to run) and any crash-*, leak-*, timeout-* or oom-* reproducer.
OUT="${FUZZ_OUT:-$ROOT/build/fuzz/msgpack}"
mkdir -p "$OUT/corpus"

echo "==> Building $OUT/fuzz_msgpack..."
# The Aether side carries the coverage and sanitizer instrumentation: std.msgpack
# is pure Aether, so it is compiled into this object. UBSan halts rather than
# printing and carrying on, so undefined behaviour fails the run.
AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1" \
    "$AE" build "$SCRIPT_DIR/fuzz_msgpack.ae" --with=extern --emit=obj -o "$OUT/fuzz_msgpack_ae.o"

AE_CFLAGS="$("$AE" cflags)"

# shellcheck disable=SC2086
"$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
    "$SCRIPT_DIR/fuzz_msgpack.c" "$OUT/fuzz_msgpack_ae.o" \
    $AE_CFLAGS \
    -o "$OUT/fuzz_msgpack"

# The input must reach msgpack.unpack whole. Cut at its first NUL, `a1 00` would
# arrive as `a1` and fail as truncated, and the fuzzer would test almost
# nothing past a zero byte.
echo "==> Checking that input arrives whole (a1 00 is a 1-byte string)..."
cat << 'CEOF' > "$OUT/check_whole.c"
#include <stdint.h>
#include <stdio.h>

extern int32_t aether_fuzz_check_nul(const void* data, int32_t len);

int main(void) {
    const uint8_t input[] = { 0xa1, 0x00 };
    if (aether_fuzz_check_nul(input, 2) != 1) {
        fprintf(stderr, "FAIL: a1 00 did not decode to a 1-byte string: the input was cut\n");
        return 1;
    }
    // And the check can fail: one byte short, it does.
    if (aether_fuzz_check_nul(input, 1) != 0) {
        fprintf(stderr, "FAIL: a1 alone was accepted: the check proves nothing\n");
        return 1;
    }
    return 0;
}
CEOF
# shellcheck disable=SC2086
"$CLANG" -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1 \
    "$OUT/check_whole.c" "$OUT/fuzz_msgpack_ae.o" \
    $AE_CFLAGS -o "$OUT/check_whole"
"$OUT/check_whole"
echo "  [PASS] a1 00 arrives as 2 bytes"

cp "$CORPUS_DIR"/* "$OUT/corpus/"

# ASan keeps a stack trace for every allocation and never frees one. The
# decoder recurses once per level, so with the default 30 frames nearly
# every nesting shape is a new trace, and RSS climbed past -rss_limit_mb
# (2 GB) in 30 minutes with no leak in the program. 8 frames still name the
# allocating function and stay flat. A caller's ASAN_OPTIONS wins.
export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

echo "==> Fuzzing for ${FUZZ_SECONDS}s..."
set +e
"$OUT/fuzz_msgpack" \
    -dict="$DICT_FILE" \
    -max_len=65536 \
    -rss_limit_mb=2048 \
    -timeout=10 \
    -max_total_time="$FUZZ_SECONDS" \
    -artifact_prefix="$OUT/" \
    -print_final_stats=1 \
    "$OUT/corpus"
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -ne 0 ]; then
    echo "FAIL: fuzz_msgpack exited $EXIT_CODE" >&2
    for repro in "$OUT"/crash-* "$OUT"/leak-* "$OUT"/timeout-* "$OUT"/oom-*; do
        [ -f "$repro" ] && echo "  reproducer: $repro  (rerun: $OUT/fuzz_msgpack $repro)" >&2
    done
    exit $EXIT_CODE
fi

echo "==> No crash, leak, timeout or oracle failure in ${FUZZ_SECONDS}s."
