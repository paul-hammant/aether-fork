# Fuzzing Aether

This directory contains coverage-guided fuzz harnesses for security-critical
components of the Aether codebase.

## CBOR Fuzzer (`tests/fuzz/cbor/`)

`std.cbor` decodes messages sent between sandboxed children and their parents,
making it a primary attack surface. The fuzz harness tests the pure Aether
implementation (`std/cbor/module.ae`) using libFuzzer and sanitizers (ASan, UBSan).

### Files

- **`fuzz_cbor.ae`**: the target, built `--emit=obj`, exporting
  `fuzz_target(data: ptr, len: int)`. It takes the input whole, zeros
  included (`bytes.string_from_ptr`), and runs the oracle: parse with the
  defaults; round-trip a parsed value through `encode_deterministic` (it must
  re-parse and re-encode to the same bytes); parse again with small limits
  taken from the first byte, where anything those stricter options accept the
  defaults must accept, as the same value. Oracle failures `abort()`; an
  ordinary parse error is a pass.
- **`fuzz_cbor.c`**: `LLVMFuzzerTestOneInput`, which calls `aether_fuzz_target`.
- **`gen_corpus.py`** writes **`corpus/`** from hex, so the seeds can be read
  in review: RFC 8949 Appendix A and some of Appendix F (malformed), the byte
  inputs of `std/cbor/test_cbor_hardening.ae`, an empty input, a truncated
  head, a byte string holding 0x00, arrays nested 255 and 257 deep, duplicate
  keys, indefinite lengths and length bombs. Edit the script, not the files.
- **`cbor.dict`**: the head bytes of every major type and length width, the
  break byte, the simple and float heads, and tags 2, 3, 24 and 55799.
- **`run_fuzz_cbor.sh`**: builds and runs the harness under libFuzzer, ASan
  and UBSan for `FUZZ_SECONDS` (default 60). It first checks that `41 00`
  reaches the decoder as 2 bytes. It fails, saying why, when clang has no
  libFuzzer; it never skips.
- **`prove_mutants.sh`**: plants two bugs in a copy of `std/cbor/module.ae`
  (a local `std/cbor/` shadows the stdlib's) and shows the fuzzer finds each:
  - **M1**: the nesting depth check removed (a stack overflow);
  - **M2**: the check that a string's declared length fits in the bytes left
    removed (an oversized allocation or an out-of-bounds read).

### Running the fuzzer

```sh
make fuzz-cbor                                   # 60 seconds
FUZZ_SECONDS=1800 sh tests/fuzz/cbor/run_fuzz_cbor.sh
```

It is not part of `make test` or CI. The binary, the working corpus (which
grows from run to run) and any reproducer stay in `build/fuzz/cbor/`;
`FUZZ_OUT` moves them.

### Reproducing and minimising a crash

A crash, leak, timeout or oracle failure writes `crash-<sha1>` (or `leak-`,
`timeout-`, `oom-`) into `build/fuzz/cbor/`, and the script names it.

```sh
build/fuzz/cbor/fuzz_cbor build/fuzz/cbor/crash-<sha1>
build/fuzz/cbor/fuzz_cbor -minimize_crash=1 -runs=100000 \
    -exact_artifact_path=min.cbor build/fuzz/cbor/crash-<sha1>
```

Add the minimised input to `gen_corpus.py`, and a regression case to
`std/cbor/test_cbor_hardening.ae`.

## MessagePack Fuzzer (`tests/fuzz/msgpack/`)

`std.msgpack` decodes untrusted binary payloads with JSON's data model. The
fuzz harness tests the pure Aether implementation (`std/msgpack/module.ae`)
using libFuzzer and sanitizers (ASan, UBSan).

### Files

- **`fuzz_msgpack.ae`**: the target, built `--emit=obj`, exporting
  `fuzz_target(data: ptr, len: int)`. It takes the input whole, zeros
  included (`bytes.string_from_ptr`), and checks: no crash/leak/UB/timeout;
  round-trip (`pack(v)` must unpack, and re-packing it must yield the exact
  same bytes); limits (depth <= MAX_DEPTH = 256, no payload length or
  container count exceeds input length); and accessor consistency (valid type
  tag, array/map sizes match accessor iterations, out-of-bounds queries return
  null/-1 without crashing). Oracle failures `abort()`; an unpack error is a
  pass.
- **`fuzz_msgpack.c`**: `LLVMFuzzerTestOneInput`, which calls `aether_fuzz_target`.
- **`gen_corpus.py`** writes **`corpus/`** from hex seeds covering all format
  families: fixint (pos/neg), fixmap, fixarray, fixstr, nil, bools, bin 8/16/32,
  ext 8/16/32, fixext 1/2/4/8/16, float 32/64, uint 8..64, int 8..64,
  str 8/16/32, array 16/32, map 16/32, plus boundaries: length limits,
  truncated forms, nesting near MAX_DEPTH, trailing bytes, and reserved byte 0xc1.
- **`msgpack.dict`**: marker bytes for all MessagePack types using `"\xNN"` escapes.
- **`run_fuzz_msgpack.sh`**: builds and runs the harness under libFuzzer, ASan
  and UBSan for `FUZZ_SECONDS` (default 60). Verifies `a1 00` arrives intact as
  a 2-byte input decoding to a 1-byte string containing NUL.
- **`prove_mutants.sh`**: plants five mutants in a copy of `std/msgpack/module.ae`
  and verifies that the fuzzer catches each within 300 seconds:
  - **M1**: the depth check (`depth > MAX_DEPTH`) removed;
  - **M2**: the declared-length check against remaining input removed;
  - **M3**: the element-count check against remaining input removed (`count_fits`);
  - **M4**: the trailing-bytes refusal in `unpack` removed;
  - **M5**: one fixed-width read's EOF check made one byte short (u64 checking 7 bytes).

### Running the fuzzer

```sh
make fuzz-msgpack                                # 60 seconds
FUZZ_SECONDS=1800 sh tests/fuzz/msgpack/run_fuzz_msgpack.sh
```

It is not part of `make test` or CI. The binary, working corpus, and reproducers
stay in `build/fuzz/msgpack/`; `FUZZ_OUT` moves them.

### AFL++ (optional, untested here)

`afl-clang-fast -fsanitize=fuzzer` links AFL++'s own driver for an
`LLVMFuzzerTestOneInput` harness:

```sh
AE_CC="afl-clang-fast -g -O1" build/ae build tests/fuzz/cbor/fuzz_cbor.ae \
    --with=extern --emit=obj -o fuzz_cbor_ae.o
afl-clang-fast -fsanitize=fuzzer,address,undefined -g -O1 \
    tests/fuzz/cbor/fuzz_cbor.c fuzz_cbor_ae.o $(build/ae cflags) -o fuzz_cbor_afl
afl-fuzz -i tests/fuzz/cbor/corpus -o afl_out -x tests/fuzz/cbor/cbor.dict -- ./fuzz_cbor_afl
```
