/**
 * @file proto_iface.c
 * @brief PROTOTYPE (throwaway): measure Kademe-3 member extraction from a
 *        synthesized Swift interface.
 *
 *   proto-iface <file.swift>            > distilled.txt   (stats on stderr)
 *
 * Parse-only (no sema): the interface text carries explicit type spellings, so
 * each member's signature is just the source slice [tok_idx, tok_end) with the
 * body ('{...}') dropped and whitespace collapsed. We attribute each member to
 * its enclosing nominal type (or extension target) and emit one line per member.
 *
 * Goal: (a) does msf's parser swallow all Kademe-3 syntax (generics, where-
 * clauses, attributes, associated types) without choking → parser_error_count;
 * (b) how big is the distilled per-module db → byte counts + (gz externally).
 */
#include "msf.h"
#include "internal/msf.h"    /* parser_init, parse_source_file, error accessors */
#include "internal/lexer.h"  /* token_stream_init/free, lexer_tokenize          */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return NULL; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  if (n < 0) { fclose(f); return NULL; }
  char *b = malloc((size_t)n + 1);
  if (!b) { fclose(f); return NULL; }
  size_t rd = fread(b, 1, (size_t)n, f);
  b[rd] = '\0';
  fclose(f);
  if (out_len) *out_len = rd;
  return b;
}

/* Collapse the source span of token range [a,b) into buf: cut at the first
 * '{' (drops bodies / accessor blocks), collapse whitespace runs to one space,
 * trim ends. Returns the written length. */
static size_t sig_of(const Source *src, const Token *toks, size_t tok_count,
                     uint32_t a, uint32_t b, char *buf, size_t cap) {
  buf[0] = '\0';
  if (b <= a || a >= tok_count || b > tok_count) return 0;
  size_t s = toks[a].pos;
  size_t e = (size_t)toks[b - 1].pos + toks[b - 1].len;
  size_t o = 0;
  int sp = 1; /* leading: treat as space so we trim */
  for (size_t i = s; i < e && o + 1 < cap; i++) {
    char c = src->data[i];
    if (c == '{') break;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!sp) { buf[o++] = ' '; sp = 1; }
    } else {
      buf[o++] = c; sp = 0;
    }
  }
  while (o > 0 && buf[o - 1] == ' ') o--;
  buf[o] = '\0';
  return o;
}

/* Drop leading attribute groups: '@' name [ '(' ...balanced... ')' ] (space)* .
 * Shifts the remainder to the front of s. */
static void strip_attrs(char *s) {
  char *r = s;
  for (;;) {
    while (*r == ' ') r++;
    if (*r != '@') break;
    r++;                                   /* skip '@' */
    while (*r && *r != ' ' && *r != '(') r++;  /* attribute name */
    if (*r == '(') {                       /* balanced (...) */
      int d = 0;
      do {
        if (*r == '(') d++;
        else if (*r == ')') d--;
        r++;
      } while (*r && d > 0);
    }
  }
  memmove(s, r, strlen(r) + 1);
}

static void name_of(const Source *src, const Token *toks, size_t tok_count,
                    uint32_t nt, char *buf, size_t cap) {
  if (!nt || nt >= tok_count) { snprintf(buf, cap, "?"); return; }
  size_t n = toks[nt].len;
  if (n >= cap) n = cap - 1;
  memcpy(buf, src->data + toks[nt].pos, n);
  buf[n] = '\0';
}

static int is_type_decl(ASTNodeKind k) {
  return k == AST_CLASS_DECL || k == AST_STRUCT_DECL || k == AST_ENUM_DECL ||
         k == AST_PROTOCOL_DECL || k == AST_ACTOR_DECL;
}
static int is_member_decl(ASTNodeKind k) {
  return k == AST_FUNC_DECL || k == AST_VAR_DECL || k == AST_LET_DECL ||
         k == AST_INIT_DECL || k == AST_DEINIT_DECL || k == AST_SUBSCRIPT_DECL ||
         k == AST_ENUM_CASE_DECL || k == AST_ENUM_ELEMENT_DECL ||
         k == AST_TYPEALIAS_DECL;
}

static long g_types = 0, g_members = 0;
static unsigned long long g_raw_bytes = 0, g_strip_bytes = 0;

static void walk(const Source *src, const Token *toks, size_t tok_count,
                 const ASTNode *node, const char *ty, unsigned depth) {
  if (depth > 300) return;
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    const char *child_ty = ty;
    char namebuf[256];
    char sig[4096];

    if (is_type_decl(c->kind)) {
      name_of(src, toks, tok_count, c->data.var.name_tok, namebuf, sizeof namebuf);
      g_types++;
      size_t rn = sig_of(src, toks, tok_count, c->tok_idx, c->tok_end, sig, sizeof sig);
      g_raw_bytes += rn + strlen(namebuf) + 3;
      strip_attrs(sig);
      g_strip_bytes += strlen(sig) + strlen(namebuf) + 3;
      printf("T %s|%s\n", namebuf, sig);
      child_ty = namebuf;
    } else if (c->kind == AST_EXTENSION_DECL) {
      name_of(src, toks, tok_count, c->data.var.name_tok, namebuf, sizeof namebuf);
      child_ty = namebuf;
    } else if (is_member_decl(c->kind)) {
      const char *owner = ty ? ty : "(global)";
      size_t rn = sig_of(src, toks, tok_count, c->tok_idx, c->tok_end, sig, sizeof sig);
      g_members++;
      g_raw_bytes += rn + strlen(owner) + 3;
      strip_attrs(sig);
      g_strip_bytes += strlen(sig) + strlen(owner) + 3;
      printf("  %s|%s\n", owner, sig);
    }
    walk(src, toks, tok_count, c, child_ty, depth + 1);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.swift>   (distilled text to stdout, stats to stderr)\n", argv[0]);
    return 2;
  }
  size_t len = 0;
  char *code = read_file(argv[1], &len);
  if (!code) return 1;

  Source src = { code, len, argv[1] };
  TokenStream ts;
  token_stream_init(&ts, len / 4 + 64);
  if (!ts.tokens) { free(code); return 1; }
  LexerDiagnostics diag;
  lexer_diag_init(&diag);
  if (lexer_tokenize(&src, &ts, 1, &diag) != 0) {
    fprintf(stderr, "%s: tokenize failed\n", argv[1]);
    token_stream_free(&ts); free(code); return 1;
  }

  ASTArena arena;
  ast_arena_init(&arena, 0);
  Parser *p = parser_init(&src, &ts, &arena);
  ASTNode *root = p ? parse_source_file(p) : NULL;
  uint32_t perr = p ? parser_error_count(p) : 0;

  if (getenv("PROTO_DUMP_ERRS")) {
    for (uint32_t i = 0; i < perr; i++)
      fprintf(stderr, "L%u\t%s\n", parser_error_line(p, i), parser_error_message(p, i));
  } else if (root) {
    walk(&src, ts.tokens, ts.count, root, NULL, 0);
  }

  fprintf(stderr,
          "%-28s tokens=%-9zu parse_errors=%-6u types=%-6ld members=%-7ld "
          "raw=%lluB strip=%lluB\n",
          argv[1], ts.count, perr, g_types, g_members, g_raw_bytes, g_strip_bytes);

  parser_destroy(p);
  ast_arena_free(&arena);
  token_stream_free(&ts);
  free(code);
  return 0;
}
