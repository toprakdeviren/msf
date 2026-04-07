/**
 * @file operator.c
 * @brief Operator and precedence group declaration parsers.
 *
 * These declarations are self-contained — they don't interact with other
 * declaration parsers.  Separated from decl.c for that reason.
 *
 *   parse_precedence_group_decl() — precedencegroup Name { ... }
 *   parse_operator_decl()         — prefix/infix/postfix operator <name>
 */
#include "../private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Precedence Group Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Matches an already-consumed attribute name against a string literal. */
#define attr_is(attr, len, lit) \
  ((len) == sizeof(lit) - 1 && memcmp((attr), (lit), sizeof(lit) - 1) == 0)

/** @brief Returns 1 if the current token is a colon (PUNCT or single-char OPERATOR). */
static int p_is_colon(const Parser *p) {
  if (p_is_eof(p)) return 0;
  const Token *t = p_tok(p);
  if (t->type == TOK_PUNCT && p->src->data[t->pos] == ':') return 1;
  if (t->type == TOK_OPERATOR && t->len == 1 && p->src->data[t->pos] == ':') return 1;
  return 0;
}

/** @brief Skips to the next attribute boundary (';' or '}' or newline). */
static void skip_to_attr_end(Parser *p) {
  while (!p_is_eof(p) && !P_RBRACE(p) &&
         cur_char(p) != ';' && p_tok(p)->type != TOK_NEWLINE)
    adv(p);
  if (p_is_punct(p, ';')) adv(p);
}

/** @brief Parses a comma-separated list of group name references. */
static void parse_pg_group_list(Parser *p, ASTNode *parent, ASTNodeKind kind) {
  ASTNode *attr_node = alloc_node(p, kind);
  if (!attr_node) return;
  while (!p_is_eof(p) && !P_RBRACE(p) &&
         (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)) {
    ASTNode *ref = alloc_node(p, AST_PG_GROUP_REF);
    if (!ref) return;
    ref->data.var.name_tok = (uint32_t)p->pos;
    adv(p);
    ast_add_child(attr_node, ref);
    if (P_COMMA(p)) adv(p);
  }
  ast_add_child(parent, attr_node);
}

/** @brief Parses `associativity: left|right|none`. */
static void parse_pg_associativity(Parser *p, ASTNode *parent) {
  ASTNode *node = alloc_node(p, AST_PG_ASSOCIATIVITY);
  if (!node) return;
  if (p_tok(p)->type == TOK_IDENTIFIER) {
    if      (p_is_ident_str(p, CK_LEFT))  node->data.integer.ival = 0;
    else if (p_is_ident_str(p, CK_RIGHT)) node->data.integer.ival = 1;
    else if (p_is_ident_str(p, CK_NONE))  node->data.integer.ival = 2;
    adv(p);
  }
  ast_add_child(parent, node);
}

/** @brief Parses `assignment: true|false`. */
static void parse_pg_assignment(Parser *p, ASTNode *parent) {
  ASTNode *node = alloc_node(p, AST_PG_ASSIGNMENT);
  if (!node) return;
  if      (p_is_kw(p, KW_TRUE))  { node->data.integer.ival = 1; adv(p); }
  else if (p_is_kw(p, KW_FALSE)) { node->data.integer.ival = 0; adv(p); }
  ast_add_child(parent, node);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Precedence Group Declaration
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a precedence group declaration.
 *
 * Syntax:
 * @code
 *   precedencegroup Name {
 *     higherThan:    AdditionPrecedence
 *     lowerThan:     MultiplicationPrecedence
 *     associativity: left
 *     assignment:    true
 *   }
 * @endcode
 */
ASTNode *parse_precedence_group_decl(Parser *p) {
  adv(p); /* precedencegroup */
  ASTNode *node = alloc_node(p, AST_PRECEDENCE_GROUP_DECL);
  if (!node) return NULL;

  if (p_tok(p)->type == TOK_IDENTIFIER) {
    node->data.var.name_tok = (uint32_t)p->pos;
    adv(p);
  }
  if (!P_LBRACE(p)) { node->tok_end = (uint32_t)p->pos; return node; }
  adv(p);

  while (!p_is_eof(p) && !P_RBRACE(p)) {
    if (p_tok(p)->type != TOK_IDENTIFIER) { adv(p); continue; }

    const char *attr = p->src->data + p_tok(p)->pos;
    size_t len = p_tok(p)->len;
    adv(p);

    if (p_is_colon(p)) {
      adv(p);
      if      (attr_is(attr, len, CK_HIGHER_THAN))  parse_pg_group_list(p, node, AST_PG_HIGHER_THAN);
      else if (attr_is(attr, len, CK_LOWER_THAN))   parse_pg_group_list(p, node, AST_PG_LOWER_THAN);
      else if (attr_is(attr, len, CK_ASSOCIATIVITY)) parse_pg_associativity(p, node);
      else if (attr_is(attr, len, CK_ASSIGNMENT))    parse_pg_assignment(p, node);
    }

    skip_to_attr_end(p);
  }

  if (P_RBRACE(p)) adv(p);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Operator Declaration
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses an operator declaration.
 *
 * Syntax: prefix/postfix/infix operator \<name\> [: PrecedenceGroup]
 *
 * @param p         Parser state (pos at prefix/postfix/infix keyword).
 * @param is_infix  If 1, allows a `: PrecedenceGroup` suffix.
 */
ASTNode *parse_operator_decl(Parser *p, int is_infix) {
  adv(p);
  if (!p_is_kw(p, KW_OPERATOR)) {
    if (p_tok(p)->type == TOK_IDENTIFIER && tok_text_eq(p, "operator", 8))
      adv(p);
    else
      return NULL;
  } else {
    adv(p);
  }
  ASTNode *node = alloc_node(p, AST_OPERATOR_DECL);
  if (!node) return NULL;
  node->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_OPERATOR)
    adv(p);
  if (is_infix && !p_is_eof(p) && p_is_colon(p)) {
    adv(p);
    ASTNode *ref = alloc_node(p, AST_PG_GROUP_REF);
    if (!ref) return NULL;
    ref->data.var.name_tok = (uint32_t)p->pos;
    if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
      adv(p);
    ast_add_child(node, ref);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}
