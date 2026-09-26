#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/analysis/typechecker.h"
#include "../../compiler/analysis/derive.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Helper: tokenize source into a heap-allocated Token** array.
static Token** tokenize_source(const char* source, int* out_count) {
    lexer_init(source);
    Token** tokens = malloc(sizeof(Token*) * 256);
    int count = 0;
    Token* tok;
    while ((tok = next_token()) != NULL && tok->type != TOKEN_EOF && count < 255) {
        tokens[count++] = tok;
    }
    if (tok) tokens[count++] = tok;  // include EOF token
    *out_count = count;
    return tokens;
}

/* The generated C starts with a multi-thousand-line runtime prelude, so a
 * fixed-size read of the first few KB never reaches the translated program:
 * an assertion against that prefix either fails for the wrong reason or
 * passes on prelude text. Read the whole stream. */
static char* read_all(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long size = ftell(f);
    if (size < 0) return NULL;
    rewind(f);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) return NULL;
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    return buf;
}

TEST(codegen_for_loop_syntax) {
    int count;
    Token** tokens = tokenize_source("main() { for i = 0; i < 3; i++ { print(i) } }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    ASSERT_NOT_NULL(gen);
    generate_program(gen, ast);

    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "int main(") != NULL);
    ASSERT_TRUE(strstr(buf, "for (") != NULL);
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

TEST(codegen_while_loop_syntax) {
    int count;
    Token** tokens = tokenize_source("main() { x = 5\n while x > 0 { x = x - 1 } }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    ASSERT_NOT_NULL(gen);
    generate_program(gen, ast);

    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "int main(") != NULL);
    ASSERT_TRUE(strstr(buf, "while (") != NULL);
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}

/* Parse -> typecheck -> codegen a snippet and return the emitted C, for
 * tests that assert on the generated text. Used by the #2210 typed-fn-pointer
 * tests and the #2206 string-"0"-compare tests below. */
static char* generate_typechecked(const char* source) {
    int count;
    Token** tokens = tokenize_source(source, &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    if (!ast) return NULL;
    if (!typecheck_program(ast)) return NULL;

    FILE* out = tmpfile();
    if (!out) return NULL;
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    char* buf = read_all(out);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
    return buf;
}

TEST(codegen_fnptr_local_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "extern strlen(s: string) -> int\n"
        "main() { prefix = \"lights\"\n"
        "  name = \"${prefix}[0].position\"\n"
        "  f = strlen as fn(string) -> int\n"
        "  println(\"${f(name)}\") }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(f))(aether_string_data(name))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_param_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "through(cb: fn(int, string) -> int, s: string) -> int { return cb(1, s) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(cb))(1, aether_string_data(s))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_struct_field_string_arg_passes_bytes) {
    char* buf = generate_typechecked(
        "struct Ops { measure: fn(string) -> int }\n"
        "by_value(o: Ops, s: string) -> int { return o.measure(s) }\n"
        "by_pointer(p: *Ops, s: string) -> int { return p.measure(s) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(o.measure)(aether_string_data(s))") != NULL);
    ASSERT_TRUE(strstr(buf, "(p->measure)(aether_string_data(s))") != NULL);
    free(buf);
}

TEST(codegen_fnptr_non_string_param_passes_arg_as_written) {
    char* buf = generate_typechecked(
        "through(cb: fn(int, ptr) -> int, n: int, p: ptr) -> int { return cb(n, p) }\n"
        "main() { }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "(cb))(n, p)") != NULL);
    ASSERT_TRUE(strstr(buf, "aether_string_data(p)") == NULL);
    free(buf);
}

/* #2206: `s != "0"` must be a content compare, not a pointer compare. Both
 * the integer literal `0` and the string literal `"0"` carry the text `0`;
 * the null-check shortcut in the string-compare codegen used to match either,
 * so the string one skipped strcmp. The string literal is TYPE_STRING out of
 * the parser, which is what the fix keys on. */
TEST(codegen_string_ne_zero_literal_uses_strcmp) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if a != \"0\" { println(\"ne\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(a), _aether_safe_str(\"0\")) != 0") != NULL);
    ASSERT_TRUE(strstr(buf, "a != \"0\"") == NULL);
    free(buf);
}

TEST(codegen_zero_literal_eq_string_uses_strcmp) {
    char* buf = generate_typechecked(
        "main() { a = \"abc\"\n if \"0\" == a { println(\"eq\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(\"0\"), _aether_safe_str(a)) == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "\"0\" == a") == NULL);
    free(buf);
}

TEST(codegen_ptr_eq_int_zero_stays_pointer_check) {
    char* buf = generate_typechecked(
        "main() { let p: ptr = null\n if p == 0 { println(\"null\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "p == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "_aether_safe_str(p)") == NULL);
    free(buf);
}

/* #2189: a trailing block (`group("first") { ... }`) inlines as a C `{ ... }`
 * block, so a name it binds is out of scope in a sibling block. A callback in
 * the second block binding its own `ml` must not capture the first block's
 * `ml`: the capture analysis used to treat every trailing block as transparent
 * and emitted `_aether_make_closure_1(root, ml)` against a name that had
 * already gone out of scope. The first block's callback still captures it. */
TEST(codegen_closure_does_not_capture_sibling_trailing_block_local) {
    char* buf = generate_typechecked(
        "group(name: string) -> int { return 1 }\n"
        "run(cb: fn) { cb() }\n"
        "main() {\n"
        "    root = \"/tmp/x\"\n"
        "    group(\"first\") {\n"
        "        ml = \"${root}/a\"\n"
        "        run() callback { println(ml) }\n"
        "    }\n"
        "    group(\"second\") {\n"
        "        run() callback {\n"
        "            ml = \"${root}/b\"\n"
        "            println(ml)\n"
        "        }\n"
        "    }\n"
        "}\n");
    ASSERT_NOT_NULL(buf);
    /* First block's callback reads the outer name: captured. */
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_0(ml)") != NULL);
    /* Second block's callback binds its own: only `root` crosses into it. */
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_1(root)") != NULL);
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_1(root, ml)") == NULL);
    free(buf);
}

/* The companion shape: a binding declared in the SAME trailing block, before
 * the callback, is visible to it and a write in the callback goes through. */
TEST(codegen_closure_captures_same_trailing_block_local_it_mutates) {
    char* buf = generate_typechecked(
        "group(name: string) -> int { return 1 }\n"
        "run(cb: fn) { cb() }\n"
        "main() {\n"
        "    group(\"counted\") {\n"
        "        hits = 0\n"
        "        run() callback { hits = hits + 1 }\n"
        "    }\n"
        "}\n");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "_aether_make_closure_0(hits)") != NULL);
    free(buf);
}

/* #2220 — observable struct models. A field store on a value whose struct is
 * `@observable` is followed by `aether_observe_notify(<object>)`: by address
 * for a value (`&(m)`), by the pointer itself for a `*T` (`(p)`). A nested
 * value store notifies innermost first and then the enclosing value. A store
 * on a struct without the attribute emits no call. */
static int count_occurrences(const char* hay, const char* needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char* p = strstr(hay, needle); p; p = strstr(p + len, needle)) n++;
    return n;
}

TEST(codegen_observable_value_store_notifies_by_address) {
    char* buf = generate_typechecked(
        "struct Model @observable { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1\n  m.count += 1 }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(2, count_occurrences(buf, "aether_observe_notify(&(m));"));
    free(buf);
}

TEST(codegen_observable_pointer_store_notifies_pointer) {
    char* buf = generate_typechecked(
        "struct Model @observable { count: int }\n"
        "main() { p = heap.new(Model)\n  p.count = 1\n  heap.free(p) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify((p));"));
    free(buf);
}

TEST(codegen_plain_struct_store_emits_no_notify) {
    char* buf = generate_typechecked(
        "struct Model { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1\n"
        "  p = heap.new(Model)\n  p.count = 2\n  heap.free(p) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify(&"));
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify(("));
    free(buf);
}

TEST(codegen_observable_nested_value_store_notifies_inner_then_outer) {
    char* buf = generate_typechecked(
        "struct Inner @observable { x: int }\n"
        "struct Outer @observable { n: int, inner: Inner }\n"
        "main() { o = Outer { n: 0, inner: Inner { x: 0 } }\n  o.inner.x = 1 }");
    ASSERT_NOT_NULL(buf);
    const char* inner = strstr(buf, "aether_observe_notify(&(o.inner));");
    const char* outer = strstr(buf, "aether_observe_notify(&(o));");
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(outer);
    ASSERT_TRUE(inner < outer);
    free(buf);
}

TEST(codegen_observable_store_through_pointer_field_notifies_pointee_only) {
    char* buf = generate_typechecked(
        "struct Inner @observable { x: int }\n"
        "struct Outer @observable { n: int, link: *Inner }\n"
        "main() { o = Outer { n: 0, link: heap.new(Inner) }\n  o.link.x = 1\n  heap.free(o.link) }");
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify((o.link));"));
    ASSERT_EQ(0, count_occurrences(buf, "aether_observe_notify(&(o));"));
    free(buf);
}

TEST(codegen_observable_survives_derive_pass) {
    int count;
    Token** tokens = tokenize_source(
        "@derive(eq)\nstruct Model @observable { count: int }\n"
        "main() { m = Model { count: 0 }\n  m.count = 1 }", &count);
    Parser* parser = create_parser(tokens, count);
    ASTNode* ast = parse_program(parser);
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ(0, derive_synthesize_pass(ast));
    ASSERT_TRUE(typecheck_program(ast));

    FILE* out = tmpfile();
    ASSERT_NOT_NULL(out);
    CodeGenerator* gen = create_code_generator(out);
    generate_program(gen, ast);
    char* buf = read_all(out);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(1, count_occurrences(buf, "aether_observe_notify(&(m));"));
    ASSERT_TRUE(strstr(buf, "Model_eq") != NULL);   /* derive still ran */
    free(buf);

    fclose(out);
    free_code_generator(gen);
    free_ast_node(ast);
    free_parser(parser);
    for (int i = 0; i < count; i++) free_token(tokens[i]);
    free(tokens);
}
