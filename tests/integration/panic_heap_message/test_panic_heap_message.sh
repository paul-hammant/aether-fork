#!/bin/sh
# Issue #2340 regression: an uncaught panic whose message was built at run
# time (an interpolation, a heap local) printed the AetherString header's
# magic bytes instead of the text. codegen passes the AetherString* so a
# catch can adopt it; the no-frame fallback %s-printed it as a C string.
# The fallback now unwraps through aether_string_data.
#
# Each fixture panics outside any try/catch; the reason line must carry the
# exact text, for a literal (which always worked) and both heap shapes.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

PREFIX="aether: panic outside any try/catch or actor: "
fail=0

check() {
    name="$1"
    expected="$2"
    bin="$TMPDIR/$name"
    if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/$name.ae" -o "$bin" >/dev/null 2>"$TMPDIR/$name.build"; then
        echo "  [FAIL] $name: ae build exited non-zero"
        head -10 "$TMPDIR/$name.build"
        fail=1
        return
    fi
    if AETHER_STACK_TRACE=0 "$bin" >/dev/null 2>"$TMPDIR/$name.err"; then
        echo "  [FAIL] $name: returned 0, should have aborted"
        fail=1
        return
    fi
    if ! grep -qxF "$PREFIX$expected" "$TMPDIR/$name.err"; then
        echo "  [FAIL] $name: expected reason line '$PREFIX$expected', got:"
        head -5 "$TMPDIR/$name.err" | od -c | head -8
        fail=1
    fi
}

check literal    "plain literal message"
check inline     "test: 5 is outside a slice of 4"
check heap_local "heap local 42"

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] uncaught panic prints heap-built message (issue #2340)"
