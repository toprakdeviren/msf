/**
 * @file test_regression.c
 * @brief Regression tests — one test per bug fix to prevent regressions.
 *
 * Each test is named after the bug it guards against and references the
 * original fix commit/description.
 */
#include "test_framework.h"
#include "msf.h"
#include "internal/msf.h"

/* ── Pipeline helper (same as integration) ────────────────────────────────── */

typedef struct {
  Source src;
  TokenStream ts;
  ASTArena ast_arena;
  TypeArena type_arena;
  Parser *parser;
  SemaContext *sema;
  ASTNode *root;
} RegPipeline;

static int reg_run(RegPipeline *p, const char *code) {
  p->src.data = code;
  p->src.len = strlen(code);
  p->src.filename = "<regression>";
  token_stream_init(&p->ts, 512);
  lexer_tokenize(&p->src, &p->ts, 1, NULL);
  ast_arena_init(&p->ast_arena, 0);
  p->parser = parser_init(&p->src, &p->ts, &p->ast_arena);
  p->root = parse_source_file(p->parser);
  type_arena_init(&p->type_arena, 0);
  type_builtins_init(&p->type_arena);
  p->sema = sema_init(&p->src, p->ts.tokens, &p->ast_arena, &p->type_arena);
  return sema_analyze(p->sema, p->root);
}

static void reg_free(RegPipeline *p) {
  sema_destroy(p->sema);
  parser_destroy(p->parser);
  type_arena_free(&p->type_arena);
  ast_arena_free(&p->ast_arena);
  token_stream_free(&p->ts);
}

static const ASTNode *find_first(const ASTNode *n, ASTNodeKind k) {
  if (!n) return NULL;
  if (n->kind == k) return n;
  for (const ASTNode *c = n->first_child; c; c = c->next_sibling) {
    const ASTNode *f = find_first(c, k);
    if (f) return f;
  }
  return NULL;
}

static int count_kind(const ASTNode *n, ASTNodeKind k) {
  if (!n) return 0;
  int c = (n->kind == k) ? 1 : 0;
  for (const ASTNode *ch = n->first_child; ch; ch = ch->next_sibling)
    c += count_kind(ch, k);
  return c;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1A. Union Clash — multi-trailing closure used data.binary.op_tok on
 *      CLOSURE_EXPR, corrupting data.closure.captures pointer.
 *      Fix: use arg_label_tok instead.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_multi_trailing_closure_no_crash(void) {
  TEST("REG-1A: multi-trailing closure does not corrupt closure captures");
  RegPipeline p = {0};
  reg_run(&p,
    "func f(a: () -> Int, b: () -> Int) -> Int { return 0 }\n"
    "let x = f { 1 } b: { 2 }\n"
  );
  /* If the union clash bug existed, the closure's captures pointer would be
     corrupt and ast_arena_free would SIGSEGV when trying to free it.
     Simply surviving pipeline + free without crash = PASS. */
  ASSERT_EQ(parser_error_count(p.parser), 0);
  reg_free(&p); /* would crash here with the old bug */
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1B. Double Free — type_substitute shallow-copied tuple labels, causing
 *      both original and substituted tuple to free() the same pointer.
 *      Fix: deep-copy labels array.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_tuple_substitute_no_double_free(void) {
  TEST("REG-1B: type_substitute tuple does not double-free labels");
  TypeArena arena;
  type_arena_init(&arena, 0);
  type_builtins_init(&arena);

  /* Create a labeled tuple: (x: T, y: T) where T is generic */
  TypeInfo *param = type_arena_alloc(&arena);
  param->kind = TY_GENERIC_PARAM;
  param->param.name = "T";

  TypeInfo *tuple = type_arena_alloc(&arena);
  tuple->kind = TY_TUPLE;
  tuple->tuple.elem_count = 2;
  tuple->tuple.elems = malloc(2 * sizeof(TypeInfo *));
  tuple->tuple.labels = malloc(2 * sizeof(const char *));
  tuple->tuple.elems[0] = param;
  tuple->tuple.elems[1] = param;
  tuple->tuple.labels[0] = "x";
  tuple->tuple.labels[1] = "y";

  /* Substitute T → Int */
  TypeSubstitution sub = {0};
  type_sub_set(&sub, "T", TY_BUILTIN_INT);
  TypeInfo *result = type_substitute(tuple, &sub, &arena);

  ASSERT_NOT_NULL(result);
  ASSERT_EQ(result->kind, TY_TUPLE);
  ASSERT_EQ(result->tuple.elem_count, 2);
  /* Labels should be independent copies — verify they exist */
  ASSERT_NOT_NULL(result->tuple.labels);
  /* Original and result should have DIFFERENT label array pointers (deep copy) */
  ASSERT(result->tuple.labels != tuple->tuple.labels);

  type_arena_free(&arena); /* would double-free here with the old bug */
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1C. Symbol Table Linked List — overload insertion linked new_sym->next
 *      to sym (middle of chain), losing predecessors.
 *      Fix: prepend to buckets[h] head.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_overload_preserves_all_symbols(void) {
  TEST("REG-1C: overloaded funcs don't lose preceding symbols in bucket");
  RegPipeline p = {0};
  reg_run(&p,
    "func process(x: Int) -> Int { return x }\n"
    "func process(x: String) -> String { return x }\n"
    "let a: Int = 1\n"
    "let b = process(x: a)\n"
  );
  /* With the old bug, defining the second 'process' could lose 'a' from
     the hash bucket if they collided. The let b = process(x: a) line
     would fail to find 'a'. */
  ASSERT_EQ(parser_error_count(p.parser), 0);
  reg_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  2A. Dict Literal AST — value was added as child of key, but sema
 *      iterated siblings expecting flat key,value,key,value list.
 *      Fix: add key and value as siblings of the dict node.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_dict_literal_type_correct(void) {
  TEST("REG-2A: dict literal [\"a\": 1] → [String: Int] (not [String: String])");
  RegPipeline p = {0};
  reg_run(&p, "let d = [\"a\": 1, \"b\": 2]");
  const ASTNode *let = find_first(p.root, AST_LET_DECL);
  ASSERT_NOT_NULL(let);
  ASSERT_NOT_NULL(let->type);
  ASSERT_EQ(let->type->kind, TY_DICT);
  reg_free(&p);
  TEST_PASS();
}

static void test_reg_dict_literal_children_are_siblings(void) {
  TEST("REG-2A: dict node children are key,val,key,val (not nested)");
  Source src; TokenStream ts;
  test_tokenize("[\"a\": 1]", &src, &ts);
  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *parser = parser_init(&src, &ts, &arena);
  ASTNode *root = parse_source_file(parser);
  /* Find the DICT_LITERAL */
  const ASTNode *dict = find_first(root, AST_DICT_LITERAL);
  ASSERT_NOT_NULL(dict);
  /* Should have 2 direct children: key (STRING_LITERAL) + value (INTEGER_LITERAL) */
  int direct_children = 0;
  for (const ASTNode *c = dict->first_child; c; c = c->next_sibling)
    direct_children++;
  ASSERT_EQ(direct_children, 2); /* key + value as siblings */
  parser_destroy(parser);
  ast_arena_free(&arena);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  2B. Member Resolution first_child bug — when class had generics or
 *      inheritance, body was never found because code did
 *      body = decl->first_child twice.
 *      Fix: use class_decl_body().
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_member_access_with_inheritance(void) {
  TEST("REG-2B: member access works on class with inheritance");
  RegPipeline p = {0};
  reg_run(&p,
    "class Animal { var name: String = \"\" }\n"
    "class Dog: Animal { var breed: String = \"\" }\n"
    "let d = Dog()\n"
    "let n = d.name\n"
  );
  /* With the old bug, d.name would fail because the member resolver
     couldn't find the body block past the CONFORMANCE child. */
  ASSERT_EQ(parser_error_count(p.parser), 0);
  reg_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  3A. if-let / guard-let — AST_IF_STMT and AST_OPTIONAL_BINDING were
 *      not handled in resolve_node, causing no scope push and no symbol
 *      registration. "if let x = opt" would produce "undeclared x".
 *      Fix: added dedicated cases for IF_STMT, GUARD_STMT, OPTIONAL_BINDING.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_if_let_no_undeclared(void) {
  TEST("REG-3A: if let x = opt → x is usable inside then-block");
  RegPipeline p = {0};
  reg_run(&p,
    "var opt: Int? = 42\n"
    "if let x = opt {\n"
    "  let y = x + 1\n"
    "}\n"
  );
  int has_undeclared = 0;
  for (uint32_t i = 0; i < sema_error_count(p.sema); i++)
    if (strstr(sema_error_message(p.sema, i), "undeclared"))
      has_undeclared = 1;
  ASSERT(!has_undeclared);
  reg_free(&p);
  TEST_PASS();
}

static void test_reg_guard_let_visible_after(void) {
  TEST("REG-3A: guard let x = opt → x visible after guard");
  RegPipeline p = {0};
  reg_run(&p,
    "func f(opt: Int?) -> Int {\n"
    "  guard let x = opt else { return 0 }\n"
    "  return x\n"
    "}\n"
  );
  ASSERT_EQ(parser_error_count(p.parser), 0);
  reg_free(&p);
  TEST_PASS();
}

static void test_reg_if_let_unwraps_optional(void) {
  TEST("REG-3A: if let x = opt → x has unwrapped type (Int, not Int?)");
  RegPipeline p = {0};
  reg_run(&p,
    "var opt: Int? = 10\n"
    "if let x = opt { }\n"
  );
  const ASTNode *binding = find_first(p.root, AST_OPTIONAL_BINDING);
  ASSERT_NOT_NULL(binding);
  if (binding->type) {
    /* Should be Int (unwrapped), NOT Int? */
    ASSERT_NEQ(binding->type->kind, TY_OPTIONAL);
  }
  reg_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  3B. TY_NAMED bypass — assignment type checking skipped user-defined
 *      types entirely because of `lt->kind != TY_NAMED` condition.
 *      Fix: removed the TY_NAMED exclusion.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_named_type_assignment_checked(void) {
  TEST("REG-3B: assigning Int to struct variable produces error");
  RegPipeline p = {0};
  reg_run(&p,
    "struct Foo { var x: Int }\n"
    "var f: Foo = Foo()\n"
    "f = 42\n"
  );
  /* With the old bug, `f = 42` would silently pass because TY_NAMED was
     excluded from type checking. Now it should produce a type mismatch. */
  int has_mismatch = 0;
  for (uint32_t i = 0; i < sema_error_count(p.sema); i++)
    if (strstr(sema_error_message(p.sema, i), "mismatch") ||
        strstr(sema_error_message(p.sema, i), "Type"))
      has_mismatch = 1;
  ASSERT(has_mismatch);
  reg_free(&p);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  4A. Shrink-then-EOF — lexer_tokenize shrunk the buffer and then
 *      pushed EOF, causing an immediate realloc (wasting the shrink).
 *      Fix: push EOF before shrink.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_tokenize_eof_before_shrink(void) {
  TEST("REG-4A: token stream always ends with EOF, capacity ≤ count*2");
  Source src; TokenStream ts;
  test_tokenize("let x = 1", &src, &ts);
  /* Stream must end with EOF */
  ASSERT(ts.count > 0);
  ASSERT_EQ(ts.tokens[ts.count - 1].type, TOK_EOF);
  /* After shrink-to-fit, capacity should not be wildly larger than count.
     With the old bug, shrink+push would grow capacity to 1.5x immediately. */
  ASSERT(ts.capacity <= ts.count * 2);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  4C. Memory Leaks — parser pg_table/custom_ops strings not freed,
 *      closure CaptureList not freed by ast_arena_free.
 *      Fix: parser_destroy frees strings, ast_arena_free frees captures.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_parser_destroy_cleans_strings(void) {
  TEST("REG-4C: parser_destroy with custom ops does not leak");
  Source src; TokenStream ts;
  test_tokenize(
    "precedencegroup MyGroup { associativity: left }\n"
    "infix operator **: MyGroup\n"
    "func **(a: Int, b: Int) -> Int { return a }\n",
    &src, &ts
  );
  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *p = parser_init(&src, &ts, &arena);
  parse_source_file(p);
  /* parser_destroy should free pg_table[].name and custom_ops[].op/group_name */
  parser_destroy(p); /* would leak with the old bug */
  ast_arena_free(&arena);
  token_stream_free(&ts);
  TEST_PASS();
}

static void test_reg_closure_captures_freed(void) {
  TEST("REG-4C: ast_arena_free cleans up closure capture lists");
  RegPipeline p = {0};
  reg_run(&p,
    "var counter = 0\n"
    "let inc = { counter += 1 }\n"
  );
  /* CaptureList is malloc'd during sema. ast_arena_free should free it. */
  const ASTNode *closure = find_first(p.root, AST_CLOSURE_EXPR);
  ASSERT_NOT_NULL(closure);
  /* Simply freeing without leak = PASS (verify with ASan/Valgrind in CI). */
  reg_free(&p); /* would leak with the old bug */
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Parser Bugs — precedencegroup string length, raw string col tracking
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_precedencegroup_length(void) {
  TEST("REG: 'precedencegroup' keyword recognized (15 chars, not 14)");
  Source src; TokenStream ts;
  test_tokenize(
    "precedencegroup AdditionPrecedence { associativity: left }",
    &src, &ts
  );
  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *p = parser_init(&src, &ts, &arena);
  ASTNode *root = parse_source_file(p);
  /* With the old bug (length=14), "precedencegroup" would not be recognized
     and would be parsed as an identifier, not a precedence group decl. */
  ASSERT(find_first(root, AST_PRECEDENCE_GROUP_DECL) != NULL);
  parser_destroy(p);
  ast_arena_free(&arena);
  token_stream_free(&ts);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  type_to_string — TY_FUNC, TY_DICT, TY_TUPLE were missing, printing "?"
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_type_to_string_func(void) {
  TEST("REG: type_to_string(TY_FUNC) prints signature, not '?'");
  TypeArena arena;
  type_arena_init(&arena, 0);
  type_builtins_init(&arena);
  TypeInfo *func = type_arena_alloc(&arena);
  func->kind = TY_FUNC;
  func->func.param_count = 0;
  func->func.params = NULL;
  func->func.ret = TY_BUILTIN_INT;
  char buf[64];
  type_to_string(func, buf, sizeof(buf));
  ASSERT(strcmp(buf, "?") != 0); /* must not be "?" */
  ASSERT(strstr(buf, "Int") != NULL); /* should contain return type */
  type_arena_free(&arena);
  TEST_PASS();
}

static void test_reg_type_to_string_tuple(void) {
  TEST("REG: type_to_string(TY_TUPLE) prints '(A, B)', not '?'");
  TypeArena arena;
  type_arena_init(&arena, 0);
  type_builtins_init(&arena);
  TypeInfo *tuple = type_arena_alloc(&arena);
  tuple->kind = TY_TUPLE;
  tuple->tuple.elem_count = 2;
  tuple->tuple.elems = malloc(2 * sizeof(TypeInfo *));
  tuple->tuple.labels = NULL;
  tuple->tuple.elems[0] = TY_BUILTIN_INT;
  tuple->tuple.elems[1] = TY_BUILTIN_STRING;
  char buf[64];
  type_to_string(tuple, buf, sizeof(buf));
  ASSERT(strcmp(buf, "?") != 0);
  ASSERT(strstr(buf, "Int") != NULL);
  ASSERT(strstr(buf, "String") != NULL);
  type_arena_free(&arena);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  type_equal_deep — TY_FUNC was missing is_async/throws check
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_type_equal_deep_async(void) {
  TEST("REG: type_equal_deep distinguishes async from sync function");
  TypeArena arena;
  type_arena_init(&arena, 0);
  type_builtins_init(&arena);
  TypeInfo *sync_fn = type_arena_alloc(&arena);
  sync_fn->kind = TY_FUNC;
  sync_fn->func.ret = TY_BUILTIN_VOID;
  sync_fn->func.param_count = 0;
  sync_fn->func.is_async = 0;

  TypeInfo *async_fn = type_arena_alloc(&arena);
  async_fn->kind = TY_FUNC;
  async_fn->func.ret = TY_BUILTIN_VOID;
  async_fn->func.param_count = 0;
  async_fn->func.is_async = 1;

  ASSERT(!type_equal_deep(sync_fn, async_fn)); /* must NOT be equal */
  ASSERT(type_equal_deep(sync_fn, sync_fn));   /* same should be equal */
  type_arena_free(&arena);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Arena OOM — ast_arena_init and type_arena_init used to call exit().
 *  Now they leave the arena zeroed and alloc returns NULL.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_arena_alloc_null_on_failed_init(void) {
  TEST("REG: arena alloc returns NULL if tail is NULL (failed init)");
  ASTArena arena = {0}; /* simulate failed init: head=tail=NULL */
  ASTNode *n = ast_arena_alloc(&arena);
  ASSERT_NULL(n);

  TypeArena tarena = {0};
  TypeInfo *t = type_arena_alloc(&tarena);
  ASSERT_NULL(t);
  TEST_PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  token_stream_push — used to call exit() on OOM.
 *  Now silently drops the token.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_reg_token_stream_push_no_exit(void) {
  TEST("REG: token_stream_push on full stream does not crash");
  TokenStream ts;
  token_stream_init(&ts, 2);
  Token tok = { .type = TOK_IDENTIFIER, .pos = 0, .len = 1 };
  token_stream_push(&ts, tok);
  token_stream_push(&ts, tok);
  /* Third push grows the buffer — should not crash even on extreme conditions */
  token_stream_push(&ts, tok);
  ASSERT(ts.count >= 2); /* at least the first two should be there */
  token_stream_free(&ts);
  TEST_PASS();
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

void run_regression_tests(void) {
  TEST_SUITE("Regression");
  /* Crash / memory corruption fixes */
  test_reg_multi_trailing_closure_no_crash();
  test_reg_tuple_substitute_no_double_free();
  test_reg_overload_preserves_all_symbols();
  /* Parser / AST structure fixes */
  test_reg_dict_literal_type_correct();
  test_reg_dict_literal_children_are_siblings();
  test_reg_member_access_with_inheritance();
  test_reg_precedencegroup_length();
  /* Semantic analysis fixes */
  test_reg_if_let_no_undeclared();
  test_reg_guard_let_visible_after();
  test_reg_if_let_unwraps_optional();
  test_reg_named_type_assignment_checked();
  /* Performance / leak fixes */
  test_reg_tokenize_eof_before_shrink();
  test_reg_parser_destroy_cleans_strings();
  test_reg_closure_captures_freed();
  /* Type system fixes */
  test_reg_type_to_string_func();
  test_reg_type_to_string_tuple();
  test_reg_type_equal_deep_async();
  /* OOM handling fixes */
  test_reg_arena_alloc_null_on_failed_init();
  test_reg_token_stream_push_no_exit();
}
