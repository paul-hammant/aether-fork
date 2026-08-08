#!/bin/sh
# Integration test for contrib.htmlsanitizer

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

WORK="$TMPDIR/work"
mkdir -p "$WORK"
ln -s "$ROOT/contrib" "$WORK/contrib"
cp "$SCRIPT_DIR/probe.ae" "$WORK/probe.ae"

cat > "$WORK/aether.toml" <<EOF
[project]
name = "htmlsanitizer_probe"
version = "0.0.0"

[[bin]]
name = "probe"
path = "probe.ae"
EOF

if ! ( cd "$WORK" && "$ROOT/build/ae" build "probe.ae" -o "$TMPDIR/probe" \
        >"$TMPDIR/build.log" 2>&1 ); then
    echo "  [FAIL] contrib_htmlsanitizer: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -30
    exit 1
fi

if [ ! -x "$TMPDIR/probe" ]; then
    echo "  [FAIL] contrib_htmlsanitizer: build produced no binary"
    sed 's/^/    /' "$TMPDIR/build.log" | head -30
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] contrib_htmlsanitizer: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "All htmlsanitizer tests passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] contrib_htmlsanitizer: didn't reach final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

echo "  [PASS] contrib_htmlsanitizer"
