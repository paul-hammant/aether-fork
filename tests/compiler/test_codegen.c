#include "../runtime/test_harness.h"
#include "../../compiler/parser/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/codegen/codegen.h"
#include "../../compiler/analysis/typechecker.h"
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

/* #2206: `s != "0"` must be a content compare, not a pointer compare.
 * Both the integer literal `0` and the string literal `"0"` carry the
 * text `0`; the null-check shortcut in the string-compare codegen used
 * to match either, so the string one skipped strcmp. The string literal
 * is TYPE_STRING straight out of the parser, which is what the fix keys
 * on — so this runs parser -> typechecker -> codegen and reads the C. */
static char* generate_c_for(const char* source) {
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

TEST(codegen_string_ne_zero_literal_uses_strcmp) {
    char* buf = generate_c_for(
        "main() { a = \"abc\"\n if a != \"0\" { println(\"ne\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(a), _aether_safe_str(\"0\")) != 0") != NULL);
    ASSERT_TRUE(strstr(buf, "a != \"0\"") == NULL);
    free(buf);
}

TEST(codegen_zero_literal_eq_string_uses_strcmp) {
    char* buf = generate_c_for(
        "main() { a = \"abc\"\n if \"0\" == a { println(\"eq\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "strcmp(_aether_safe_str(\"0\"), _aether_safe_str(a)) == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "\"0\" == a") == NULL);
    free(buf);
}

TEST(codegen_ptr_eq_int_zero_stays_pointer_check) {
    char* buf = generate_c_for(
        "main() { let p: ptr = null\n if p == 0 { println(\"null\") } }");
    ASSERT_NOT_NULL(buf);
    ASSERT_TRUE(strstr(buf, "p == 0") != NULL);
    ASSERT_TRUE(strstr(buf, "_aether_safe_str(p)") == NULL);
    free(buf);
}
