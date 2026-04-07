/**
 * @file type.c
 * @brief Type expression parsing, parameter lists, generic params,
 *        where clauses, inheritance clauses, and protocol body.
 */
#include "private.h"

#define PROTO_REQ_IS_FUNC (1u << 23)

/* ═══════════════════════════════════════════════════════════════════════════════
 * Type Attribute & Prefix Helpers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Consumes @escaping, @autoclosure, @Sendable, etc. Returns modifier flags. */
static uint32_t consume_type_attributes(Parser *p) {
  uint32_t mods = 0;
  while (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
         p->src->data[p_tok(p)->pos] == '@') {
    adv(p);
    if (p_tok(p)->type == TOK_IDENTIFIER) {
      if (p_is_ident_str(p, CK_ESCAPING))       mods |= MOD_ESCAPING;
      else if (p_is_ident_str(p, CK_AUTOCLOSURE)) mods |= MOD_AUTOCLOSURE;
      adv(p);
    }
  }
  return mods;
}

/** @brief Parses `inout T` → AST_TYPE_INOUT wrapping the base type. */
static ASTNode *parse_type_inout(Parser *p, ASTNode *node) {
  node->kind = AST_TYPE_INOUT;
  adv(p);
  ASTNode *base = parse_type(p);
  if (base) ast_add_child(node, base);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Array / Dict Type: [T], [K: V], [N of T]
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Strips underscores and parses an integer literal (shared with prefix.c). */
static int64_t parse_int_value(const Parser *p, const Token *lit) {
  char buf[48];
  size_t len = lit->len < 47 ? lit->len : 47;
  memcpy(buf, p->src->data + lit->pos, len);
  buf[len] = '\0';
  /* strip underscores */
  char *w = buf;
  for (char *r = buf; *r; r++) if (*r != '_') *w++ = *r;
  *w = '\0';
  int base = 0;
  const char *start = buf;
  if (buf[0] == '0' && buf[1] == 'b')     { base = 2; start = buf + 2; }
  else if (buf[0] == '0' && buf[1] == 'o') { base = 8; start = buf + 2; }
  return (int64_t)strtoll(start, NULL, base);
}

/** @brief Parses `[T]`, `[K: V]`, or `[N of T]` into the pre-allocated node. */
static void parse_type_array_or_dict(Parser *p, ASTNode *node) {
  node->kind = AST_TYPE_ARRAY;
  adv(p); /* '[' */

  /* [N of T] — fixed-size array */
  if (p_tok(p)->type == TOK_INTEGER_LIT && p->pos + 1 < p->ts->count) {
    const Token *tok_of = &p->ts->tokens[p->pos + 1];
    if (tok_of->type == TOK_IDENTIFIER && tok_of->len == 2 &&
        p->src->data[tok_of->pos] == 'o' && p->src->data[tok_of->pos + 1] == 'f') {
      ASTNode *count_lit = alloc_node(p, AST_INTEGER_LITERAL);
      if (!count_lit) return;
      count_lit->data.integer.ival = parse_int_value(p, p_tok(p));
      adv(p); /* integer */
      adv(p); /* "of" */
      ASTNode *inner = parse_type(p);
      if (inner) { ast_add_child(node, count_lit); ast_add_child(node, inner); }
      if (P_RBRACK(p)) adv(p);
      return;
    }
  }

  /* [T] or [K: V] */
  ASTNode *inner = parse_type(p);
  if (inner) ast_add_child(node, inner);
  if (P_COLON(p)) {
    adv(p);
    node->kind = AST_TYPE_DICT;
    ASTNode *val = parse_type(p);
    if (val) ast_add_child(node, val);
  }
  if (P_RBRACK(p)) adv(p);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Tuple / Function Type: (T, U), (label: T) -> U
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Returns 1 if the next token is ':' (PUNCT or single-char OPERATOR). */
static int next_is_colon(const Parser *p) {
  if (p->pos + 1 >= p->ts->count) return 0;
  const Token *nt = &p->ts->tokens[p->pos + 1];
  return (nt->type == TOK_PUNCT && p->src->data[nt->pos] == ':') ||
         (nt->type == TOK_OPERATOR && nt->len == 1 && p->src->data[nt->pos] == ':');
}

/** @brief Parses one tuple element: [label :] Type */
static ASTNode *parse_tuple_element(Parser *p) {
  ASTNode *elem = alloc_node(p, AST_PARAM);
  if (!elem) return NULL;
  elem->data.var.name_tok = 0;

  /* Detect `label:` */
  if (p_tok(p)->type == TOK_IDENTIFIER && next_is_colon(p)) {
    elem->data.var.name_tok = (uint32_t)p->pos;
    adv(p); /* label */
    adv(p); /* ':' */
  }
  ASTNode *ty = parse_type(p);
  if (ty) ast_add_child(elem, ty);
  elem->tok_end = (uint32_t)p->pos;
  return elem;
}

/** @brief Parses `(T, U)` tuple or `(T) -> U` function type into node. */
static void parse_type_tuple_or_func(Parser *p, ASTNode *node) {
  node->kind = AST_TYPE_TUPLE;
  adv(p); /* '(' */

  while (!p_is_eof(p) && !P_RPAREN(p)) {
    ASTNode *elem = parse_tuple_element(p);
    if (elem) ast_add_child(node, elem);
    if (P_COMMA(p)) adv(p); else break;
  }
  if (P_RPAREN(p)) adv(p);

  /* async / throws */
  if (p_is_kw(p, KW_ASYNC)) { node->modifiers |= MOD_ASYNC; adv(p); }
  ASTNode *throws_clause = parse_throws_clause(p);
  if (throws_clause) {
    if (throws_clause_is_throwing(p->src, p->ts, throws_clause))
      node->modifiers |= MOD_THROWS;
    if (throws_clause->modifiers & MOD_RETHROWS)
      node->modifiers |= MOD_RETHROWS;
    ast_add_child(node, throws_clause);
  }

  /* -> ReturnType → function type */
  if (p_is_op(p, OP_ARROW)) {
    adv(p);
    node->kind = AST_TYPE_FUNC;
    ASTNode *ret = parse_type(p);
    if (ret) ast_add_child(node, ret);
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Named / Generic / Qualified Type: Foo, Foo<T>, Module.Type, T.Item
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses a named type with optional generic args and qualified suffix. */
static void parse_type_named(Parser *p, ASTNode *node) {
  node->tok_idx = (uint32_t)p->pos;
  adv(p); /* identifier */

  /* Generic args: Foo<T, U> */
  if (!p_is_eof(p) && cur_char(p) == '<' &&
      p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) {
    node->kind = AST_TYPE_GENERIC;
    adv(p); /* '<' */
    while (!p_is_eof(p) && cur_char(p) != '>') {
      ASTNode *arg = parse_type(p);
      if (arg) ast_add_child(node, arg);
      if (P_COMMA(p)) adv(p); else break;
    }
    if (!p_is_eof(p) && cur_char(p) == '>') adv(p);
  }

  /* Qualified: Module.Type or T.Item */
  while (p_is_punct(p, '.')) {
    adv(p);
    if (p_tok(p)->type == TOK_IDENTIFIER) {
      ASTNode *member = alloc_node(p, AST_TYPE_IDENT);
      if (!member) return;
      member->tok_idx = (uint32_t)p->pos;
      adv(p);
      ast_add_child(node, member);
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Suffix: P & Q, T?, T!
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Wraps node in protocol composition: P & Q & R */
static ASTNode *wrap_composition(Parser *p, ASTNode *node) {
  while (!p_is_eof(p) && p_tok(p)->type == TOK_OPERATOR &&
         p_tok(p)->len == 1 && p->src->data[p_tok(p)->pos] == '&') {
    adv(p);
    ASTNode *right = parse_type(p);
    if (!right) break;
    ASTNode *comp = alloc_node(p, AST_TYPE_COMPOSITION);
    if (!comp) return node;
    ast_add_child(comp, node);
    ast_add_child(comp, right);
    comp->tok_end = (uint32_t)p->pos;
    node = comp;
  }
  return node;
}

/** @brief Wraps node in AST_TYPE_OPTIONAL if followed by '?' or '!'. */
static ASTNode *wrap_optional(Parser *p, ASTNode *node) {
  if (p_is_eof(p)) return node;
  char c = p->src->data[p_tok(p)->pos];
  if (c != '?' && c != '!') return node;

  ASTNode *inner = node;
  inner->tok_end = (uint32_t)p->pos;
  node = alloc_node(p, AST_TYPE_OPTIONAL);
  if (!node) return inner;
  node->tok_idx = inner->tok_idx;
  ast_add_child(node, inner);
  adv(p);
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * parse_type — main entry point
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a Swift type expression.
 *
 * Handles: @escaping, some/any, inout, [T], [K:V], [N of T], (T,U),
 * (T)->U, generics, qualified names (A.B), P & Q, and T?/T!.
 */
ASTNode *parse_type(Parser *p) {
  ASTNode *node = alloc_node(p, AST_TYPE_IDENT);
  if (!node) return NULL;

  /* Type attributes: @escaping, @autoclosure */
  node->modifiers |= consume_type_attributes(p);

  /* some T / any T */
  if (p_is_kw(p, KW_SOME))      { node->kind = AST_TYPE_SOME; adv(p); }
  else if (p_is_kw(p, KW_ANY))  { node->kind = AST_TYPE_ANY;  adv(p); }

  /* inout T */
  if (p_is_ident_str(p, CK_INOUT))
    return parse_type_inout(p, node);

  /* [T], [K: V], [N of T] */
  if (P_LBRACK(p)) {
    parse_type_array_or_dict(p, node);
    node = wrap_composition(p, node);
    node = wrap_optional(p, node);
    node->tok_end = (uint32_t)p->pos;
    return node;
  }

  /* (T, U) or (T) -> U */
  if (P_LPAREN(p)) {
    parse_type_tuple_or_func(p, node);
    node->tok_end = (uint32_t)p->pos;
    return node;
  }

  /* Named type: Foo, Foo<T>, Module.Type */
  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD)
    parse_type_named(p, node);
  else
    adv(p); /* error recovery */

  node = wrap_composition(p, node);
  node = wrap_optional(p, node);
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Parameter List
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Tries to consume a borrowing/consuming ownership modifier. */
static void try_consume_ownership(Parser *p, ASTNode *param) {
  if (p_tok(p)->type != TOK_KEYWORD) return;
  uint32_t mod = 0;
  uint8_t kind = 0;
  if (p_tok(p)->keyword == KW_BORROWING)      { mod = MOD_BORROWING; kind = OWNERSHIP_BORROWING; }
  else if (p_tok(p)->keyword == KW_CONSUMING) { mod = MOD_CONSUMING; kind = OWNERSHIP_CONSUMING; }
  else return;

  ASTNode *own = alloc_node(p, AST_OWNERSHIP_SPEC);
  if (!own) return;
  own->tok_idx = (uint32_t)p->pos;
  own->data.aux.kind = kind;
  own->tok_end = (uint32_t)(p->pos + 1);
  ast_add_child(param, own);
  param->modifiers |= mod;
  adv(p);
}

/**
 * @brief Parses a parenthesized parameter list: (ext int: Type = default, ...).
 *
 * Handles external/internal labels, ownership modifiers, variadic `...`,
 * and default values.
 */
void parse_params(Parser *p, ASTNode *parent) {
  if (!P_LPAREN(p)) return;
  adv(p);

  while (!p_is_eof(p) && !P_RPAREN(p)) {
    size_t before = p->pos;
    ASTNode *param = alloc_node(p, AST_PARAM);
    if (!param) return;

    try_consume_ownership(p, param);

    /* External label (or sole name) */
    uint32_t ext_tok = 0;
    int has_ext = 0;
    if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD ||
        (p_tok(p)->type == TOK_OPERATOR && p->src->data[p_tok(p)->pos] == '_')) {
      ext_tok = (uint32_t)p->pos;
      has_ext = 1;
      adv(p);
    }

    /* Internal name (if present) */
    if (p_tok(p)->type == TOK_IDENTIFIER) {
      param->data.var.name_tok = (uint32_t)p->pos;
      adv(p);
    } else if (has_ext) {
      param->data.var.name_tok = ext_tok;
    }

    /* : Type */
    if (P_COLON(p)) {
      adv(p);
      ASTNode *tn = parse_type(p);
      if (tn) ast_add_child(param, tn);
    }

    /* Variadic: Type... */
    if (!p_is_eof(p) && p_tok(p)->op_kind == OP_RANGE_INCL) {
      param->modifiers |= MOD_VARIADIC;
      adv(p);
    }

    /* Default value: = expr */
    if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '=') {
      adv(p);
      ASTNode *def = parse_expr_pratt(p, 0);
      if (def) ast_add_child(param, def);
    }

    param->tok_end = (uint32_t)p->pos;
    ast_add_child(parent, param);
    if (P_COMMA(p)) adv(p);
    if (p->pos == before) adv(p); /* progress guard */
  }
  if (P_RPAREN(p)) adv(p);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Generic Parameters & Constraints
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Recursively flattens compositions into individual CONFORMANCE children. */
void add_generic_constraint_nodes(Parser *p, ASTNode *gp,
                                  ASTNode *constraint_type,
                                  int *suppressed_first) {
  if (!constraint_type) return;
  if (constraint_type->kind == AST_TYPE_COMPOSITION) {
    for (ASTNode *part = constraint_type->first_child; part;) {
      ASTNode *next = part->next_sibling;
      add_generic_constraint_nodes(p, gp, part, suppressed_first);
      part = next;
    }
    return;
  }
  ASTNode *c = alloc_node(p, AST_CONFORMANCE);
  if (!c) return;
  c->tok_idx = constraint_type->tok_idx;
  c->tok_end = constraint_type->tok_end;
  if (*suppressed_first) { c->modifiers |= MOD_SUPPRESSED_CONFORMANCE; *suppressed_first = 0; }
  ast_add_child(c, constraint_type);
  ast_add_child(gp, c);
}

/** @brief Parses: <T: Proto, U: Hashable & Comparable> */
void parse_generic_params(Parser *p, ASTNode *parent) {
  if (p_is_eof(p) || cur_char(p) != '<') return;
  adv(p);

  while (!p_is_eof(p) && cur_char(p) != '>') {
    if (p_tok(p)->type != TOK_IDENTIFIER) break;

    ASTNode *gp = alloc_node(p, AST_GENERIC_PARAM);
    if (!gp) return;
    gp->tok_idx = (uint32_t)p->pos;
    adv(p);

    if (P_COLON(p)) {
      adv(p);
      while (!p_is_eof(p)) {
        int suppressed = 0;
        if (p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1 &&
            p->src->data[p_tok(p)->pos] == '~') { suppressed = 1; adv(p); }
        ASTNode *ct = parse_type(p);
        if (ct) { int sf = suppressed; add_generic_constraint_nodes(p, gp, ct, &sf); }
        if (!p_is_eof(p) && cur_char(p) == '&' &&
            p_tok(p)->type == TOK_OPERATOR && p_tok(p)->len == 1) { adv(p); continue; }
        break;
      }
    }

    gp->tok_end = (uint32_t)p->pos;
    ast_add_child(parent, gp);
    if (P_COMMA(p)) adv(p); else break;
  }
  if (!p_is_eof(p) && cur_char(p) == '>') adv(p);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Where Clause & Inheritance Clause
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Parses: where T: Equatable, U == Int */
ASTNode *parse_where_clause(Parser *p) {
  if (!p_is_kw(p, KW_WHERE)) return NULL;

  ASTNode *wc = alloc_node(p, AST_WHERE_CLAUSE);
  if (!wc) return NULL;
  adv(p);

  while (!p_is_eof(p) && !P_LBRACE(p)) {
    ASTNode *lhs = parse_type(p);
    if (!lhs) break;

    ASTNode *req = alloc_node(p, AST_BINARY_EXPR);
    if (!req) return NULL;

    if (P_COLON(p)) {
      /* Conformance: T: Equatable */
      adv(p);
      req->data.binary.op_tok = 0;
    } else if (!p_is_eof(p) && p_tok(p)->op_kind == OP_EQ) {
      /* Same-type: T == Int */
      req->data.binary.op_tok = (uint32_t)(p->pos);
      adv(p);
    } else {
      ast_add_child(wc, lhs);
      if (P_COMMA(p)) adv(p); else break;
      continue;
    }

    ASTNode *rhs = parse_type(p);
    ast_add_child(req, lhs);
    if (rhs) ast_add_child(req, rhs);
    ast_add_child(wc, req);

    if (P_COMMA(p)) adv(p); else break;
  }

  wc->tok_end = (uint32_t)p->pos;
  return wc;
}

/** @brief Parses: : Proto1, Proto2, SuperClass */
ASTNode *parse_inheritance_clause(Parser *p) {
  if (!P_COLON(p)) return NULL;
  adv(p);

  ASTNode *conf = alloc_node(p, AST_CONFORMANCE);
  if (!conf) return NULL;

  while (!p_is_eof(p) && !P_LBRACE(p) && !p_is_kw(p, KW_WHERE)) {
    ASTNode *entry = parse_type(p);
    if (entry) ast_add_child(conf, entry);
    if (P_COMMA(p)) adv(p); else break;
  }

  conf->tok_end = (uint32_t)p->pos;
  return conf;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Protocol Body
 * ═══════════════════════════════════════════════════════════════════════════════ */

/** @brief Collects static/mutating modifiers in a protocol body. */
static uint32_t collect_proto_modifiers(Parser *p) {
  uint32_t mods = 0;
  while (!p_is_eof(p) && p_tok(p)->type == TOK_KEYWORD) {
    if (p_tok(p)->keyword == KW_STATIC)        { mods |= MOD_STATIC; adv(p); }
    else if (p_tok(p)->keyword == KW_MUTATING) { mods |= MOD_MUTATING; adv(p); }
    else break;
  }
  return mods;
}

/** @brief Parses a func requirement in a protocol body. */
static ASTNode *parse_proto_func_req(Parser *p, uint32_t mods) {
  ASTNode *req = alloc_node(p, AST_PROTOCOL_REQ);
  if (!req) return NULL;
  adv(p); /* 'func' */
  req->tok_idx = (uint32_t)p->pos;
  req->modifiers = mods | PROTO_REQ_IS_FUNC;

  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_OPERATOR)
    adv(p);
  if (!p_is_eof(p) && cur_char(p) == '<')
    parse_generic_params(p, req);
  parse_params(p, req);

  if (p_is_kw(p, KW_ASYNC)) adv(p);
  ASTNode *tc = parse_throws_clause(p);
  if (tc) {
    if (throws_clause_is_throwing(p->src, p->ts, tc)) req->modifiers |= MOD_THROWS;
    if (tc->modifiers & MOD_RETHROWS) req->modifiers |= MOD_RETHROWS;
    ast_add_child(req, tc);
  }
  if (p_is_op(p, OP_ARROW)) {
    adv(p);
    ASTNode *ret = parse_type(p);
    if (ret) ast_add_child(req, ret);
  }
  if (p_is_kw(p, KW_WHERE)) {
    ASTNode *wc = parse_where_clause(p);
    if (wc) ast_add_child(req, wc);
  }
  if (P_LBRACE(p)) ast_add_child(req, parse_block(p));
  return req;
}

/** @brief Parses a var requirement: var name: Type { get [set] } */
static ASTNode *parse_proto_var_req(Parser *p, uint32_t mods) {
  ASTNode *req = alloc_node(p, AST_PROTOCOL_REQ);
  if (!req) return NULL;
  adv(p); /* 'var' */
  req->tok_idx = (uint32_t)p->pos;
  req->modifiers = mods;

  if (p_tok(p)->type == TOK_IDENTIFIER) adv(p);
  if (P_COLON(p)) {
    adv(p);
    ASTNode *tn = parse_type(p);
    if (tn) ast_add_child(req, tn);
  }
  if (P_LBRACE(p)) {
    adv(p);
    while (!p_is_eof(p) && !P_RBRACE(p)) {
      if (p_tok(p)->type == TOK_IDENTIFIER && tok_eq(p, p_tok(p), CK_SET))
        { req->modifiers |= MOD_PROTOCOL_PROP_SET; adv(p); break; }
      adv(p);
    }
    if (P_RBRACE(p)) adv(p);
  }
  return req;
}

/** @brief Parses associatedtype or typealias in a protocol body. */
static ASTNode *parse_proto_assoc_req(Parser *p, uint32_t mods) {
  ASTNode *req = alloc_node(p, AST_PROTOCOL_REQ);
  if (!req) return NULL;
  adv(p); /* 'associatedtype' or 'typealias' */
  req->tok_idx = (uint32_t)p->pos;
  req->modifiers = mods | MOD_PROTOCOL_ASSOC_TYPE;

  if (p_tok(p)->type == TOK_IDENTIFIER) adv(p);
  if (P_COLON(p)) { adv(p); ASTNode *ct = parse_type(p); if (ct) ast_add_child(req, ct); }
  if (!p_is_eof(p) && cur_char(p) == '=') { adv(p); ASTNode *dt = parse_type(p); if (dt) ast_add_child(req, dt); }
  return req;
}

/** @brief Parses an init requirement in a protocol body. */
static ASTNode *parse_proto_init_req(Parser *p, uint32_t mods) {
  ASTNode *req = alloc_node(p, AST_PROTOCOL_REQ);
  if (!req) return NULL;
  req->tok_idx = (uint32_t)p->pos;
  req->modifiers = mods;
  adv(p); /* 'init' */

  if (!p_is_eof(p) && cur_char(p) == '?') { adv(p); req->modifiers |= MOD_FAILABLE; }
  if (!p_is_eof(p) && cur_char(p) == '!') { adv(p); req->modifiers |= MOD_IMPLICITLY_UNWRAPPED_FAILABLE; }
  parse_params(p, req);

  ASTNode *tc = parse_throws_clause(p);
  if (tc) {
    if (throws_clause_is_throwing(p->src, p->ts, tc)) req->modifiers |= MOD_THROWS;
    if (tc->modifiers & MOD_RETHROWS) req->modifiers |= MOD_RETHROWS;
    ast_add_child(req, tc);
  }
  return req;
}

/** @brief Parses a protocol body: { func...; var...; associatedtype...; init...; } */
void parse_protocol_body(Parser *p, ASTNode *proto) {
  if (!P_LBRACE(p)) return;
  adv(p);

  while (!p_is_eof(p) && !P_RBRACE(p)) {
    size_t before = p->pos;
    uint32_t mods = collect_proto_modifiers(p);

    ASTNode *req = NULL;
    if (p_is_kw(p, KW_FUNC))
      req = parse_proto_func_req(p, mods);
    else if (p_is_kw(p, KW_VAR))
      req = parse_proto_var_req(p, mods);
    else if (p_is_kw(p, KW_TYPEALIAS) || p_is_ident_str(p, CK_ASSOC_TYPE))
      req = parse_proto_assoc_req(p, mods);
    else if (p_is_kw(p, KW_INIT))
      req = parse_proto_init_req(p, mods);
    else
      adv(p);

    if (req) { req->tok_end = (uint32_t)p->pos; ast_add_child(proto, req); }
    if (p->pos == before) adv(p);
  }
  if (P_RBRACE(p)) adv(p);
}
