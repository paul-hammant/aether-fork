#!/bin/sh
# #2335: a dependency's own [dependencies] resolve for the project using it.
#
# Only the consumer's manifest was read, so `app -> pkga -> pkgb` failed with
# "unresolved import 'beta'" from app while `ae run` inside pkga worked: the
# consumer had to declare pkgb itself and patch it to a path inside pkga's
# checkout, which breaks the moment pkga moves its submodule.
#
# Asserts:
#   - pkga's [dependencies] are followed, and its [patch] path resolves
#     against pkga's own root (lib-path lists ../pkga and ../pkgb)
#   - the transitive patch is announced, naming the package that patched it
#   - the consumer's [patch] wins over a dependency's for the same package
#   - a diamond (two packages patching one to the same directory) resolves
#     once, with no error
#   - a cycle terminates
#   - one package resolved to two different directories is an error naming
#     both and who required each, and the command fails
#   - a transitive dependency missing from the cache names who required it
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] transitive_deps: ae not built"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT

# A private, empty package cache: nothing here may come from ~/.aether.
export HOME="$T/home"
mkdir -p "$HOME"
if command -v cygpath >/dev/null 2>&1; then
    USERPROFILE="$(cygpath -m "$HOME")"
else
    USERPROFILE="$HOME"
fi
export USERPROFILE

fail() {
    echo "  [FAIL] transitive_deps: $1"
    [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/    /' | head -12
    exit 1
}

pkg() {  # pkg DIR NAME -- a package exporting its root
    mkdir -p "$T/$1"
    printf '[package]\nname = "%s"\nmodules = "."\n' "$2" > "$T/$1/aether.toml"
}

pkg pkgb pkgb
mkdir -p "$T/pkgb/beta"
printf 'exports (twice)\ntwice(x: int) -> int { return x * 2 }\n' > "$T/pkgb/beta/module.ae"

# A second pkgb with different behaviour, to tell which one was built.
pkg pkgb_alt pkgb
mkdir -p "$T/pkgb_alt/beta"
printf 'exports (twice)\ntwice(x: int) -> int { return x * 10 }\n' > "$T/pkgb_alt/beta/module.ae"

pkg pkga pkga
mkdir -p "$T/pkga/alpha"
cat >> "$T/pkga/aether.toml" <<'TOML'

[dependencies]
"example.com/pkgb" = "0.1.0"

[patch]
"example.com/pkgb" = "../pkgb"
TOML
printf 'import beta\nexports (quad)\nquad(x: int) -> int { return beta.twice(beta.twice(x)) }\n' \
    > "$T/pkga/alpha/module.ae"

mkdir -p "$T/app"
printf 'import alpha\nmain() { println("quad(3) = ${alpha.quad(3)}") }\n' > "$T/app/main.ae"
app_manifest() {
    printf '[package]\nname = "app"\n\n[dependencies]\n%s\n\n[patch]\n%s\n' "$1" "$2" \
        > "$T/app/aether.toml"
}

# --- 1. the issue's three-package chain ---------------------------------
app_manifest '"example.com/pkga" = "0.1.0"' '"example.com/pkga" = "../pkga"'
cd "$T/app"
OUT=$("$AE" run main.ae 2>&1) || fail "app -> pkga -> pkgb did not run" "$OUT"
echo "$OUT" | grep -qx "quad(3) = 12" || fail "wrong output for the chain" "$OUT"
echo "$OUT" | grep -qF "Overriding example.com/pkgb -> ../pkgb (patched by example.com/pkga)" \
    || fail "the transitive patch was not announced with its patcher" "$OUT"

LP=$("$AE" lib-path 2>/dev/null) || fail "lib-path failed" "$LP"
echo "$LP" | grep -qx "../pkga" || fail "lib-path is missing ../pkga" "$LP"
echo "$LP" | grep -qx "../pkgb" || fail "lib-path is missing ../pkgb (pkga's patch not resolved against pkga's root)" "$LP"

# --- 2. the consumer's [patch] wins -------------------------------------
app_manifest '"example.com/pkga" = "0.1.0"' '"example.com/pkga" = "../pkga"
"example.com/pkgb" = "../pkgb_alt"'
OUT=$("$AE" run main.ae 2>&1) || fail "consumer patch of a transitive dep did not run" "$OUT"
echo "$OUT" | grep -qx "quad(3) = 300" || fail "the consumer's [patch] did not win" "$OUT"
echo "$OUT" | grep -q "two different directories" && fail "consumer patch reported as a conflict" "$OUT"

# --- 3. a diamond resolves once -----------------------------------------
pkg pkgc pkgc
mkdir -p "$T/pkgc/gamma"
cat >> "$T/pkgc/aether.toml" <<'TOML'

[dependencies]
"example.com/pkgb" = "0.1.0"

[patch]
"example.com/pkgb" = "../pkgb"
TOML
printf 'import beta\nexports (dbl)\ndbl(x: int) -> int { return beta.twice(x) }\n' > "$T/pkgc/gamma/module.ae"
printf 'import alpha\nimport gamma\nmain() { println("${alpha.quad(1)} ${gamma.dbl(1)}") }\n' > "$T/app/both.ae"
app_manifest '"example.com/pkga" = "0.1.0"
"example.com/pkgc" = "0.1.0"' '"example.com/pkga" = "../pkga"
"example.com/pkgc" = "../pkgc"'
OUT=$("$AE" run both.ae 2>&1) || fail "a diamond did not run" "$OUT"
echo "$OUT" | grep -qx "4 2" || fail "wrong output for the diamond" "$OUT"
[ "$(echo "$OUT" | grep -c "Overriding example.com/pkgb")" = 1 ] \
    || fail "the shared dependency was resolved more than once" "$OUT"

# --- 4. a cycle terminates ----------------------------------------------
cat >> "$T/pkgb/aether.toml" <<'TOML'

[dependencies]
"example.com/pkga" = "0.1.0"

[patch]
"example.com/pkga" = "../pkga"
TOML
app_manifest '"example.com/pkga" = "0.1.0"' '"example.com/pkga" = "../pkga"'
OUT=$("$AE" run main.ae 2>&1) || fail "a cycle did not run" "$OUT"
echo "$OUT" | grep -qx "quad(3) = 12" || fail "wrong output for the cycle" "$OUT"
printf '[package]\nname = "pkgb"\nmodules = "."\n' > "$T/pkgb/aether.toml"

# --- 5. two directories for one package is an error ---------------------
sed 's|"../pkgb"|"../pkgb_alt"|' "$T/pkgc/aether.toml" > "$T/pkgc/aether.toml.new"
mv "$T/pkgc/aether.toml.new" "$T/pkgc/aether.toml"
app_manifest '"example.com/pkga" = "0.1.0"
"example.com/pkgc" = "0.1.0"' '"example.com/pkga" = "../pkga"
"example.com/pkgc" = "../pkgc"'
if OUT=$("$AE" run both.ae 2>&1); then fail "a conflicting graph ran" "$OUT"; fi
echo "$OUT" | grep -qF "Error: dependency 'example.com/pkgb' resolves to two different directories" \
    || fail "the conflict was not reported" "$OUT"
echo "$OUT" | grep -qF "../pkgb (required by example.com/pkga)" || fail "conflict did not name the first path" "$OUT"
echo "$OUT" | grep -qF "../pkgb_alt (required by example.com/pkgc)" || fail "conflict did not name the second path" "$OUT"
if OUT=$("$AE" lib-path 2>&1); then fail "lib-path succeeded on a conflicting graph" "$OUT"; fi

# --- 6. a transitive dependency missing from the cache -------------------
printf '[package]\nname = "pkgc"\nmodules = "."\n\n[dependencies]\n"example.com/nowhere" = "1.0.0"\n' \
    > "$T/pkgc/aether.toml"
OUT=$("$AE" lib-path 2>&1 || true)
echo "$OUT" | grep -qF "Error: dependency 'example.com/nowhere' (required by example.com/pkgc) is not installed" \
    || fail "a missing transitive dependency did not name who required it" "$OUT"

echo "  [PASS] transitive dependency resolution (issue #2335)"
