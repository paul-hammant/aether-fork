# Fuzzing Aether

This directory contains coverage-guided fuzz harnesses for security-critical
components of the Aether codebase.

## CBOR Fuzzer (`tests/fuzz/cbor/`)

`std.cbor` decodes messages sent between sandboxed children and their parents,
making it a primary attack surface. The fuzz harness tests the pure Aether
implementation (`std/cbor/module.ae`) using libFuzzer and sanitizers (ASan, UBSan).

### Files

- **`fuzz_cbor.ae`**: Aether library compiled with `--emit=obj` exporting `fuzz_target(data: ptr, len: int)`.
  It adopts raw binary input whole without truncation (preserving NUL bytes) using `std.bytes`.
- **`fuzz_cbor.c`**: Implements `LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)` and forwards
  inputs to `aether_fuzz_target`.
- **`corpus/`**: Curated binary seed inputs including RFC 8949 Appendix A examples, `std.cbor` hardening
  and regression tests, 0-length input, truncated headers, byte strings containing 0x00, nested arrays at
  depth 255 and 257, duplicate map keys, and indefinite-length collections.
- **`cbor.dict`**: libFuzzer dictionary covering CBOR major types, additional information bytes, break byte (`0xff`),
  and common tags (2, 3, 24, 55799).
- **`run_fuzz_cbor.sh`**: Builds and runs the harness under libFuzzer, AddressSanitizer, and UndefinedBehaviorSanitizer
  for `FUZZ_SECONDS` (default 60).
- **`prove_mutants.sh`**: Mutation testing script demonstrating that the harness reliably detects bugs:
  - **M1**: Removed nesting depth check (stack overflow on deep nesting).
  - **M2**: Removed declared length check against remaining bytes (out-of-memory / out-of-bounds access).

### Running the Fuzzer

To run the fuzzer using `make`:

```sh
make fuzz-cbor
```

Or run the script directly with custom parameters:

```sh
FUZZ_SECONDS=300 ./tests/fuzz/cbor/run_fuzz_cbor.sh
```

### Reproducing a Crash

When libFuzzer encounters a crash, leak, or oracle failure, it writes a reproducer file (e.g., `crash-<sha1>`):

```sh
# Build the harness binary if not already built
./tests/fuzz/cbor/run_fuzz_cbor.sh   # sets up the build in a scratch dir

# Or manually run the harness binary against the reproducer file:
./fuzz_cbor <path_to_reproducer_file>
```

### Minimising a Crash

To reduce a reproducer input to its minimal failing size:

```sh
./fuzz_cbor -minimize_crash=1 -exact_artifact_path=minimized_repro <path_to_reproducer_file>
```

### Running with AFL++ (Optional)

If AFL++ is installed (`afl-clang-fast`), you can build the harness using AFL++'s libFuzzer compatibility layer:

```sh
# 1. Compile fuzz_cbor.ae to an object file with AFL instrumentation:
AE_CC="afl-clang-fast -g -O1" ./build/ae build tests/fuzz/cbor/fuzz_cbor.ae --with=extern --emit=obj -o fuzz_cbor_ae.o

# 2. Link with afl-clang-fast:
afl-clang-fast -g -O1 -fsanitize=address,undefined tests/fuzz/cbor/fuzz_cbor.c fuzz_cbor_ae.o $(./build/ae cflags) -o fuzz_cbor_afl

# 3. Run with afl-fuzz:
afl-fuzz -i tests/fuzz/cbor/corpus -o afl_out -x tests/fuzz/cbor/cbor.dict -- ./fuzz_cbor_afl @@
```
