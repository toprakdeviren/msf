/**
 * @file member.c
 * @brief Member expression resolution: base.member, implicit .member,
 *        optional chaining, tuple indexing, associated type resolution.
 */

#include "../../private.h"

/*
 */

TypeInfo *resolve_member_expr(SemaContext *ctx, ASTNode *node) {
  ASTNode *base = node->first_child;
  const ASTNode *member = base ? base->next_sibling : NULL;
  const char *mname = member ? tok_intern(ctx, member->tok_idx)
                             : (node->data.var.name_tok
                                    ? tok_intern(ctx, node->data.var.name_tok)
                                    : NULL);
  if (base && base->kind == AST_IDENT_EXPR && mname) {
    const char *bname = tok_intern(ctx, base->tok_idx);
    if (bname && strcmp(bname, "CommandLine") == 0 &&
        strcmp(mname, "arguments") == 0) {
      TypeInfo *arr_t = type_arena_alloc(ctx->type_arena);
      arr_t->kind = TY_ARRAY;
      arr_t->inner = TY_BUILTIN_STRING;
      return (node->type = arr_t);
    }
  }
  if (ctx->in_class_init_phase1 && base && base->kind == AST_IDENT_EXPR) {
    const char *bname = tok_intern(ctx, base->tok_idx);
    if (bname && strcmp(bname, "self") == 0) {
      if (mname && strcmp(mname, "init") != 0) {
        int is_lhs = (node->parent && node->parent->kind == AST_ASSIGN_EXPR &&
                      node->parent->first_child == node);
        if (!is_lhs)
          sema_error(ctx, node,
                     "Cannot read instance property before 'super.init()' "
                     "(two-phase initialization)");
      }
    }
  }
  TypeInfo *base_t = base ? resolve_node(ctx, base) : NULL;
  if (!base_t && !base && mname) {
    /* Implicit member expression (.foo) — resolve from contextual type */
    TypeInfo *ctx_type = get_contextual_type_for_implicit_member(ctx, node);
    if (ctx_type) {
      if (ctx_type->kind == TY_OPTIONAL && ctx_type->inner)
        ctx_type = ctx_type->inner;
      TypeInfo *found = lookup_builtin_member(ctx, ctx_type, mname);
      if (found)
        return (node->type = found);
      if (ctx_type->kind == TY_TUPLE && node->data.var.name_tok) {
        const Token *mtok = &ctx->tokens[node->data.var.name_tok];
        if (mtok->type == TOK_INTEGER_LIT) {
          char ibuf[16];
          size_t ilen = mtok->len < 15 ? mtok->len : 15;
          memcpy(ibuf, ctx->src->data + mtok->pos, ilen);
          ibuf[ilen] = '\0';
          uint32_t idx = (uint32_t)atoi(ibuf);
          if (idx < ctx_type->tuple.elem_count && ctx_type->tuple.elems[idx])
            return (node->type = ctx_type->tuple.elems[idx]);
        }
      }
      if (ctx_type->kind == TY_NAMED && ctx_type->named.decl) {
        const ASTNode *decl = (const ASTNode *)ctx_type->named.decl;
        const ASTNode *body = class_decl_body(decl);
        for (const ASTNode *ch = body ? body->first_child : decl->first_child;
             ch; ch = ch->next_sibling) {
          if (ch->kind == AST_VAR_DECL || ch->kind == AST_LET_DECL) {
            const char *chn = tok_intern(ctx, ch->tok_idx);
            if (chn == mname) {
              if (!ch->type)
                resolve_node(ctx, (ASTNode *)ch);
              return (node->type = ((ASTNode *)ch)->type);
            }
          }
          if (ch->kind == AST_FUNC_DECL) {
            const char *chn = tok_intern(ctx, ch->data.func.name_tok);
            if (chn == mname) {
              if (!ch->type)
                resolve_node(ctx, (ASTNode *)ch);
              return (node->type = ((ASTNode *)ch)->type);
            }
          }
          if (ch->kind == AST_ATTRIBUTE && ch->next_sibling &&
              (ch->next_sibling->kind == AST_VAR_DECL ||
               ch->next_sibling->kind == AST_LET_DECL)) {
            const ASTNode *var = ch->next_sibling;
            const char *chn = tok_intern(ctx, var->data.var.name_tok);
            if (chn == mname) {
              if (!var->type)
                resolve_node(ctx, (ASTNode *)var);
              return (node->type = ((ASTNode *)var)->type);
            }
          }
        }
        if (decl->kind == AST_ENUM_DECL) {
          for (const ASTNode *b = decl->first_child; b; b = b->next_sibling) {
            if (b->kind != AST_BLOCK)
              continue;
            for (const ASTNode *cd = b->first_child; cd;
                 cd = cd->next_sibling) {
              if (cd->kind != AST_ENUM_CASE_DECL)
                continue;
              for (const ASTNode *el = cd->first_child; el;
                   el = el->next_sibling) {
                if (el->kind != AST_ENUM_ELEMENT_DECL)
                  continue;
                const char *el_name = tok_intern(ctx, el->data.var.name_tok);
                if (el_name && strcmp(el_name, mname) == 0)
                  return (node->type = ctx_type);
              }
            }
          }
        }
      }
    }
  }
  TypeInfo *unwrapped_base = base_t;
  if (base_t && base_t->kind == TY_OPTIONAL)
    unwrapped_base = base_t->inner;
  int base_is_opt_chain = (base && base->kind == AST_OPTIONAL_CHAIN);

  if (unwrapped_base && unwrapped_base == TY_BUILTIN_STRING && mname &&
      strcmp(mname, "uuidString") == 0) {
    return (node->type = wrap_optional_result(TY_BUILTIN_STRING,
                                              base_is_opt_chain, ctx));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.name && mname &&
      strcmp(unwrapped_base->named.name, "Mirror") == 0) {
    if (strcmp(mname, "displayStyle") == 0)
      return (node->type = wrap_optional_result(TY_BUILTIN_STRING, 1, ctx));
  }

  /* Optional.map / Optional.flatMap: resolve on Optional type (not unwrapped) */
  if (base_t && base_t->kind == TY_OPTIONAL && mname) {
    TypeInfo *bm_opt = lookup_builtin_member(ctx, base_t, mname);
    if (bm_opt)
      return (node->type = bm_opt);
  }
  if (unwrapped_base && mname) {
    TypeInfo *bm = lookup_builtin_member(ctx, unwrapped_base, mname);
    if (bm)
      return (node->type = wrap_optional_result(bm, base_is_opt_chain, ctx));
  }

  if (unwrapped_base && unwrapped_base->kind == TY_TUPLE &&
      node->data.var.name_tok) {
    const Token *mtok = &ctx->tokens[node->data.var.name_tok];
    if (mtok->type == TOK_INTEGER_LIT) {
      char ibuf[16];
      size_t ilen = mtok->len < 15 ? mtok->len : 15;
      memcpy(ibuf, ctx->src->data + mtok->pos, ilen);
      ibuf[ilen] = '\0';
      uint32_t idx = (uint32_t)atoi(ibuf);
      if (idx < unwrapped_base->tuple.elem_count &&
          unwrapped_base->tuple.elems[idx]) {
        TypeInfo *t = unwrapped_base->tuple.elems[idx];
        return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
      }
    }
    if (mtok->type == TOK_IDENTIFIER && unwrapped_base->tuple.labels) {
      const char *mname =
          sema_intern(ctx, ctx->src->data + mtok->pos, mtok->len);
      for (size_t li = 0; li < unwrapped_base->tuple.elem_count; li++) {
        if (unwrapped_base->tuple.labels[li] &&
            unwrapped_base->tuple.labels[li] == mname) {
          TypeInfo *t = unwrapped_base->tuple.elems[li];
          return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
        }
      }
    }
  }

  if (unwrapped_base && unwrapped_base->kind == TY_NAMED &&
      unwrapped_base->named.decl && mname) {
    const ASTNode *decl = (const ASTNode *)unwrapped_base->named.decl;
    const ASTNode *body = class_decl_body(decl);
    for (const ASTNode *ch = body ? body->first_child : decl->first_child; ch;
         ch = ch->next_sibling) {
      if (ch->kind == AST_VAR_DECL || ch->kind == AST_LET_DECL) {
        const char *chn = tok_intern(ctx, ch->tok_idx);
        if (chn == mname) {
          if (!private_member_visible(ctx, ch, decl))
            sema_error(ctx, node, "private member is not accessible");
          if (!ch->type)
            resolve_node(ctx, (ASTNode *)ch);
          TypeInfo *t = ((ASTNode *)ch)->type;
          return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
        }
      }
      if (ch->kind == AST_FUNC_DECL) {
        const char *chn = tok_intern(ctx, ch->data.func.name_tok);
        if (chn == mname) {
          if (!private_member_visible(ctx, ch, decl))
            sema_error(ctx, node, "private member is not accessible");
          if (!ch->type)
            resolve_node(ctx, (ASTNode *)ch);
          TypeInfo *t = ((ASTNode *)ch)->type;
          return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
        }
      }
      if (ch->kind == AST_ATTRIBUTE && ch->next_sibling &&
          (ch->next_sibling->kind == AST_VAR_DECL ||
           ch->next_sibling->kind == AST_LET_DECL)) {
        const ASTNode *var = ch->next_sibling;
        const char *chn = tok_intern(ctx, var->data.var.name_tok);
        if (chn == mname) {
          if (!private_member_visible(ctx, var, decl))
            sema_error(ctx, node, "private member is not accessible");
          if (!var->type)
            resolve_node(ctx, (ASTNode *)var);
          TypeInfo *t = ((ASTNode *)var)->type;
          return (node->type = wrap_optional_result(t, base_is_opt_chain, ctx));
        }
      }
    }
    /* Associated type resolution: T.Element → concrete type via conformance */
    if (ctx->conformance_table && ctx->assoc_type_table) {
      const ConformanceTable *ct = ctx->conformance_table;
      for (uint32_t ci = 0; ci < ct->count; ci++) {
        if (!ct->entries[ci].type_name || !ct->entries[ci].protocol_name)
          continue;
        if (strcmp(ct->entries[ci].type_name, unwrapped_base->named.name) != 0)
          continue;
        TypeInfo *resolved = resolve_assoc_type_to_concrete(
            ctx, unwrapped_base, ct->entries[ci].protocol_name, mname);
        if (resolved)
          return (node->type =
                      wrap_optional_result(resolved, base_is_opt_chain, ctx));
      }
    }
  }
  if (node->type)
    return node->type;
  return NULL;
}
