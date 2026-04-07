/**
 * @file prefix.c
 * @brief Prefix and primary expression parsing: literals, identifiers,
 *        unary operators, keywords, collections, closures, key paths, macros.
 */
#include "../private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief True if the current token is a single-char '.' (PUNCT or OPERATOR). */
static int p_is_dot(const Parser *p) {
  if (p_is_punct(p, '.')) return 1;
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR && t->len == 1 && p->src->data[t->pos] == '.';
}

/** @brief True if the current token is a single-char '#' operator. */
static int p_is_hash(const Parser *p) {
  const Token *t = p_tok(p);
  return t->type == TOK_OPERATOR && t->len == 1 && p->src->data[t->pos] == '#';
}

/** @brief Allocates an IDENT_EXPR, consumes one token, and wraps in postfix. */
static ASTNode *parse_ident_postfix(Parser *p) {
  ASTNode *n = alloc_node(p, AST_IDENT_EXPR);
  if (!n) return NULL;
  adv(p);
  n->tok_end = (uint32_t)p->pos;
  return parse_postfix(p, n);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Keyword Expressions
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses keyword-initiated expressions: if, switch, try, await, consume.
 *
 * try? and try! are distinguished via MOD_WEAK / MOD_THROWS flags.
 */
static ASTNode *parse_prefix_keyword(Parser *p) {
  if (p_is_kw(p, KW_IF)) return parse_if(p);
  if (p_is_kw(p, KW_SWITCH)) return parse_switch(p);

  if (p_is_kw(p, KW_TRY)) {
    adv(p);
    ASTNode *node = alloc_node(p, AST_TRY_EXPR);
    if (!node) return NULL;
    if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '?') {
      node->modifiers |= MOD_WEAK; adv(p);
    } else if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '!') {
      node->modifiers |= MOD_THROWS; adv(p);
    }
    ASTNode *inner = parse_prefix(p);
    if (inner) ast_add_child(node, inner);
    node->tok_end = (uint32_t)p->pos;
    return node;
  }

  if (p_is_kw(p, KW_AWAIT)) {
    adv(p);
    ASTNode *node = alloc_node(p, AST_AWAIT_EXPR);
    if (!node) return NULL;
    ASTNode *inner = parse_prefix(p);
    if (inner) ast_add_child(node, inner);
    node->tok_end = (uint32_t)p->pos;
    return node;
  }

  if (p_is_kw(p, KW_CONSUME)) {
    adv(p);
    ASTNode *node = alloc_node(p, AST_CONSUME_EXPR);
    if (!node) return NULL;
    ASTNode *inner = parse_prefix(p);
    if (inner) ast_add_child(node, inner);
    node->tok_end = (uint32_t)p->pos;
    return parse_postfix(p, node);
  }

  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Unary Prefix Operators
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses unary prefix operators: !, ~, -, +, and inout (&). */
static ASTNode *parse_prefix_unary(Parser *p) {
  if (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) {
    char op = p->src->data[p_tok(p)->pos];
    if (op == '!' || op == '~' || op == '-' || op == '+') {
      ASTNode *node = alloc_node(p, AST_UNARY_EXPR);
      if (!node) return NULL;
      node->data.binary.op_tok = (uint32_t)p->pos;
      adv(p);
      ASTNode *inner = parse_prefix(p);
      if (inner) ast_add_child(node, inner);
      node->tok_end = (uint32_t)p->pos;
      return parse_postfix(p, node);
    }
    if (op == '&') {
      ASTNode *node = alloc_node(p, AST_INOUT_EXPR);
      if (!node) return NULL;
      adv(p);
      ASTNode *inner = parse_prefix(p);
      if (inner) ast_add_child(node, inner);
      node->tok_end = (uint32_t)p->pos;
      return node;
    }
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Literals
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Strips underscore separators from a numeric string in-place. */
static void strip_underscores(char *buf) {
  char *w = buf;
  for (char *r = buf; *r; r++)
    if (*r != '_') *w++ = *r;
  *w = '\0';
}

/** @brief Allocates a literal node, consumes the token, wraps in postfix. */
static ASTNode *make_literal(Parser *p, ASTNodeKind kind) {
  ASTNode *n = alloc_node(p, kind);
  if (!n) return NULL;
  adv(p);
  n->tok_end = (uint32_t)p->pos;
  return parse_postfix(p, n);
}

/** @brief Copies token text into buf (max 47 chars) and strips underscores. */
static void read_number_text(const Parser *p, const Token *lit, char *buf) {
  size_t len = lit->len < 47 ? lit->len : 47;
  memcpy(buf, p->src->data + lit->pos, len);
  buf[len] = '\0';
  strip_underscores(buf);
}

/**
 * @brief Parses literal expressions: true, false, nil, integers, floats, strings, regex.
 *
 * Integer literals handle 0b (binary), 0o (octal), 0x (hex) prefixes and
 * underscore separators (1_000_000).
 */
static ASTNode *parse_prefix_literal(Parser *p) {
  /* true / false */
  if (p_is_kw(p, KW_TRUE) || p_is_kw(p, KW_FALSE)) {
    int val = p_is_kw(p, KW_TRUE);
    ASTNode *n = make_literal(p, AST_BOOL_LITERAL);
    if (n) n->data.boolean.bval = val;
    return n;
  }
  /* nil */
  if (p_is_kw(p, KW_NIL))
    return make_literal(p, AST_NIL_LITERAL);

  /* 42, 0xFF, 0b1010, 1_000_000 */
  if (p_tok(p)->type == TOK_INTEGER_LIT) {
    char buf[48];
    read_number_text(p, p_tok(p), buf);
    int base = 0;
    const char *start = buf;
    if (buf[0] == '0' && buf[1] == 'b')     { base = 2; start = buf + 2; }
    else if (buf[0] == '0' && buf[1] == 'o') { base = 8; start = buf + 2; }
    ASTNode *n = make_literal(p, AST_INTEGER_LITERAL);
    if (n) n->data.integer.ival = (int64_t)strtoll(start, NULL, base);
    return n;
  }
  /* 3.14, 1_000.5 */
  if (p_tok(p)->type == TOK_FLOAT_LIT) {
    char buf[48];
    read_number_text(p, p_tok(p), buf);
    ASTNode *n = make_literal(p, AST_FLOAT_LITERAL);
    if (n) n->data.flt.fval = strtod(buf, NULL);
    return n;
  }
  /* "hello" */
  if (p_tok(p)->type == TOK_STRING_LIT)
    return make_literal(p, AST_STRING_LITERAL);

  /* /pattern/ */
  if (p_tok(p)->type == TOK_REGEX_LIT)
    return make_literal(p, AST_REGEX_LITERAL);

  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Special Primary Expressions
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses a key path expression: \Type.property or \.property. */
static ASTNode *parse_key_path(Parser *p) {
  uint32_t slash_tok = (uint32_t)p->pos;
  adv(p); /* '\\' */
  ASTNode *kp = alloc_node(p, AST_KEY_PATH_EXPR);
  if (!kp) return NULL;
  kp->tok_idx = slash_tok;

  /* \.property — inferred root type */
  if (!p_is_dot(p)) {
    ASTNode *type_node = parse_type(p);
    if (type_node) ast_add_child(kp, type_node);
  }
  if (!p_is_eof(p) && p_is_dot(p)) {
    adv(p);
    kp->data.var.name_tok = (uint32_t)p->pos;
    if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
      adv(p);
    kp->tok_end = (uint32_t)p->pos;
    return parse_postfix(p, kp);
  }
  return NULL;
}

/** @brief Parses #available(...) / #unavailable(...) argument list. */
static void parse_availability_args(Parser *p, ASTNode *n) {
  while (!p_is_eof(p) && !P_RPAREN(p)) {
    size_t before = p->pos;
    ASTNode *arg = parse_expr_pratt(p, 0);
    if (arg) {
      ast_add_child(n, arg);
    } else if (p->pos == before) {
      /* Raw token (e.g. *, iOS, 15.0) — wrap as ident */
      ASTNode *raw = alloc_node(p, AST_IDENT_EXPR);
      if (!raw) return;
      raw->tok_idx = (uint32_t)p->pos;
      raw->tok_end = (uint32_t)(p->pos + 1);
      adv(p);
      ast_add_child(n, raw);
    }
    if (P_COMMA(p)) adv(p);
  }
}

/** @brief Parses a hash expression: #available(...), #selector(...), #fileLiteral, etc. */
static ASTNode *parse_hash_expr(Parser *p) {
  adv(p); /* '#' */
  uint32_t name_tok = (uint32_t)p->pos;

  /* Read directive name */
  char dir_name[32] = "";
  if (!p_is_eof(p) && (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)) {
    size_t dlen = p_tok(p)->len < 31 ? p_tok(p)->len : 31;
    memcpy(dir_name, p->src->data + p_tok(p)->pos, dlen);
    dir_name[dlen] = '\0';
    adv(p);
  }

  int is_availability = (strcmp(dir_name, "available") == 0 ||
                         strcmp(dir_name, "unavailable") == 0);
  ASTNode *n = alloc_node(p, is_availability ? AST_AVAILABILITY_EXPR : AST_MACRO_EXPANSION);
  if (!n) return NULL;
  n->tok_idx = name_tok;
  n->data.aux.name_tok = name_tok;
  if (is_availability)
    n->data.aux.kind = (strcmp(dir_name, "unavailable") == 0)
                           ? AVAILABILITY_UNAVAILABLE : AVAILABILITY_AVAILABLE;

  /* Argument list */
  if (P_LPAREN(p)) {
    adv(p);
    if (is_availability)
      parse_availability_args(p, n);
    else
      parse_arg_list(p, n, ')');
    if (P_RPAREN(p)) adv(p);
  }

  n->tok_end = (uint32_t)p->pos;
  return parse_postfix(p, n);
}

/** @brief Parses implicit member expression: .enumCase, .member. */
static ASTNode *parse_implicit_member(Parser *p) {
  ASTNode *dot = alloc_node(p, AST_MEMBER_EXPR);
  if (!dot) return NULL;
  adv(p); /* '.' */
  dot->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
    adv(p);
  dot->tok_end = (uint32_t)p->pos;
  return parse_postfix(p, dot);
}

/**
 * @brief Parses special primary expressions: self, super, identifiers,
 *        key paths, hash directives, implicit members, operators-as-identifiers.
 */
static ASTNode *parse_prefix_special(Parser *p) {
  /* self, super, identifiers */
  if (p_is_kw(p, KW_SELF) || p_is_kw(p, KW_SUPER) ||
      p_tok(p)->type == TOK_IDENTIFIER)
    return parse_ident_postfix(p);

  /* Key path: \Type.property or \.property */
  if (p_is_punct(p, '\\'))
    return parse_key_path(p);

  /* Hash expression: #available(...), #selector(...), etc. */
  if (p_is_hash(p))
    return parse_hash_expr(p);

  /* Implicit member: .enumCase */
  if (p_is_dot(p))
    return parse_implicit_member(p);

  /* Operator-as-identifier: sorted(by: <), reduce(0, +).
     Only when followed by a closing delimiter. */
  if (p_tok(p)->type == TOK_OPERATOR) {
    uint32_t next_pos = p->pos + 1;
    if (next_pos < p->ts->count) {
      const Token *nx = &p->ts->tokens[next_pos];
      if (nx->type == TOK_PUNCT &&
          (p->src->data[nx->pos] == ')' || p->src->data[nx->pos] == ']' ||
           p->src->data[nx->pos] == ','))
        return parse_ident_postfix(p);
    }
  }

  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Collections & Grouping
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses collection/grouping expressions: (expr), (a, b) tuples,
 *        [elements] arrays, [k:v] dicts, and { closure } bodies.
 *
 * Parenthesized expressions with 2+ elements are promoted to AST_TUPLE_EXPR.
 * Array literals are promoted to AST_DICT_LITERAL when a ':' is found.
 */
static ASTNode *parse_prefix_collection(Parser *p) {
  if (P_LPAREN(p)) {
    ASTNode *paren = alloc_node(p, AST_PAREN_EXPR);
    if (!paren) return NULL;
    adv(p);
    parse_arg_list(p, paren, ')');
    if (P_RPAREN(p)) adv(p);
    int child_count = 0;
    for (ASTNode *c = paren->first_child; c; c = c->next_sibling) child_count++;
    if (child_count >= 2) paren->kind = AST_TUPLE_EXPR;
    paren->tok_end = (uint32_t)p->pos;
    return parse_postfix(p, paren);
  }
  if (P_LBRACK(p)) {
    ASTNode *arr = alloc_node(p, AST_ARRAY_LITERAL);
    if (!arr) return NULL;
    adv(p);
    while (!p_is_eof(p) && !P_RBRACK(p)) {
      size_t before = p->pos;
      ASTNode *e = parse_expr_pratt(p, 0);
      if (e) {
        if (P_COLON(p)) {
          arr->kind = AST_DICT_LITERAL;
          adv(p);
          ASTNode *v = parse_expr_pratt(p, 0);
          /* key and value as siblings so sema iterates: k1, v1, k2, v2, ... */
          ast_add_child(arr, e);
          if (v) ast_add_child(arr, v);
        } else {
          ast_add_child(arr, e);
        }
      } else if (p->pos == before) {
        adv(p);
      }
      if (P_COMMA(p)) adv(p);
    }
    if (P_RBRACK(p)) adv(p);
    arr->tok_end = (uint32_t)p->pos;
    return parse_postfix(p, arr);
  }
  if (P_LBRACE(p)) {
    return parse_closure_body(p);
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Entry Point
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Entry point for prefix expression parsing.
 *
 * Tries each prefix form in priority order: keywords, unary operators,
 * literals, special expressions, and collections.  Returns NULL if no
 * prefix expression matches (e.g. at a statement boundary).
 */
ASTNode *parse_prefix(Parser *p) {
  if (p_is_eof(p)) return NULL;

  ASTNode *node = NULL;

  if ((node = parse_prefix_keyword(p))) return node;
  if ((node = parse_prefix_unary(p))) return node;
  if ((node = parse_prefix_literal(p))) return node;
  if ((node = parse_prefix_special(p))) return node;
  if ((node = parse_prefix_collection(p))) return node;

  return NULL;
}
