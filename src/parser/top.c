/**
 * @file top.c
 * @brief Top-level dispatch — parse_decl_stmt and parse_source_file.
 *
 * parse_decl_stmt() is the single entry point for parsing any declaration
 * or statement.  It handles attributes, contextual modifiers, keyword
 * dispatch, hash directives, labeled statements, and expression fallback.
 */
#include "private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Tries to consume a contextual modifier if followed by a specific keyword.
 *
 * Used for `convenience init`, `required init` — these are identifiers
 * that act as modifiers only when immediately before the target keyword.
 */
static int try_consume_contextual_mod(Parser *p, const char *ck, size_t ck_len,
                                      Keyword next_kw, uint32_t mod_flag,
                                      uint32_t *mods) {
  if (tok_is_ident(p, ck, ck_len) &&
      p->pos + 1 < p->ts->count &&
      p->ts->tokens[p->pos + 1].keyword == next_kw) {
    *mods |= mod_flag;
    adv(p);
    return 1;
  }
  return 0;
}

/** @brief Parses one `@Attr` (consumes the leading `@`).  Returns the
 *  attribute node, or NULL if the attribute was a marker that doesn't
 *  produce a node (@testable). */
static ASTNode *parse_single_attribute(Parser *p, uint32_t *extra_mods) {
  adv(p);  /* '@' */

  /* @testable import — consume and mark, no attribute node */
  if (p_tok(p)->type == TOK_IDENTIFIER && p_is_ck(p, CK_TESTABLE) &&
      p->pos + 1 < p->ts->count &&
      p->ts->tokens[p->pos + 1].keyword == KW_IMPORT) {
    p->import_is_testable = 1;
    adv(p);
    return NULL;
  }

  ASTNode *attr = alloc_node(p, AST_ATTRIBUTE);
  if (!attr) return NULL;
  attr->data.var.name_tok = (uint32_t)p->pos;

  if (p_is_ident_str(p, CK_MAIN_ACTOR))
    *extra_mods |= MOD_MAIN_ACTOR;

  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
    adv(p);
  /* Property-wrapper attributes can carry generic args before any `(...)`:
   * @Published<Bool>, @ObservedObject<Document>, @IntentParameter<URL>. */
  if (!p_is_eof(p) && cur_char(p) == '<')
    skip_generic_params(p);
  if (P_LPAREN(p))
    skip_balanced(p, '(', ')');
  attr->tok_end = (uint32_t)p->pos;
  return attr;
}

/** @brief Parses a run of `@A @B(args) @C` attributes into a linked list
 *  via `next_sibling`.  Returns the head, with `*out_tail` pointing at the
 *  last node so callers can splice the chain into a declaration's
 *  children. */
ASTNode *parse_attribute_chain(Parser *p, uint32_t *extra_mods,
                               ASTNode **out_tail) {
  ASTNode *head = NULL, *tail = NULL;
  while (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
         p->src->data[p_tok(p)->pos] == '@') {
    ASTNode *a = parse_single_attribute(p, extra_mods);
    if (!a) continue;
    if (!head) head = a;
    if (tail)  tail->next_sibling = a;
    tail = a;
  }
  if (out_tail) *out_tail = tail;
  return head;
}

/** @brief Attaches an attribute chain (linked via next_sibling) as the
 *  leading children of @p decl.  Each attribute's parent/next_sibling is
 *  re-linked so the decl owns the chain structurally.  No-op if either
 *  argument is NULL. */
void attach_attribute_chain(ASTNode *decl, ASTNode *head) {
  if (!decl || !head) return;
  for (ASTNode *a = head, *next; a; a = next) {
    next = a->next_sibling;
    a->next_sibling = NULL;
    a->parent       = NULL;
    ast_add_child(decl, a);
  }
}

/** @brief Collects contextual modifiers before the keyword switch. */
static uint32_t collect_contextual_modifiers(Parser *p) {
  uint32_t extra = 0;

  if (p_is_ident_str(p, CK_NONISOLATED))
    { extra |= MOD_NONISOLATED; adv(p); }

  if (p_is_ident_str(p, CK_INDIRECT))
    { extra |= MOD_INDIRECT; adv(p); }

  try_consume_contextual_mod(p, CK_CONVENIENCE, sizeof(CK_CONVENIENCE) - 1,
                             KW_INIT, MOD_CONVENIENCE, &extra);
  try_consume_contextual_mod(p, CK_REQUIRED, sizeof(CK_REQUIRED) - 1,
                             KW_INIT, MOD_REQUIRED, &extra);
  return extra;
}

/** @brief Handles prefix/postfix/infix operator or function declarations.
 *
 * `prefix`/`postfix`/`infix` may be lexed as either KW_* keywords or as
 * contextual identifiers depending on lexer rules; accept either form. */
static ASTNode *try_parse_fixity_decl(Parser *p, uint32_t mods) {
  int is_infix = 0;
  const Token *t = p_tok(p);
  if (t->type == TOK_KEYWORD &&
      (t->keyword == KW_PREFIX || t->keyword == KW_POSTFIX ||
       t->keyword == KW_INFIX)) {
    is_infix = (t->keyword == KW_INFIX);
  } else if (t->type == TOK_IDENTIFIER &&
             (p_is_ck(p, CK_PREFIX) || p_is_ck(p, CK_POSTFIX) ||
              p_is_ck(p, CK_INFIX))) {
    is_infix = p_is_ck(p, CK_INFIX);
  } else {
    return NULL;
  }
  const Token *next = p_peek1(p);

  if ((next->type == TOK_KEYWORD && next->keyword == KW_OPERATOR) ||
      (next->type == TOK_IDENTIFIER && tok_eq(p, next, "operator"))) {
    adv(p);
    return parse_operator_decl(p, is_infix);
  }
  if (next->type == TOK_KEYWORD && next->keyword == KW_FUNC) {
    adv(p);
    return parse_func_decl(p, mods);
  }
  return NULL;
}

/** @brief Wraps an expression in AST_EXPR_STMT. */
static ASTNode *make_expr_stmt(Parser *p, ASTNode *expr) {
  ASTNode *stmt = ast_arena_alloc(p->arena);
  if (!stmt) return NULL;
  stmt->kind = AST_EXPR_STMT;
  stmt->tok_idx = expr->tok_idx;
  stmt->tok_end = expr->tok_end;
  ast_add_child(stmt, expr);
  return stmt;
}

/** @brief Returns 1 if '#' + next token form a statement-level directive. */
static int is_hash_directive(const Parser *p) {
  if (p_tok(p)->type != TOK_OPERATOR || p_tok(p)->len != 1 ||
      p->src->data[p_tok(p)->pos] != '#')
    return 0;
  const Token *next = p_peek1(p);
  if (next->type == TOK_KEYWORD &&
      (next->keyword == KW_IF || next->keyword == KW_ELSE))
    return 1;
  if (next->type == TOK_IDENTIFIER &&
      (tok_eq(p, next, CK_ELSEIF) || tok_eq(p, next, CK_ENDIF) ||
       tok_eq(p, next, CK_WARNING) || tok_eq(p, next, CK_ERROR) ||
       tok_eq(p, next, CK_SOURCE_LOCATION) || tok_eq(p, next, CK_LINE)))
    return 1;
  return 0;
}

/** @brief Detects `label:` and parses the labeled statement. */
static ASTNode *try_parse_labeled_stmt(Parser *p) {
  if (p_tok(p)->type != TOK_IDENTIFIER || p->pos + 1 >= p->ts->count)
    return NULL;
  const Token *next = &p->ts->tokens[p->pos + 1];
  int is_colon = (next->type == TOK_PUNCT && p->src->data[next->pos] == ':') ||
                 (next->type == TOK_OPERATOR && next->len == 1 &&
                  p->src->data[next->pos] == ':');
  if (!is_colon) return NULL;

  uint32_t label_tok = (uint32_t)p->pos;
  adv(p); adv(p);
  ASTNode *body = parse_stmt(p);
  if (body) body->data.var.name_tok = label_tok;
  return body;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Main Dispatcher
 * ═══════════════════════════════════════════════════════════════════════════════ */

ASTNode *parse_decl_stmt(Parser *p) {
  if (p_is_eof(p)) return NULL;

  /* @attributes — collect a chain so we can attach them structurally to
   * whatever declaration follows, instead of emitting them as bare siblings.
   * Sema looks for AST_ATTRIBUTE *children* of the decl (see callers in
   * semantic/) so attachment is no longer inferred from adjacency. */
  uint32_t extra_mods = 0;
  ASTNode *attr_tail = NULL;
  ASTNode *attr_head = parse_attribute_chain(p, &extra_mods, &attr_tail);

  /* Contextual modifiers: nonisolated, indirect, convenience, required */
  extra_mods |= collect_contextual_modifiers(p);

  uint32_t mods = collect_modifiers(p) | extra_mods;

  /* Post-modifier contextual: `override convenience init` */
  try_consume_contextual_mod(p, CK_CONVENIENCE, sizeof(CK_CONVENIENCE) - 1,
                             KW_INIT, MOD_CONVENIENCE, &mods);
  try_consume_contextual_mod(p, CK_REQUIRED, sizeof(CK_REQUIRED) - 1,
                             KW_INIT, MOD_REQUIRED, &mods);

  ASTNode *result = NULL;

  if (p_is_eof(p)) goto done;

  /* prefix/postfix/infix operator or func */
  ASTNode *fixity = try_parse_fixity_decl(p, mods);
  if (fixity) { result = fixity; goto done; }

  /* Keyword-driven declarations and statements */
  if (p_tok(p)->type == TOK_KEYWORD) {
    switch (p_tok(p)->keyword) {
    /* — Declarations — */
    case KW_ASYNC:
      if (p->pos + 1 < p->ts->count && p_peek1(p)->keyword == KW_LET) {
        adv(p);
        ASTNode *n = parse_var_decl(p, 1, mods);
        if (n) n->data.var.is_async_let = 1;
        result = n; goto done;
      }
      break;
    case KW_FUNC:         result = parse_func_decl(p, mods);       goto done;
    case KW_VAR:          result = parse_var_decl(p, 0, mods);      goto done;
    case KW_LET: {
      ASTNode *n = parse_var_decl(p, 1, mods);
      if (n && (mods & MOD_ASYNC)) n->data.var.is_async_let = 1;
      result = n; goto done;
    }
    case KW_CLASS:        result = parse_nominal(p, AST_CLASS_DECL,     mods); goto done;
    case KW_STRUCT:       result = parse_nominal(p, AST_STRUCT_DECL,    mods); goto done;
    case KW_ENUM:         result = parse_nominal(p, AST_ENUM_DECL,      mods); goto done;
    case KW_PROTOCOL:     result = parse_nominal(p, AST_PROTOCOL_DECL,  mods); goto done;
    case KW_EXTENSION:    result = parse_nominal(p, AST_EXTENSION_DECL, mods); goto done;
    case KW_ACTOR:        result = parse_nominal(p, AST_ACTOR_DECL,     mods); goto done;
    case KW_IMPORT:       result = parse_import_decl(p);                       goto done;
    case KW_TYPEALIAS:    result = parse_typealias(p, mods);                   goto done;
    case KW_INIT:         result = parse_init_decl(p, mods);                   goto done;
    case KW_DEINIT:       result = parse_deinit_decl(p);                       goto done;
    case KW_SUBSCRIPT:    result = parse_subscript_decl(p, mods);              goto done;
    case KW_PRECEDENCEGROUP: result = parse_precedence_group_decl(p);          goto done;

    /* — Statements — */
    case KW_RETURN:       result = parse_return(p);                  goto done;
    case KW_THROW:        result = parse_throw(p);                   goto done;
    case KW_IF:           result = parse_if(p);                      goto done;
    case KW_GUARD:        result = parse_guard(p);                   goto done;
    case KW_FOR:          result = parse_for(p);                     goto done;
    case KW_WHILE:        result = parse_while(p);                   goto done;
    case KW_REPEAT:       result = parse_repeat(p);                  goto done;
    case KW_DO:           result = parse_do(p);                      goto done;
    case KW_DEFER:        result = parse_defer(p);                   goto done;
    case KW_DISCARD:      result = parse_discard(p);                 goto done;
    case KW_SWITCH:       result = parse_switch(p);                  goto done;
    case KW_BREAK:        result = parse_jump(p, AST_BREAK_STMT);    goto done;
    case KW_CONTINUE:     result = parse_jump(p, AST_CONTINUE_STMT); goto done;
    case KW_FALLTHROUGH:  result = parse_jump(p, AST_FALLTHROUGH_STMT); goto done;
    default: break;
    }
  }

  /* precedencegroup (contextual — may be lexed as identifier) */
  if (p_tok(p)->type == TOK_IDENTIFIER &&
      tok_text_eq(p, CK_PRECEDENCEGROUP_ID, sizeof(CK_PRECEDENCEGROUP_ID) - 1)) {
    result = parse_precedence_group_decl(p);
    goto done;
  }

  /* macro declaration (Swift 5.9 — `macro` is contextual). Only when followed
   * by a name, so `macro` as a variable/call (`macro(...)`, `let macro = …`)
   * is left to the expression fallback. */
  if (p_tok(p)->type == TOK_IDENTIFIER && tok_text_eq(p, "macro", 5) &&
      p_peek1(p)->type == TOK_IDENTIFIER) {
    result = parse_macro_decl(p, mods);
    goto done;
  }

  /* Hash directives */
  if (is_hash_directive(p)) { result = parse_hash_directive(p); goto done; }

  /* Labeled statement: `label: for/while/repeat { }` */
  ASTNode *labeled = try_parse_labeled_stmt(p);
  if (labeled) { result = labeled; goto done; }

  /* Expression statement (fallback) */
  ASTNode *expr = parse_expr_pratt(p, 0);
  if (expr) { result = make_expr_stmt(p, expr); goto done; }

  /* Unknown token — skip with diagnostic (semicolons are silent) */
  if (!p_is_eof(p)) {
    int is_semi = (p_tok(p)->type == TOK_PUNCT && p->src->data[p_tok(p)->pos] == ';') ||
                  (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1 &&
                   p->src->data[p_tok(p)->pos] == ';');
    if (!is_semi)
      parse_error_push(p, "%s:%u:%u: unexpected token '%.*s'",
                       p->src->filename, p_tok(p)->line, p_tok(p)->col,
                       (int)p_tok(p)->len, p->src->data + p_tok(p)->pos);
  }
  adv(p);

done:
  /* Attach any leading @attributes structurally to the produced node.  If
   * there is no node (statement-level token we skipped, EOF, etc.) the
   * chain has nowhere to live — emit it as a sibling so the tokens at
   * least round-trip in dumps. */
  if (attr_head) {
    if (result) {
      attach_attribute_chain(result, attr_head);
    } else {
      /* Return only the chain head; caller's add_stmt_chain will splice
       * the rest of the chain via next_sibling. */
      (void)attr_tail;
      return attr_head;
    }
  }
  return result;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Source File
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses an entire source file into an AST_SOURCE_FILE root node. */
ASTNode *parse_source_file(Parser *p) {
  for (int i = 0; i < p->pg_count; i++)
    free(p->pg_table[i].name);
  for (int i = 0; i < p->custom_op_count; i++) {
    free(p->custom_ops[i].op);
    free(p->custom_ops[i].group_name);
  }
  p->pg_count = 0;
  p->custom_op_count = 0;

  ASTNode *root = ast_arena_alloc(p->arena);
  if (!root) return NULL;
  root->kind = AST_SOURCE_FILE;
  root->tok_idx = 0;
  while (!p_is_eof(p))
    add_stmt_chain(p, root, parse_decl_stmt(p));
  root->tok_end = (uint32_t)p->pos;
  return root;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Test Entry Points
 * ═══════════════════════════════════════════════════════════════════════════════ */

ASTNode *parse_decl(Parser *p) { return parse_decl_stmt(p); }
ASTNode *parse_expr(Parser *p) { return parse_expr_pratt(p, 0); }
ASTNode *parse_stmt(Parser *p) { return parse_decl_stmt(p); }
