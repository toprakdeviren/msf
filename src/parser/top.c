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

/** @brief Parses @attributes, returning the first one found (or NULL). */
static ASTNode *parse_attributes(Parser *p, uint32_t *extra_mods) {
  while (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
         p->src->data[p_tok(p)->pos] == '@') {
    adv(p);

    /* @testable import — consume and mark, no attribute node */
    if (p_tok(p)->type == TOK_IDENTIFIER &&
        p_is_ck(p, CK_TESTABLE) &&
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
    if (P_LPAREN(p))
      skip_balanced(p, '(', ')');
    attr->tok_end = (uint32_t)p->pos;
    return attr;
  }
  return NULL;
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

/** @brief Handles prefix/postfix/infix operator or function declarations. */
static ASTNode *try_parse_fixity_decl(Parser *p, uint32_t mods) {
  if (p_tok(p)->type != TOK_IDENTIFIER) return NULL;
  if (!p_is_ck(p, CK_PREFIX) && !p_is_ck(p, CK_POSTFIX) && !p_is_ck(p, CK_INFIX))
    return NULL;

  int is_infix = p_is_ck(p, CK_INFIX);
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

  /* @attributes */
  uint32_t extra_mods = 0;
  ASTNode *attr = parse_attributes(p, &extra_mods);
  if (attr) return attr;

  /* Contextual modifiers: nonisolated, indirect, convenience, required */
  extra_mods |= collect_contextual_modifiers(p);

  uint32_t mods = collect_modifiers(p) | extra_mods;

  /* Post-modifier contextual: `override convenience init` */
  try_consume_contextual_mod(p, CK_CONVENIENCE, sizeof(CK_CONVENIENCE) - 1,
                             KW_INIT, MOD_CONVENIENCE, &mods);
  try_consume_contextual_mod(p, CK_REQUIRED, sizeof(CK_REQUIRED) - 1,
                             KW_INIT, MOD_REQUIRED, &mods);

  if (p_is_eof(p)) return NULL;

  /* prefix/postfix/infix operator or func */
  ASTNode *fixity = try_parse_fixity_decl(p, mods);
  if (fixity) return fixity;

  /* Keyword-driven declarations and statements */
  if (p_tok(p)->type == TOK_KEYWORD) {
    switch (p_tok(p)->keyword) {
    /* — Declarations — */
    case KW_ASYNC:
      if (p->pos + 1 < p->ts->count && p_peek1(p)->keyword == KW_LET) {
        adv(p);
        ASTNode *n = parse_var_decl(p, 1, mods);
        if (n) n->data.var.is_async_let = 1;
        return n;
      }
      break;
    case KW_FUNC:         return parse_func_decl(p, mods);
    case KW_VAR:          return parse_var_decl(p, 0, mods);
    case KW_LET: {
      ASTNode *n = parse_var_decl(p, 1, mods);
      if (n && (mods & MOD_ASYNC)) n->data.var.is_async_let = 1;
      return n;
    }
    case KW_CLASS:        return parse_nominal(p, AST_CLASS_DECL, mods);
    case KW_STRUCT:       return parse_nominal(p, AST_STRUCT_DECL, mods);
    case KW_ENUM:         return parse_nominal(p, AST_ENUM_DECL, mods);
    case KW_PROTOCOL:     return parse_nominal(p, AST_PROTOCOL_DECL, mods);
    case KW_EXTENSION:    return parse_nominal(p, AST_EXTENSION_DECL, mods);
    case KW_ACTOR:        return parse_nominal(p, AST_ACTOR_DECL, mods);
    case KW_IMPORT:       return parse_import_decl(p);
    case KW_TYPEALIAS:    return parse_typealias(p, mods);
    case KW_INIT:         return parse_init_decl(p, mods);
    case KW_DEINIT:       return parse_deinit_decl(p);
    case KW_SUBSCRIPT:    return parse_subscript_decl(p, mods);
    case KW_PRECEDENCEGROUP: return parse_precedence_group_decl(p);

    /* — Statements — */
    case KW_RETURN:       return parse_return(p);
    case KW_THROW:        return parse_throw(p);
    case KW_IF:           return parse_if(p);
    case KW_GUARD:        return parse_guard(p);
    case KW_FOR:          return parse_for(p);
    case KW_WHILE:        return parse_while(p);
    case KW_REPEAT:       return parse_repeat(p);
    case KW_DO:           return parse_do(p);
    case KW_DEFER:        return parse_defer(p);
    case KW_DISCARD:      return parse_discard(p);
    case KW_SWITCH:       return parse_switch(p);
    case KW_BREAK:        return parse_jump(p, AST_BREAK_STMT);
    case KW_CONTINUE:     return parse_jump(p, AST_CONTINUE_STMT);
    case KW_FALLTHROUGH:  return parse_jump(p, AST_FALLTHROUGH_STMT);
    default: break;
    }
  }

  /* precedencegroup (contextual — may be lexed as identifier) */
  if (p_tok(p)->type == TOK_IDENTIFIER &&
      tok_text_eq(p, CK_PRECEDENCEGROUP_ID, sizeof(CK_PRECEDENCEGROUP_ID) - 1))
    return parse_precedence_group_decl(p);

  /* Hash directives */
  if (is_hash_directive(p))
    return parse_hash_directive(p);

  /* Labeled statement: `label: for/while/repeat { }` */
  ASTNode *labeled = try_parse_labeled_stmt(p);
  if (labeled) return labeled;

  /* Expression statement (fallback) */
  ASTNode *expr = parse_expr_pratt(p, 0);
  if (expr) return make_expr_stmt(p, expr);

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
  return NULL;
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
