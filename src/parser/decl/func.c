/**
 * @file func.c
 * @brief Function-like declaration parsers: func, init, deinit, subscript.
 *
 * All four follow the same general pattern:
 *   keyword [name] [<generics>] (params) [async] [throws] [-> RetType] [where ...] { body }
 *
 * Functions:
 *   parse_func_decl()      — func name<T>(params) async throws -> Ret { }
 *   parse_init_decl()      — init?/init!<T>(params) throws { }
 *   parse_deinit_decl()    — deinit { }
 *   parse_subscript_decl() — subscript<T>(index: Int) -> Elem { get/set }
 */
#include "../private.h"

/**
 * @brief Parses a function declaration.
 *
 * Full syntax:
 * @code
 *   func name<T>(params) async throws -> ReturnType where T: P { body }
 *         |   |    |       |     |         |           |          |
 *         1   2    3       4     5         6           7          8
 * @endcode
 *
 * Parsing steps:
 *   1. Name — identifier or operator (func + or func ==)
 *   2. Generic parameters — <T: Comparable, U>
 *   3. Parameter list — (label name: Type, ...)
 *   4. async modifier
 *   5. throws / rethrows / throws(ErrorType)
 *   6. Return type — -> Type
 *   7. where clause — where T: Hashable
 *   8. Body — { ... } (optional for protocol requirements)
 *
 * The name can be an operator token (func +), which is why both
 * TOK_IDENTIFIER and TOK_OPERATOR are accepted at step 1.
 *
 * @param p     Parser state (pos at `func` keyword).
 * @param mods  Modifier bitmask from collect_modifiers().
 * @return      AST_FUNC_DECL node with children: params, throws clause,
 *              return type, where clause, body.
 */
ASTNode *parse_func_decl(Parser *p, uint32_t mods) {
  adv(p); /* func */
  ASTNode *node = alloc_node(p, AST_FUNC_DECL);
  if (!node)
    return NULL;
  node->modifiers = mods;

  /* 1. Name (identifier or operator) */
  node->data.func.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_OPERATOR)
    adv(p);

  /* 2. Generic parameters: <T, U: Proto> */
  if (!p_is_eof(p) && cur_char(p) == '<')
    parse_generic_params(p, node);

  /* 3. Parameter list: (label name: Type, ...) */
  parse_params(p, node);

  /* 4. async modifier */
  if (p_is_kw(p, KW_ASYNC)) {
    node->modifiers |= MOD_ASYNC;
    adv(p);
  }

  /* 5. throws / rethrows / throws(ErrorType) */
  ASTNode *throws_clause = parse_throws_clause(p);
  if (throws_clause) {
    if (throws_clause_is_throwing(p->src, p->ts, throws_clause))
      node->modifiers |= MOD_THROWS;
    if (throws_clause->modifiers & MOD_RETHROWS)
      node->modifiers |= MOD_RETHROWS;
    ast_add_child(node, throws_clause);
  }
  if (p_is_kw(p, KW_RETHROWS)) {
    node->modifiers |= MOD_RETHROWS;
    adv(p);
  }

  /* 6. Return type: -> Type */
  if (p_is_op(p, OP_ARROW)) {
    adv(p);
    ASTNode *ret = parse_type(p);
    if (ret)
      ast_add_child(node, ret);
  }

  /* 7. where clause: where T: Hashable */
  if (p_is_kw(p, KW_WHERE)) {
    ASTNode *wc = parse_where_clause(p);
    if (wc)
      ast_add_child(node, wc);
  }

  /* 8. Body (optional — protocol requirements have no body) */
  if (P_LBRACE(p)) {
    ASTNode *body = parse_block(p);
    ast_add_child(node, body);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/**
 * @brief Parses a subscript declaration.
 *
 * Full syntax:
 * @code
 *   subscript<T>(index: Int) -> Element { get { } set { } }
 *            |       |            |         |
 *            1       2            3         4
 * @endcode
 *
 * Parsing steps:
 *   1. Generic parameters (optional)
 *   2. Parameter list — subscript parameters act like function params
 *   3. Return type — always present (what the subscript yields)
 *   4. Body — two forms:
 *      a) Accessor block: { get { } set { } } — uses parse_computed_property()
 *      b) Shorthand getter: { return expr } — treated as implicit get-only
 *
 * To distinguish (4a) from (4b), we peek past '{' and check for accessor
 * keywords (get/set/_read/_modify).  If none found, backtrack and parse
 * the whole block as a shorthand getter.
 *
 * @param p     Parser state (pos at `subscript` keyword).
 * @param mods  Modifier bitmask from collect_modifiers().
 * @return      AST_SUBSCRIPT_DECL node.
 */
ASTNode *parse_subscript_decl(Parser *p, uint32_t mods) {
  adv(p); /* subscript */
  ASTNode *node = alloc_node(p, AST_SUBSCRIPT_DECL);
  if (!node)
    return NULL;
  node->modifiers = mods;

  /* 1. Generic parameters: <T> */
  if (!p_is_eof(p) && cur_char(p) == '<')
    parse_generic_params(p, node);

  /* 2. Parameter list: (index: Int) */
  parse_params(p, node);

  /* 3. Return type: -> Element */
  if (p_is_op(p, OP_ARROW)) {
    adv(p);
    ASTNode *ret = parse_type(p);
    if (ret)
      ast_add_child(node, ret);
  }

  /* 4. Body: accessor block or shorthand getter */
  if (P_LBRACE(p)) {
    size_t saved = p->pos;
    adv(p); /* peek past '{' */
    if (!p_is_eof(p) &&
        (p_is_ck(p, CK_GET) ||
         p_is_ck(p, CK_SET) ||
         p_is_ck(p, CK_READ) ||
         p_is_ck(p, CK_MODIFY))) {
      /* 4a. Accessor block: { get { } set(newValue) { } } */
      parse_computed_property(p, node);
    } else {
      /* 4b. Shorthand getter: { return expr } — backtrack and parse as block */
      p->pos = saved;
      node->data.var.has_getter = 1;
      node->data.var.getter_body = parse_block(p);
    }
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/**
 * @brief Parses an initializer declaration.
 *
 * Full syntax:
 * @code
 *   init?<T>(params) async throws(ErrorType) { body }
 *       | |    |       |      |                 |
 *       1 2    3       4      5                 6
 * @endcode
 *
 * Parsing steps:
 *   1. Failable marker — `?` (returns nil) or `!` (implicitly unwrapped)
 *   2. Generic parameters — <T: Proto>
 *   3. Parameter list — (label name: Type, ...)
 *   4. async modifier
 *   5. throws / rethrows / throws(ErrorType)
 *   6. Body — { ... }
 *
 * Unlike func, init has no name token (it's always "init") and no return
 * type (always Self).  The failable markers `?` and `!` are stored as
 * modifier flags (MOD_FAILABLE, MOD_IMPLICITLY_UNWRAPPED_FAILABLE).
 *
 * @param p     Parser state (pos at `init` keyword).
 * @param mods  Modifier bitmask from collect_modifiers().
 * @return      AST_INIT_DECL node.
 */
ASTNode *parse_init_decl(Parser *p, uint32_t mods) {
  adv(p); /* init */
  ASTNode *node = alloc_node(p, AST_INIT_DECL);
  if (!node)
    return NULL;
  node->modifiers = mods;

  /* 1. Failable: init? or init! */
  if (!p_is_eof(p) && cur_char(p) == '?') {
    node->modifiers |= MOD_FAILABLE;
    adv(p);
  } else if (!p_is_eof(p) && cur_char(p) == '!') {
    node->modifiers |= MOD_FAILABLE;
    node->modifiers |= MOD_IMPLICITLY_UNWRAPPED_FAILABLE;
    adv(p);
  }

  /* 2. Generic parameters */
  if (!p_is_eof(p) && cur_char(p) == '<')
    parse_generic_params(p, node);

  /* 3. Parameter list */
  parse_params(p, node);

  /* 4. async */
  if (p_is_kw(p, KW_ASYNC)) {
    node->modifiers |= MOD_ASYNC;
    adv(p);
  }

  /* 5. throws / rethrows / throws(ErrorType) */
  ASTNode *throws_clause = parse_throws_clause(p);
  if (throws_clause) {
    if (throws_clause_is_throwing(p->src, p->ts, throws_clause))
      node->modifiers |= MOD_THROWS;
    if (throws_clause->modifiers & MOD_RETHROWS)
      node->modifiers |= MOD_RETHROWS;
    ast_add_child(node, throws_clause);
  }

  /* 6. Body */
  if (P_LBRACE(p)) {
    ASTNode *body = parse_block(p);
    ast_add_child(node, body);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}

/**
 * @brief Parses a deinitializer declaration.
 *
 * Syntax: `deinit { body }`
 *
 * The simplest function-like declaration — no name, no parameters, no
 * return type, no generics, no throws.  Just a keyword and a body.
 * Only valid inside class and actor declarations.
 *
 * @param p  Parser state (pos at `deinit` keyword).
 * @return   AST_DEINIT_DECL node with body as child.
 */
ASTNode *parse_deinit_decl(Parser *p) {
  adv(p); /* deinit */
  ASTNode *node = alloc_node(p, AST_DEINIT_DECL);
  if (!node)
    return NULL;
  if (P_LBRACE(p)) {
    ASTNode *body = parse_block(p);
    ast_add_child(node, body);
  }
  node->tok_end = (uint32_t)p->pos;
  return node;
}
