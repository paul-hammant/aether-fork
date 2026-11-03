#!/usr/bin/env python3
# Writes the std.msgpack fuzz seed corpus (tests/fuzz/msgpack/corpus/) from the
# hex below, so a reviewer can read what each seed holds. Run it after editing
# this list:  python3 tests/fuzz/msgpack/gen_corpus.py
import os, shutil

SEEDS = {
    # Nil, booleans
    "nil": "c0",
    "false": "c2",
    "true": "c3",

    # Positive FixInt (0x00..0x7f)
    "pos_fixint_0": "00",
    "pos_fixint_1": "01",
    "pos_fixint_42": "2a",
    "pos_fixint_127": "7f",

    # Negative FixInt (0xe0..0xff -> -32..-1)
    "neg_fixint_minus1": "ff",
    "neg_fixint_minus16": "f0",
    "neg_fixint_minus32": "e0",

    # Unsigned integers (u8, u16, u32, u64)
    "uint8_0": "cc00",
    "uint8_255": "ccff",
    "uint16_256": "cd0100",
    "uint16_65535": "cdffff",
    "uint32_65536": "ce00010000",
    "uint32_max": "ceffffffff",
    "uint64_max32": "cf0000000100000000",
    "uint64_topbit": "cf8000000000000000",
    "uint64_max": "cfffffffffffffffff",

    # Signed integers (i8, i16, i32, i64)
    "int8_minus33": "d0df",
    "int8_minus128": "d080",
    "int16_minus129": "d1ff7f",
    "int16_minus32768": "d18000",
    "int32_minus32769": "d2ffff7fff",
    "int32_min": "d280000000",
    "int64_min32": "d3ffffffff7fffffff",
    "int64_min": "d38000000000000000",

    # Floats (float32, float64)
    "float32_zero": "ca00000000",
    "float32_one": "ca3f800000",
    "float32_subnormal": "ca00000001",
    "float32_negsubnormal": "ca80000001",
    "float32_inf": "ca7f800000",
    "float32_nan": "ca7fc00000",
    "float64_zero": "cb0000000000000000",
    "float64_one": "cb3ff0000000000000",
    "float64_inf": "cb7ff0000000000000",
    "float64_nan": "cb7ff8000000000000",

    # FixStr (0xa0..0xbf)
    "fixstr_empty": "a0",
    "fixstr_hello": "a568656c6c6f",
    "fixstr_with_nul": "a100",
    "fixstr_utf8": "a3e6b0b4",
    "fixstr_31": "bf" + "61" * 31,

    # Str 8, 16, 32
    "str8_empty": "d900",
    "str8_hello": "d90568656c6c6f",
    "str16_empty": "da0000",
    "str16_3b": "da0003666f6f",
    "str32_empty": "db00000000",
    "str32_3b": "db00000003626172",

    # Bin 8, 16, 32
    "bin8_empty": "c400",
    "bin8_4b": "c40401020304",
    "bin8_with_nul": "c40200ff",
    "bin16_empty": "c50000",
    "bin16_2b": "c50002dead",
    "bin32_empty": "c600000000",
    "bin32_3b": "c600000003cafeba",

    # FixExt 1, 2, 4, 8, 16
    "fixext1": "d401aa",
    "fixext1_negtype": "d4ffaa",
    "fixext2": "d502aabb",
    "fixext4": "d60301020304",
    "fixext8": "d7040102030405060708",
    "fixext16": "d805" + "01" * 16,

    # Ext 8, 16, 32
    "ext8_empty": "c70001",
    "ext8_3b": "c70302010203",
    "ext16_empty": "c8000001",
    "ext16_2b": "c80002031122",
    "ext32_empty": "c90000000001",
    "ext32_1b": "c9000000010477",

    # FixArray (0x90..0x9f)
    "fixarray_empty": "90",
    "fixarray_1": "9101",
    "fixarray_3": "93010203",
    "fixarray_nested": "9291019102",
    "fixarray_15": "9f" + "01" * 15,

    # Array 16, 32
    "array16_empty": "dc0000",
    "array16_2": "dc00020102",
    "array32_empty": "dd00000000",
    "array32_1": "dd0000000101",

    # FixMap (0x80..0x8f)
    "fixmap_empty": "80",
    "fixmap_1": "81a16101",
    "fixmap_2": "82a16101a16202",
    "fixmap_nested": "81a16ba181a16101",
    "fixmap_15": "8f" + ("a16101" * 15),

    # Map 16, 32
    "map16_empty": "de0000",
    "map16_1": "de0001a16101",
    "map32_empty": "df00000000",
    "map32_1": "df00000001a16101",

    # Nesting boundaries (MAX_DEPTH = 256)
    "nested_arr_255": "91" * 255 + "01",
    "nested_arr_256": "91" * 256 + "01",
    "nested_arr_257": "91" * 257 + "01",
    "nested_map_255": ("81a161" * 255) + "01",
    "nested_map_257": ("81a161" * 257) + "01",

    # Boundaries and length bombs (oversized declared lengths)
    "bomb_str8": "d9ff",
    "bomb_str16": "daffff",
    "bomb_str32": "dbffffffff",
    "bomb_bin8": "c4ff",
    "bomb_bin16": "c5ffff",
    "bomb_bin32": "c6ffffffff",
    "bomb_ext8": "c7ff01",
    "bomb_ext16": "c8ffff01",
    "bomb_ext32": "c9ffffffff01",
    "bomb_arr16": "dcffff",
    "bomb_arr32": "ddffffffff",
    "bomb_arr32_half": "dd80000000",
    "bomb_map16": "deffff",
    "bomb_map32": "dfffffffff",
    "bomb_map32_half": "df80000000",

    # Truncated heads / incomplete markers
    "trunc_u16": "cd01",
    "trunc_u32": "ce0001",
    "trunc_u64": "cf00000001",
    "trunc_i16": "d101",
    "trunc_i32": "d20001",
    "trunc_i64": "d300000001",
    "trunc_float32": "ca3f80",
    "trunc_float64": "cb3ff00000",
    "trunc_fixext2": "d501aa",
    "trunc_fixext4": "d601aabb",
    "trunc_fixext8": "d701aabbccdd",
    "trunc_fixext16": "d801aabbccddeeff",
    "trunc_str8_payload": "d9056162",
    "trunc_bin8_payload": "c4040102",
    "trunc_ext8_payload": "c70401aabb",
    "trunc_fixarray": "930102",
    "trunc_fixmap_val": "81a161",

    # Reserved byte 0xc1 (Never used in MessagePack spec)
    "reserved_c1": "c1",
    "reserved_c1_in_arr": "91c1",
    "reserved_c1_in_map": "81a161c1",

    # Trailing bytes
    "trailing_int_int": "0102",
    "trailing_nil_true": "c0c3",
    "trailing_str_int": "a16105",
    "trailing_arr_nil": "90c0",
    "trailing_map_nil": "80c0",

    # Mixed complex structures
    "mixed_array": "95c0c301ca3f800000a26869",
    "mixed_map": "83a36e696cc0a474727565c3a36e756d05",
    "map_nonstring_keys": "8201a36f6e6502a374776f",
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "corpus")
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
for name, hx in SEEDS.items():
    data = bytes.fromhex(hx.replace(" ", ""))
    assert len(data) > 0, f"seed {name} is empty"
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)
print(f"{len(SEEDS)} seeds in {out}")
