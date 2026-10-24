#!/bin/sh
# Regression: #2975 move buffer into a message. The compiler must REJECT
# use-after-move for straight-line code, if branches, loops, defers,
# borrowed parameters, captured variables, and object fields.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "  [FAIL] buffer_move_reject: $1"; exit 1; }

expect_reject() {
    name="$1"; src="$2"; pattern="$3"
    printf '%s\n' "$src" > "$WORK/$name.ae"
    out="$("$AE" build "$WORK/$name.ae" -o "$WORK/$name" 2>&1)"
    if [ $? -eq 0 ]; then fail "$name: expected rejection, but it compiled"; fi
    printf '%s\n' "$out" | grep -q "$pattern" \
        || fail "$name: rejected, but not with pattern '$pattern'. Got: $(printf '%s' "$out" | grep -i error | head -1)"
}

# 1. Straight-line use-after-move
expect_reject straight_line '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let b = bytes.new(10)
    r ! Chunk { data: move b }
    let len = bytes.length(b)
}' "use of moved value 'b'"

# 2. If-branch move followed by use after if
expect_reject if_arm_move '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let b = bytes.new(10)
    let cond = true
    if cond {
        r ! Chunk { data: move b }
    }
    let len = bytes.length(b)
}' "use of moved value 'b'"

# 3. Loop body move that repeats
expect_reject loop_move '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let b = bytes.new(10)
    var i = 0
    while i < 2 {
        r ! Chunk { data: move b }
        i = i + 1
    }
}' "use of moved value 'b'"

# 4. Defer use after move
expect_reject defer_use '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let b = bytes.new(10)
    defer { bytes.free(b) }
    r ! Chunk { data: move b }
}' "use of moved value 'b'"

# 5. Borrowed parameter move refusal
expect_reject borrowed_param '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
send_buf(r: ActorRef[Receiver], b: ptr) {
    r ! Chunk { data: move b }
}
main() { }' "cannot move borrowed parameter 'b'"

# 6. Captured variable move refusal
expect_reject captured_var '
import std.bytes
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let b = bytes.new(10)
    let fn_call = || {
        r ! Chunk { data: move b }
    }
}' "cannot move captured variable 'b'"

# 7. Field move refusal
expect_reject field_move '
import std.bytes
struct Box { b: ptr }
message Chunk { data: ptr }
actor Receiver { receive { Chunk(data) -> { } } }
main() {
    let r = spawn(Receiver())
    let box = Box { b: bytes.new(10) }
    r ! Chunk { data: move box.b }
}' "cannot move field 'b' out of object 'box'"

echo "  [PASS] buffer_move_reject: all use-after-move and refusal cases rejected correctly"
exit 0
