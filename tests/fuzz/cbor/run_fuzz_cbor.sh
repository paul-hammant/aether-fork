#!/bin/sh
set -e

# Run the coverage-guided fuzz harness for std.cbor.
#
# Builds the harness with:
#   clang -fsanitize=fuzzer,address,undefined -g -O1 against $(ae cflags)
# and runs it for FUZZ_SECONDS (default 60) on a scratch copy of the corpus.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

FUZZ_SECONDS="${FUZZ_SECONDS:-60}"
CORPUS_DIR="$SCRIPT_DIR/corpus"
DICT_FILE="$SCRIPT_DIR/cbor.dict"

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

WORK_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

echo "==> Building fuzz_cbor..."
# Compile fuzz_cbor.ae to an object file with libFuzzer coverage and ASan/UBSan instrumentation
AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -g -O1" \
    "$AE" build "$SCRIPT_DIR/fuzz_cbor.ae" --with=extern --emit=obj -o "$WORK_DIR/fuzz_cbor_ae.o"

# Fetch cflags from ae
AE_CFLAGS="$("$AE" cflags)"

# Compile fuzz_cbor.c and link the harness binary with libFuzzer, ASan, UBSan
# shellcheck disable=SC2086
"$CLANG" -fsanitize=fuzzer,address,undefined -g -O1 \
    "$SCRIPT_DIR/fuzz_cbor.c" "$WORK_DIR/fuzz_cbor_ae.o" \
    $AE_CFLAGS \
    -o "$WORK_DIR/fuzz_cbor"

# Verify that binary data with NUL bytes arrives whole:
# Check that the 2-byte input 41 00 decodes to CBOR_BYTES of length 1.
echo "==> Verifying binary input arrival (NUL byte preservation)..."
cat << 'EOF' > "$WORK_DIR/test_nul_harness.c"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern int32_t aether_fuzz_check_nul(const void* data, int32_t len);

int main(void) {
    const uint8_t input[] = { 0x41, 0x00 };
    int32_t ok = aether_fuzz_check_nul(input, sizeof(input));
    if (ok != 1) {
        fprintf(stderr, "Error: '41 00' did not decode to CBOR_BYTES of length 1! (truncated arrival)\n");
        return 1;
    }
    // Also confirm that if truncated to 1 byte, it fails as truncated
    const uint8_t truncated[] = { 0x41 };
    int32_t trunc_ok = aether_fuzz_check_nul(truncated, sizeof(truncated));
    if (trunc_ok != 0) {
        fprintf(stderr, "Error: truncated '41' unexpectedly passed!\n");
        return 1;
    }
    return 0;
}
EOF
# shellcheck disable=SC2086
"$CLANG" -fsanitize=address,undefined -g -O1 \
    "$WORK_DIR/test_nul_harness.c" "$WORK_DIR/fuzz_cbor_ae.o" \
    $AE_CFLAGS -o "$WORK_DIR/test_nul_harness"
"$WORK_DIR/test_nul_harness"
echo "  [PASS] 2-byte input '41 00' handled correctly."

# Prepare scratch corpus copy
SCRATCH_CORPUS="$WORK_DIR/corpus"
mkdir -p "$SCRATCH_CORPUS"
if [ -d "$CORPUS_DIR" ]; then
    cp -r "$CORPUS_DIR"/* "$SCRATCH_CORPUS/" 2>/dev/null || true
fi

echo "==> Running fuzz_cbor for ${FUZZ_SECONDS}s..."
cd "$WORK_DIR"
set +e
./fuzz_cbor \
    -dict="$DICT_FILE" \
    -max_len=65536 \
    -rss_limit_mb=2048 \
    -max_total_time="$FUZZ_SECONDS" \
    -print_final_stats=1 \
    "$SCRATCH_CORPUS"
EXIT_CODE=$?
set -e

if [ $EXIT_CODE -ne 0 ]; then
    echo "Fuzzer failed with exit code $EXIT_CODE" >&2
    repro=$(find "$WORK_DIR" -name "crash-*" -o -name "leak-*" -o -name "timeout-*" 2>/dev/null | head -n 1)
    if [ -n "$repro" ]; then
        echo "Reproducer found at: $repro" >&2
    fi
    exit $EXIT_CODE
fi

echo "==> Fuzzing completed cleanly."
