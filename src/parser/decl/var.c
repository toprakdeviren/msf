/**
 * @file var.c
 * @brief Variable and property declaration parsers.
 *
 * Handles the full range of Swift property declarations:
 *   - Simple stored: var x: Int = 42
 *   - Computed: var x: Int { get { } set { } }
 *   - Shorthand getter: var x: Int { return expr }
 *   - Observers: var x: Int = 0 { willSet { } didSet { } }
 *   - Multi-var: var x = 0, y = 0 (chained via next_sibling)
 *   - class var (MOD_STATIC + class_var_flag)
 *
 * Also contains the accessor parsers shared by var and subscript:
 *   parse_computed_property()    — get/set/_read/_modify
 *   parse_property_observers()  — willSet/didSet
 */
#include "../private.h"

/* ═══════════════════════════════════════════════════════════════════════════════
 * Computed Properties & Property Observers
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses computed property accessors: get, set, _read, _modify.
 *
 * Expects the opening '{' to already be consumed.  Each accessor with a
 * body produces an AST_ACCESSOR_DECL child node.  set and _modify
 * support optional parameter names: set(newValue) { ... }.
 */
void parse_computed_property(Parser *p, ASTNode *node) {
  node->data.var.is_computed = 1;
  while (!p_is_eof(p) && !P_RBRACE(p)) {
    uint32_t acc_tok = (uint32_t)p->pos;
    if (p_is_ck(p, CK_GET)) {
      adv(p);
      node->data.var.has_getter = 1;
      if (P_LBRACE(p)) {
        node->data.var.getter_body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_GET;
        ast_add_child(acc, node->data.var.getter_body);
        acc->tok_end = node->data.var.getter_body->tok_end;
        ast_add_child(node, acc);
      }
    } else if (p_is_ck(p, CK_SET)) {
      adv(p);
      node->data.var.has_setter = 1;
      node->data.var.setter_param_name_tok = parse_optional_param_name(p);
      if (P_LBRACE(p)) {
        node->data.var.setter_body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_SET;
        acc->data.aux.name_tok = node->data.var.setter_param_name_tok;
        ast_add_child(acc, node->data.var.setter_body);
        acc->tok_end = node->data.var.setter_body->tok_end;
        ast_add_child(node, acc);
      }
    } else if (p_is_ck(p, CK_READ)) {
      adv(p);
      if (P_LBRACE(p)) {
        ASTNode *body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_READ;
        ast_add_child(acc, body);
        acc->tok_end = body->tok_end;
        ast_add_child(node, acc);
      }
    } else if (p_is_ck(p, CK_MODIFY)) {
      adv(p);
      if (P_LBRACE(p)) {
        ASTNode *body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_MODIFY;
        ast_add_child(acc, body);
        acc->tok_end = body->tok_end;
        ast_add_child(node, acc);
      }
    } else {
      adv(p);
    }
  }
  if (P_RBRACE(p))
    adv(p);
}

/**
 * @brief Parses property observers: willSet and didSet.
 *
 * Both support optional parameter names: willSet(newVal) { ... }.
 * Each observer produces an AST_ACCESSOR_DECL child node.
 */
void parse_property_observers(Parser *p, ASTNode *node) {
  while (!p_is_eof(p) && !P_RBRACE(p)) {
    uint32_t acc_tok = (uint32_t)p->pos;
    if (p_is_ck(p, CK_WILL_SET)) {
      adv(p);
      node->data.var.has_will_set = 1;
      node->data.var.will_set_param_name_tok = parse_optional_param_name(p);
      if (P_LBRACE(p)) {
        node->data.var.will_set_body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_WILL_SET;
        acc->data.aux.name_tok = node->data.var.will_set_param_name_tok;
        ast_add_child(acc, node->data.var.will_set_body);
        acc->tok_end = node->data.var.will_set_body->tok_end;
        ast_add_child(node, acc);
      }
    } else if (p_is_ck(p, CK_DID_SET)) {
      adv(p);
      node->data.var.has_did_set = 1;
      node->data.var.did_set_param_name_tok = parse_optional_param_name(p);
      if (P_LBRACE(p)) {
        node->data.var.did_set_body = parse_block(p);
        ASTNode *acc = alloc_node(p, AST_ACCESSOR_DECL);
        if (!acc) return;
        acc->tok_idx = acc_tok;
        acc->data.aux.kind = ACCESSOR_DID_SET;
        acc->data.aux.name_tok = node->data.var.did_set_param_name_tok;
        ast_add_child(acc, node->data.var.did_set_body);
        acc->tok_end = node->data.var.did_set_body->tok_end;
        ast_add_child(node, acc);
      }
    } else {
      adv(p);
    }
  }
  if (P_RBRACE(p))
    adv(p);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Variable / Constant Declarations
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Parses a var or let declaration.
 *
 * After parsing the name, type annotation, and initializer, checks for a
 * braced body to determine if this is a computed property, observer, or
 * shorthand getter.
 *
 * Multi-var declarations (var x = 0, y = 0) chain subsequent decls via
 * next_sibling — the caller (parse_block) walks the chain.
 *
 * @param p       Parser state (pos at var/let keyword).
 * @param is_let  1 for let, 0 for var.
 * @param mods    Modifier bitmask from collect_modifiers().
 * @return        First declaration node (extras chained via next_sibling).
 */
ASTNode *parse_var_decl(Parser *p, int is_let, uint32_t mods) {
  adv(p);
  ASTNode *node = alloc_node(p, is_let ? AST_LET_DECL : AST_VAR_DECL);
  if (!node)
    return NULL;
  node->modifiers = mods;
  node->data.var.setter_access = p->setter_access;
  if ((mods & MOD_STATIC) && p->class_var_flag) {
    node->data.var.is_class_var = 1;
    p->class_var_flag = 0;
  }
  node->data.var.name_tok = (uint32_t)p->pos;
  if (p_tok(p)->type == TOK_IDENTIFIER)
    adv(p);

  /* : Type */
  if (P_COLON(p)) {
    adv(p);
    ASTNode *tn = parse_type(p);
    if (tn)
      ast_add_child(node, tn);
  }

  /* = expr */
  if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '=') {
    adv(p);
    ASTNode *init = parse_expr_pratt(p, 0);
    if (init)
      ast_add_child(node, init);
  }

  /* Braced body: computed property / observer / shorthand getter */
  if (P_LBRACE(p)) {
    size_t before_brace = p->pos;
    adv(p);
    if (!p_is_eof(p)) {
      if (p_is_ck(p, CK_GET) ||
          p_is_ck(p, CK_SET) ||
          p_is_ck(p, CK_READ) ||
          p_is_ck(p, CK_MODIFY)) {
        parse_computed_property(p, node);
      } else if (p_is_ck(p, CK_WILL_SET) ||
                 p_is_ck(p, CK_DID_SET)) {
        parse_property_observers(p, node);
      } else {
        /* Shorthand getter: var x: T { return expr } */
        p->pos = before_brace;
        node->data.var.is_computed = 1;
        node->data.var.has_getter = 1;
        node->data.var.getter_body = parse_block(p);
      }
    }
  }
  node->tok_end = (uint32_t)p->pos;

  /* Multi-var: var x = 0, y = 0 — chain extras via next_sibling */
  ASTNode *tail = node;
  while (P_COMMA(p)) {
    adv(p);
    if (p_is_eof(p) || p_tok(p)->type != TOK_IDENTIFIER)
      break;
    ASTNode *extra = alloc_node(p, is_let ? AST_LET_DECL : AST_VAR_DECL);
    if (!extra)
      return NULL;
    extra->modifiers = mods;
    extra->data.var.name_tok = (uint32_t)p->pos;
    adv(p);
    if (P_COLON(p)) {
      adv(p);
      ASTNode *tn = parse_type(p);
      if (tn)
        ast_add_child(extra, tn);
    }
    if (!p_is_eof(p) && p->src->data[p_tok(p)->pos] == '=') {
      adv(p);
      ASTNode *init = parse_expr_pratt(p, 0);
      if (init)
        ast_add_child(extra, init);
    }
    extra->tok_end = (uint32_t)p->pos;
    tail->next_sibling = extra;
    tail = extra;
  }

  return node;
}
