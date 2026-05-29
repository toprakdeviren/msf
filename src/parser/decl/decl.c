/**
 * @file decl.c
 * @brief Declaration dispatch — block, import, typealias, enum, nominal types.
 *
 * This is the "hub" for declaration parsing.  Simple declarations live here;
 * complex ones are split into focused files:
 *
 *   var.c      — var/let, computed properties, observers
 *   func.c     — func, init, deinit, subscript
 *   operator.c — operator, precedencegroup
 *
 * Functions in this file:
 *   parse_block()      — { stmt; stmt; ... }
 *   parse_import_decl() — import Module.Sub
 *   parse_typealias()   — typealias Name<T> = Type
 *   tok_is_ident()      — identifier text comparison helper
 *   parse_optional_param_name() — (paramName) helper
 *   parse_enum_body()   — { case a, b(Int); ... }
 *   parse_nominal()     — class/struct/enum/protocol/actor/extension
 */
#include "../private.h"

/* ═════════════════════════════════════════════════════════════════════════════
 * Block
 * ══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a brace-delimited block: { stmt; stmt; ... }.
 *
 * Multi-var declarations (var x = 0, y = 0) produce sibling-chained nodes;
 * this function walks the chain and adds each as a separate child.
 * Also registers precedence groups and operator declarations encountered
 * inside the block.
 */
ASTNode *parse_block(Parser *p) {
  if (parser_recurse_enter(p) != 0) {
    parse_error_push(p, "%s:%u:%u: block too deeply nested",
                     p->src->filename, p_tok(p)->line, p_tok(p)->col);
    return NULL;
  }
  ASTNode *node = alloc_node(p, AST_BLOCK);
  if (!node) { parser_recurse_leave(p); return NULL; }
  if (!P_LBRACE(p)) {
    parse_error_push(p, "%s:%u:%u: expected '{'", p->src->filename,
                     p_tok(p)->line, p_tok(p)->col);
    parser_recurse_leave(p);
    return node;
  }
  adv(p);
  while (!p_is_eof(p) && !P_RBRACE(p)) {
    size_t blk_before = p->pos;
    add_stmt_chain(p, node, parse_decl_stmt(p));
    if (p->pos == blk_before)
      adv(p);
  }
  if (P_RBRACE(p))
    adv(p);
  node->tok_end = (uint32_t)p->pos;
  parser_recurse_leave(p);
  return node;
}

/* ═════════════════════════════════════════════════════════════════════════════
 * Import & Typealias
 * ═════════════════════════════════════════════════════════════════════════════ */

static int is_import_kind(Parser *p) {
  if (p_is_eof(p))
    return 0;
  if (p_tok(p)->type == TOK_KEYWORD) {
    switch (p_tok(p)->keyword) {
      case KW_TYPEALIAS:
      case KW_STRUCT:
      case KW_CLASS:
      case KW_ENUM:
      case KW_PROTOCOL:
      case KW_LET:
      case KW_VAR:
      case KW_FUNC:
      case KW_OPERATOR:
        return 1;
      default:
        return 0;
    }
  }
  return tok_text_eq(p, "operator", 8);
}

/** @brief Parses: import [kind] Module.Sub.Path */
ASTNode *parse_import_decl(Parser *p) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_IMPORT_DECL);
  if (!node)
    return NULL;
  if (p->import_is_testable) {
    node->modifiers |= MOD_TESTABLE_IMPORT;
    p->import_is_testable = 0;
  }
  if (is_import_kind(p))
    adv(p);
  node->data.var.name_tok = (uint32_t)p->pos;
  while (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD) {
    adv(p);
    if (p_is_punct(p, '.'))
      adv(p);
    else
      break;
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/** @brief Parses: typealias Name\<T\> = Type */
ASTNode *parse_typealias(Parser *p, uint32_t mods) {
  adv(p);
  ASTNode *node = alloc_node(p, AST_TYPEALIAS_DECL);
  if (!node)
    return NULL;
  node->modifiers = mods;
  node->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER)
    adv(p);
  parse_generic_params(p, node);
  if (!p_is_eof(p) && cur_char(p) == '=') {
    adv(p);
    ASTNode *rhs = parse_type(p);
    if (rhs)
      ast_add_child(node, rhs);
  }
  if (p_is_kw(p, KW_WHERE)) {
    ASTNode *wc = parse_where_clause(p);
    if (wc)
      ast_add_child(node, wc);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helper Utilities
 * ══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns 1 if the current token is an identifier matching @p name. */
int tok_is_ident(const Parser *p, const char *name, size_t len) {
  return p_tok(p)->type == TOK_IDENTIFIER && p_tok(p)->len == len &&
         memcmp(p->src->data + p_tok(p)->pos, name, len) == 0;
}

/** @brief Parses an optional (paramName) and returns the token index (0 if absent). */
uint32_t parse_optional_param_name(Parser *p) {
  if (!P_LPAREN(p))
    return 0;
  adv(p);
  if (p_tok(p)->type != TOK_IDENTIFIER) {
    if (P_RPAREN(p))
      adv(p);
    return 0;
  }
  uint32_t tok = (uint32_t)p->pos;
  adv(p);
  if (P_RPAREN(p))
    adv(p);
  return tok;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Enum Body
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses associated value parameters: (label: Type, ...). */
static void parse_assoc_values(Parser *p, ASTNode *elem) {
  adv(p); /* '(' */
  while (!p_is_eof(p) && !P_RPAREN(p)) {
    ASTNode *param = alloc_node(p, AST_PARAM);
    if (!param) return;
    if (p_tok(p)->type == TOK_IDENTIFIER) {
      param->data.var.name_tok = (uint32_t)p->pos;
      const Token *peek = p_peek1(p);
      if (peek->type == TOK_PUNCT && p->src->data[peek->pos] == ':') {
        adv(p); /* label */
        adv(p); /* ':' */
      }
    }
    ASTNode *ty = parse_type(p);
    if (ty) ast_add_child(param, ty);
    /* Associated values can carry a default in synthesized interfaces:
     * `case hourly(during: DateInterval = DateInterval(...))`.  Consume `= expr`,
     * balance-skipping any remainder (multiline calls) to the next ','/')'. */
    if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '=' && p_tok(p)->len == 1) {
      adv(p);
      ASTNode *def = parse_expr_pratt(p, 0);
      if (def) ast_add_child(param, def);
      int d = 0;
      while (!p_is_eof(p)) {
        char ch = p->src->data[p_tok(p)->pos];
        if (d == 0 && (ch == ',' || ch == ')')) break;
        if (ch == '(' || ch == '[' || ch == '{') d++;
        else if (ch == ')' || ch == ']' || ch == '}') d--;
        adv(p);
      }
    }
    ast_add_child(elem, param);
    if (P_COMMA(p)) adv(p);
  }
  if (P_RPAREN(p)) adv(p);
}

/** @brief True if the current token can name an enum case.
 *
 * Besides plain identifiers, Swift's declaration-modifier keywords are
 * contextual and may be used bare as case names (`case open`, `case final`,
 * `case `static``...).  They tokenize as hard keywords, but after `case` there
 * is no modifier position, so accepting them here is unambiguous. */
static int token_can_name_enum_case(const Parser *p) {
  /* An identifier, or any keyword used as a case name (`case actor`, `case open`,
   * `case `default``).  After `case` the next token is always the element name. */
  return p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD;
}

static ASTNode *parse_enum_element(Parser *p) {
  ASTNode *elem = alloc_node(p, AST_ENUM_ELEMENT_DECL);
  if (!elem) return NULL;
  elem->data.var.name_tok = (uint32_t)p->pos;
  if (token_can_name_enum_case(p)) adv(p);

  if (P_LPAREN(p))
    parse_assoc_values(p, elem);

  if (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
      p->src->data[p_tok(p)->pos] == '=' && p_tok(p)->len == 1) {
    adv(p);
    ASTNode *val = parse_expr_pratt(p, 0);
    if (val) ast_add_child(elem, val);
  }
  return elem;
}

/**
 * @brief Parses an enum body: case declarations, methods, nested types.
 *
 * Handles: indirect case, comma-separated elements, associated values,
 * raw values, and arbitrary member declarations (methods, init, etc.).
 */
void parse_enum_body(Parser *p, ASTNode *parent) {
  if (!P_LBRACE(p)) {
    parse_error_push(p, "%s:%u:%u: expected '{' for enum body", p->src->filename,
                     p_tok(p)->line, p_tok(p)->col);
    return;
  }
  ASTNode *body = alloc_node(p, AST_BLOCK);
  if (!body) return;
  adv(p);

  while (!p_is_eof(p) && !P_RBRACE(p)) {
    size_t before = p->pos;

    /* A case may carry leading attributes (`@available(...) case x`, common in
     * real code and ubiquitous in .swiftinterface) and the `indirect` modifier.
     * Parse that prefix; if `case` follows it is a case (attach the
     * attributes).  Otherwise rewind and let parse_decl_stmt handle the member
     * (methods / nested types / computed vars parse their own attributes). */
    uint32_t extra_mods = 0;
    ASTNode *attr_tail = NULL;
    ASTNode *attr_head = parse_attribute_chain(p, &extra_mods, &attr_tail);

    uint32_t case_mods = extra_mods;
    if (p_is_ident_str(p, CK_INDIRECT)) { case_mods |= MOD_INDIRECT; adv(p); }

    if (p_is_kw(p, KW_CASE)) {
      ASTNode *case_decl = alloc_node(p, AST_ENUM_CASE_DECL);
      if (!case_decl) return;
      case_decl->modifiers = case_mods;
      if (attr_head) attach_attribute_chain(case_decl, attr_head);
      adv(p);
      do {
        ASTNode *elem = parse_enum_element(p);
        if (elem) ast_add_child(case_decl, elem);
      } while (P_COMMA(p) && (adv(p), 1));
      ast_add_child(body, case_decl);
    } else {
      p->pos = before;  /* rewind; parse_decl_stmt re-parses any attributes */
      add_stmt_chain(p, body, parse_decl_stmt(p));
    }

    if (p->pos == before) adv(p);
  }

  if (P_RBRACE(p)) adv(p);
  body->tok_end = (uint32_t)p->pos;
  ast_add_child(parent, body);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Nominal Type Declarations
 * ═════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a nominal type declaration.
 *
 * Shared entry point for: class, struct, enum, protocol, actor, extension.
 * Syntax: keyword Name\<T\>: Proto where T: P { body }
 *
 * Dispatches to specialized body parsers for protocol and enum;
 * other types use the generic parse_block().
 *
 * @param p     Parser state (pos at the type keyword).
 * @param kind  Which AST node kind to produce.
 * @param mods  Modifier bitmask from collect_modifiers().
 */
ASTNode *parse_nominal(Parser *p, ASTNodeKind kind, uint32_t mods) {
  adv(p);
  ASTNode *node = alloc_node(p, kind);
  if (!node)
    return NULL;
  node->modifiers = mods;
  node->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER)
    adv(p);

  if (kind == AST_EXTENSION_DECL) {
    /* Extension targets are types, not generic parameter declarations:
     * `extension Foo<T>.Bar: P where ... {}`.  The semantic model stores the
     * leading name token today, so consume the qualified suffix for parser
     * recovery and leave richer target typing to a later AST shape. */
    while (!p_is_eof(p)) {
      if (cur_char(p) == '<') {
        int depth = 0;
        do {
          if (cur_char(p) == '<') depth++;
          else if (cur_char(p) == '>') depth--;
          adv(p);
        } while (!p_is_eof(p) && depth > 0);
        continue;
      }
      int is_dot = p_is_punct(p, '.') ||
                   (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1 &&
                    p->src->data[p_tok(p)->pos] == '.');
      if (!is_dot)
        break;
      adv(p);
      if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD) {
        /* The extended type is the LAST component of a qualified path:
         * `extension Dispatch.DispatchQueue` extends DispatchQueue, not the
         * `Dispatch` module qualifier.  Track the last identifier so name_tok
         * names the actual type (correct for sema's extension→type matching
         * and for vocabulary extraction). */
        node->data.var.name_tok = (uint32_t)p->pos;
        adv(p);
      } else
        break;
    }
  } else if (!p_is_eof(p) && cur_char(p) == '<') {
    parse_generic_params(p, node);
  }

  if (P_COLON(p)) {
    ASTNode *conf = parse_inheritance_clause(p);
    if (conf)
      ast_add_child(node, conf);
  }

  if (p_is_kw(p, KW_WHERE)) {
    ASTNode *wc = parse_where_clause(p);
    if (wc)
      ast_add_child(node, wc);
  }

  if (kind == AST_PROTOCOL_DECL)
    parse_protocol_body(p, node);
  else if (kind == AST_ENUM_DECL)
    parse_enum_body(p, node);
  else if (P_LBRACE(p)) {
    ASTNode *body = parse_block(p);
    ast_add_child(node, body);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}
