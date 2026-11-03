#!/bin/sh
set -e

# Prove that the fuzz harness can find bugs by planting mutants in std/msgpack/module.ae
# and running the fuzzer against them until crash (or timeout FUZZ_SECONDS=300).
#
# M1: the depth check (depth > MAX_DEPTH) shifted by one or removed.
# M2: the declared-length check against the remaining input (len > cursor.remaining(c)) removed.
# M3: the element-count check against the remaining input removed (count_fits helper).
# M4: the trailing-bytes refusal in unpack removed.
# M5: one fixed-width read's EOF check made one byte short (e.g. u64 checking for 7 bytes).
#
# Does NOT use git stash and does NOT edit the real std/msgpack/module.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
export AETHER_HOME="$ROOT"
AE="$ROOT/build/ae"
CLANG="${CLANG:-clang}"
FUZZ_SECONDS="${FUZZ_SECONDS:-300}"

TMPDIR_BASE="$(mktemp -d)"
cleanup() {
    rm -rf "$TMPDIR_BASE"
}
trap cleanup EXIT INT TERM

# ASan keeps a stack trace for every allocation and never frees one. The
# decoder recurses once per level, so with the default 30 frames nearly
# every nesting shape is a new trace, and RSS climbed past -rss_limit_mb
# (2 GB) in 30 minutes with no leak in the program. 8 frames still name the
# allocating function and stay flat. A caller's ASAN_OPTIONS wins.
export ASAN_OPTIONS="${ASAN_OPTIONS:-malloc_context_size=8}"

DICT_FILE="$SCRIPT_DIR/msgpack.dict"
ORIGINAL_MSGPACK="$ROOT/std/msgpack/module.ae"

run_mutant() {
    name="$1"
    mutant_ae="$2"
    echo "=========================================================="
    echo "Running Mutant $name..."
    echo "=========================================================="

    M_DIR="$TMPDIR_BASE/$name"
    mkdir -p "$M_DIR/std/msgpack"
    mkdir -p "$M_DIR/corpus"
    cp -r "$SCRIPT_DIR/corpus"/* "$M_DIR/corpus/" 2>/dev/null || true

    # Copy the mutant module into place in a local module tree
    cp "$mutant_ae" "$M_DIR/std/msgpack/module.ae"

    # Also copy harness and compile against this mutant module
    cp "$SCRIPT_DIR/fuzz_msgpack.ae" "$M_DIR/fuzz_msgpack.ae"
    cp "$SCRIPT_DIR/fuzz_msgpack.c" "$M_DIR/fuzz_msgpack.c"

    echo "==> Building $name harness..."
    (cd "$M_DIR" && AE_CC="$CLANG -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1" \
        "$AE" build "fuzz_msgpack.ae" --with=extern --emit=obj -o "fuzz_msgpack_ae.o")

    AE_CFLAGS="$("$AE" cflags)"
    # shellcheck disable=SC2086
    (cd "$M_DIR" && "$CLANG" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 \
        "fuzz_msgpack.c" "fuzz_msgpack_ae.o" \
        $AE_CFLAGS \
        -o "fuzz_msgpack")

    echo "==> Fuzzing $name (up to ${FUZZ_SECONDS}s)..."
    cd "$M_DIR"
    set +e
    start_time=$(date +%s)
    ./fuzz_msgpack \
        -dict="$DICT_FILE" \
        -max_len=65536 \
        -rss_limit_mb=2048 \
        -max_total_time="$FUZZ_SECONDS" \
        "$M_DIR/corpus" > "$M_DIR/fuzz.log" 2>&1
    rc=$?
    end_time=$(date +%s)
    duration=$((end_time - start_time))
    set -e

    if [ $rc -eq 0 ]; then
        echo "FAIL: $name was NOT found within ${FUZZ_SECONDS}s!" >&2
        tail -n 30 "$M_DIR/fuzz.log" >&2
        return 1
    fi

    echo "SUCCESS: $name found in ${duration}s (exit code $rc)!"
    echo "--- Crash report summary for $name ---"
    grep -E "ERROR: (AddressSanitizer|libFuzzer|UndefinedBehaviorSanitizer)|SCARINESS|SUMMARY:|SEGV|stack-overflow|out-of-memory|deadly signal|Test unit written" "$M_DIR/fuzz.log" | head -n 10 || head -n 25 "$M_DIR/fuzz.log"
    echo "--------------------------------------"
}

make_mutant() {
    out="$1"; target="$2"; replacement="$3"
    TARGET="$target" REPLACEMENT="$replacement" python3 - "$ORIGINAL_MSGPACK" "$out" << 'PYEOF'
import os, sys
src = open(sys.argv[1]).read()
t, r = os.environ["TARGET"], os.environ["REPLACEMENT"]
assert src.count(t) == 1, f"mutant target not found exactly once ({src.count(t)}): {t}"
open(sys.argv[2], "w").write(src.replace(t, r))
PYEOF
}

# M1: the depth check (depth > MAX_DEPTH) shifted by one or removed.
M1_FILE="$TMPDIR_BASE/m1.ae"
make_mutant "$M1_FILE" \
    '    if depth > MAX_DEPTH {
        return null, "nesting deeper than 256"
    }' \
    '    // M1: depth check removed'

# M2: the declared-length check against the remaining input (len > cursor.remaining(c)) removed.
M2_FILE="$TMPDIR_BASE/m2.ae"
make_mutant "$M2_FILE" \
    '    if len > (cursor.remaining(c) as long) {
        return what
    }' \
    '    // M2: remaining check removed'

# M3: the element-count check against the remaining input removed (count <= remaining / min_size helper).
M3_FILE="$TMPDIR_BASE/m3.ae"
make_mutant "$M3_FILE" \
    'fn count_fits(c: ptr, count: long, min_size: long) -> bool {
    return count <= (cursor.remaining(c) as long) / min_size
}' \
    'fn count_fits(c: ptr, count: long, min_size: long) -> bool {
    return true
}'

# M4: the trailing-bytes refusal in unpack removed.
M4_FILE="$TMPDIR_BASE/m4.ae"
make_mutant "$M4_FILE" \
    '    if cursor.remaining(c) > 0 {
        free(val)
        return null, "trailing bytes after value"
    }' \
    '    // M4: trailing bytes check removed'

# M5: one fixed-width read'\''s EOF check made one byte short (for example, u64 checking for 7 bytes).
M5_FILE="$TMPDIR_BASE/m5.ae"
make_mutant "$M5_FILE" \
    '    if m == 0xcf {
        if cursor.remaining(c) < 8 { return null, "EOF reading u64" }' \
    '    if m == 0xcf {
        if cursor.remaining(c) < 7 { return null, "EOF reading u64" }'

echo "Testing mutants with prove_mutants.sh..."
# `prove_mutants.sh M4 M5` runs only those.
for m in ${*:-M1 M2 M3 M4 M5}; do
    eval "run_mutant $m \"\$${m}_FILE\""
done

echo "All mutants proven successfully!"
