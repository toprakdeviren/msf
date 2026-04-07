/**
 * @file pre.c
 * @brief Operator precedence tables, argument list parsing, and custom operator registration.
 */
#include "../private.h"

/**
 * @brief Registers a parsed precedencegroup declaration into the parser's runtime table.
 *
 * Extracts the group name and associativity from the AST, assigns default
 * binding powers (lbp=130), and adjusts rbp based on associativity
 * (left: rbp=lbp-1, right: rbp=lbp+1, none: rbp=lbp).
 */
void register_precedence_group_from_ast(Parser *p, ASTNode *node) {
  if (p->pg_count >= MAX_PRECEDENCE_GROUPS)
    return;
  if (!node->data.var.name_tok)
    return;
  const Token *t = &p->ts->tokens[node->data.var.name_tok];
  char *name = malloc((size_t)t->len + 1);
  if (!name)
    return;
  memcpy(name, p->src->data + t->pos, (size_t)t->len);
  name[t->len] = '\0';
  int lbp = 130, rbp = 130;
  for (ASTNode *c = node->first_child; c; c = c->next_sibling) {
    if (c->kind == AST_PG_ASSOCIATIVITY) {
      int assoc = (int)c->data.integer.ival; /* 0=left, 1=right, 2=none */
      if (assoc == 0)
        rbp = lbp - 1;
      else if (assoc == 1)
        rbp = lbp + 1;
      else
        rbp = lbp;
      break;
    }
  }
  p->pg_table[p->pg_count].name = name;
  p->pg_table[p->pg_count].lbp = lbp;
  p->pg_table[p->pg_count].rbp = rbp;
  p->pg_count++;
}

/**
 * @brief Registers a parsed operator declaration (infix operator <op> : Group).
 *
 * Links the operator symbol to its precedence group name so that
 * get_custom_infix_prec() can look up binding powers at parse time.
 */
void register_operator_from_ast(Parser *p, ASTNode *node) {
  if (p->custom_op_count >= MAX_CUSTOM_OPERATORS)
    return;
  if (!node->data.var.name_tok)
    return;
  const Token *t = &p->ts->tokens[node->data.var.name_tok];
  char *op = malloc((size_t)t->len + 1);
  if (!op)
    return;
  memcpy(op, p->src->data + t->pos, (size_t)t->len);
  op[t->len] = '\0';
  char *group = NULL;
  const ASTNode *child = node->first_child;
  if (child && child->kind == AST_PG_GROUP_REF && child->data.var.name_tok) {
    const Token *gt = &p->ts->tokens[child->data.var.name_tok];
    group = malloc((size_t)gt->len + 1);
    if (group) {
      memcpy(group, p->src->data + gt->pos, (size_t)gt->len);
      group[gt->len] = '\0';
    }
  }
  if (!group) {
    free(op);
    return;
  }
  p->custom_ops[p->custom_op_count].op = op;
  p->custom_ops[p->custom_op_count].group_name = group;
  p->custom_op_count++;
}

/**
 * @brief Looks up binding powers for a custom infix operator.
 *
 * Matches the current token against registered custom operators, then
 * resolves the associated precedence group's lbp/rbp.
 *
 * @return Prec with lbp/rbp >= 0 if found, or {-1, -1} if not a custom operator.
 */
Prec get_custom_infix_prec(Parser *p) {
  const Token *t = p_tok(p);
  if (t->type != TOK_OPERATOR || t->len <= 0)
    return (Prec){-1, -1};
  size_t len = (size_t)t->len;
  for (int i = 0; i < p->custom_op_count; i++) {
    size_t olen = strlen(p->custom_ops[i].op);
    if (olen == len &&
        memcmp(p->src->data + t->pos, p->custom_ops[i].op, len) == 0) {
      const char *gn = p->custom_ops[i].group_name;
      for (int j = 0; j < p->pg_count; j++)
        if (p->pg_table[j].name && strcmp(p->pg_table[j].name, gn) == 0)
          return (Prec){p->pg_table[j].lbp, p->pg_table[j].rbp};
      return (Prec){130, 130};
    }
  }
  return (Prec){-1, -1};
}

/**
 * @brief Returns the infix precedence (lbp/rbp) for the current token.
 *
 * Handles built-in operators via op_kind switch (O(1)), single-char operators
 * via character switch, custom operators via get_custom_infix_prec(), and
 * is/as keywords.  Returns {-1, -1} if the token is not an infix operator.
 */
Prec get_infix_prec(Parser *p) {
  if (p_is_eof(p))
    return (Prec){-1, -1};

  const Token *t = p_tok(p);

  if (t->type == TOK_OPERATOR) {
    /* Multi-char: dispatch by pre-classified op_kind */
    switch (t->op_kind) {
    case OP_EQ:
    case OP_NEQ:
    case OP_LEQ:
    case OP_GEQ:
    case OP_IDENTITY_EQ:
    case OP_IDENTITY_NEQ:
      return (Prec){130, 131};
    case OP_RANGE_EXCL:
    case OP_RANGE_INCL:
      return (Prec){135, 135};
    case OP_NIL_COAL:
      return (Prec){125, 124}; /* right-assoc */
    case OP_AND:
      return (Prec){120, 121};
    case OP_OR:
      return (Prec){110, 111};
    case OP_LSHIFT:
    case OP_RSHIFT:
      return (Prec){157, 158};
    case OP_WRAP_ADD:
    case OP_WRAP_SUB:
      return (Prec){140, 141};
    case OP_WRAP_MUL:
      return (Prec){150, 151};
    case OP_ADD_ASSIGN:
    case OP_SUB_ASSIGN:
    case OP_MUL_ASSIGN:
    case OP_DIV_ASSIGN:
    case OP_MOD_ASSIGN:
    case OP_AND_ASSIGN:
    case OP_OR_ASSIGN:
    case OP_XOR_ASSIGN:
      return (Prec){90, 89}; /* right-assoc assignment */
    case OP_ARROW:
      return (Prec){-1, -1}; /* not an infix op (type syntax only) */
    default:
      break;
    }
    /* Single-char operators (op_kind == OP_NONE) */
    char c = p->src->data[t->pos];
    switch (c) {
    case '<':
    case '>':
      return (Prec){130, 131};
    case '+':
    case '-':
      return (Prec){140, 141};
    case '*':
    case '/':
    case '%':
      return (Prec){150, 151};
    case '&':
      return (Prec){145, 146};
    case '|':
      return (Prec){115, 116};
    case '^':
      return (Prec){118, 119};
    case '=':
      return (Prec){90, 89}; /* right-assoc */
    case '?':
      return (Prec){100, 99}; /* ternary */
    default: {
      Prec custom = get_custom_infix_prec(p);
      if (custom.lbp >= 0)
        return custom;
      break;
    }
    }
  }
  /* is / as keyword infix operators */
  if (t->type == TOK_KEYWORD) {
    if (t->keyword == KW_IS || t->keyword == KW_AS)
      return (Prec){132, 133};
  }
  return (Prec){-1, -1};
}

ASTNode *parse_expr_pratt(Parser *p, int min_prec);
ASTNode *parse_prefix(Parser *p);
ASTNode *parse_postfix(Parser *p, ASTNode *lhs);
ASTNode *parse_closure_body(Parser *p);

/**
 * @brief Parses a comma-separated argument list: (label: expr, expr, ...).
 *
 * Detects optional argument labels (identifier followed by ':') and stores
 * them in arg_label_tok.  Stops at @p end_char without consuming it.
 */
void parse_arg_list(Parser *p, ASTNode *parent, char end_char) {
  while (!p_is_eof(p) && !p_is_punct(p, end_char)) {
    size_t before = p->pos;
    uint32_t label_tok = 0;
    int has_label = 0;
    /* Detect optional label: `separator: " "` */
    if ((p_tok(p)->type == TOK_IDENTIFIER || p_tok(p)->type == TOK_KEYWORD) &&
        p->pos + 1 < p->ts->count &&
        p->ts->tokens[p->pos + 1].type == TOK_PUNCT &&
        p->src->data[p->ts->tokens[p->pos + 1].pos] == ':') {
      label_tok = (uint32_t)p->pos;
      has_label = 1;
      adv(p); /* label */
      adv(p); /* ':' */
      before = p->pos;
    }
    ASTNode *e = parse_expr_pratt(p, 0);
    if (e) {
      if (has_label)
        e->arg_label_tok = label_tok;
      ast_add_child(parent, e);
    } else if (p->pos == before)
      adv(p); /* progress guard */
    if (P_COMMA(p))
      adv(p);
  }
}
