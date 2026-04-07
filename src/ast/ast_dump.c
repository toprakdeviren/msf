/**
 * @file ast_dump.c
 * @brief AST serialization: text, JSON, and S-expression output.
 *
 * This file is a pure consumer of the AST — it reads nodes and prints them.
 * It does NOT allocate nodes, modify the tree, or manage memory.
 * The core AST infrastructure lives in ast.c.
 *
 * Serialization is table-driven: each ASTNodeKind maps to a DumpMode that
 * determines how the node's payload is extracted (function name, variable
 * name, operator token, literal value, etc.).  A single extract_value()
 * function serves all three output formats.
 */

#include "internal/ast.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════════
 * Dump Mode Table
 *
 * Maps each ASTNodeKind to a formatting strategy.
 * Adding a new node kind = one line here.  No formatter changes needed.
 * ═══════════════════════════════════════════════════════════════════════════════ */

typedef enum {
  DUMP_PLAIN,       /* no payload — just print the node kind label           */
  DUMP_FUNC_NAME,   /* label(name) — function/init declaration name          */
  DUMP_VAR_NAME,    /* label(name) — var/let/param/class/struct name         */
  DUMP_DOT_NAME,    /* label(.name) — member expression                      */
  DUMP_AT_NAME,     /* label(@name) — attribute                              */
  DUMP_TOK_IDX,     /* label(token) — identifier, type, import               */
  DUMP_OP,          /* label(op) — binary/unary/assignment operator           */
  DUMP_INT_LIT,     /* label(42) — integer literal value                     */
  DUMP_FLOAT_LIT,   /* label(3.14) — floating point literal value            */
  DUMP_STRING_LIT,  /* label("hello") — string/regex literal token           */
  DUMP_BOOL_LIT,    /* label(true/false) — boolean literal                   */
  DUMP_CASE_CLAUSE, /* label + optional (default) annotation                 */
  DUMP_KEYPATH,     /* label(\Type.member) — key path expression             */
} DumpMode;

static const DumpMode dump_modes[AST__COUNT] = {
  [AST_FUNC_DECL]            = DUMP_FUNC_NAME,
  [AST_INIT_DECL]            = DUMP_FUNC_NAME,

  [AST_VAR_DECL]             = DUMP_VAR_NAME,
  [AST_LET_DECL]             = DUMP_VAR_NAME,
  [AST_PARAM]                = DUMP_VAR_NAME,
  [AST_TYPEALIAS_DECL]       = DUMP_VAR_NAME,
  [AST_ENUM_ELEMENT_DECL]    = DUMP_VAR_NAME,
  [AST_OPTIONAL_BINDING]     = DUMP_VAR_NAME,
  [AST_CLASS_DECL]           = DUMP_VAR_NAME,
  [AST_STRUCT_DECL]          = DUMP_VAR_NAME,
  [AST_ENUM_DECL]            = DUMP_VAR_NAME,
  [AST_PROTOCOL_DECL]        = DUMP_VAR_NAME,
  [AST_EXTENSION_DECL]       = DUMP_VAR_NAME,
  [AST_ACTOR_DECL]           = DUMP_VAR_NAME,
  [AST_GENERIC_PARAM]        = DUMP_VAR_NAME,
  [AST_PRECEDENCE_GROUP_DECL]= DUMP_VAR_NAME,

  [AST_MEMBER_EXPR]          = DUMP_DOT_NAME,
  [AST_ATTRIBUTE]            = DUMP_AT_NAME,

  [AST_IDENT_EXPR]           = DUMP_TOK_IDX,
  [AST_TYPE_IDENT]           = DUMP_TOK_IDX,
  [AST_TYPE_GENERIC]         = DUMP_TOK_IDX,
  [AST_TYPE_SOME]            = DUMP_TOK_IDX,
  [AST_TYPE_ANY]             = DUMP_TOK_IDX,
  [AST_IMPORT_DECL]          = DUMP_TOK_IDX,
  [AST_OWNERSHIP_SPEC]       = DUMP_TOK_IDX,
  [AST_THROWS_CLAUSE]        = DUMP_TOK_IDX,
  [AST_ACCESSOR_DECL]        = DUMP_TOK_IDX,
  [AST_AVAILABILITY_EXPR]    = DUMP_TOK_IDX,
  [AST_MACRO_EXPANSION]      = DUMP_TOK_IDX,
  [AST_OPERATOR_DECL]        = DUMP_TOK_IDX,

  [AST_KEY_PATH_EXPR]        = DUMP_KEYPATH,

  [AST_BINARY_EXPR]          = DUMP_OP,
  [AST_UNARY_EXPR]           = DUMP_OP,
  [AST_CONSUME_EXPR]         = DUMP_OP,
  [AST_ASSIGN_EXPR]          = DUMP_OP,
  [AST_CAST_EXPR]            = DUMP_OP,

  [AST_INTEGER_LITERAL]      = DUMP_INT_LIT,
  [AST_FLOAT_LITERAL]        = DUMP_FLOAT_LIT,
  [AST_STRING_LITERAL]       = DUMP_STRING_LIT,
  [AST_REGEX_LITERAL]        = DUMP_STRING_LIT,
  [AST_BOOL_LITERAL]         = DUMP_BOOL_LIT,

  [AST_CASE_CLAUSE]          = DUMP_CASE_CLAUSE,
};

/* ═══════════════════════════════════════════════════════════════════════════════
 * Shared Value Extraction
 * ═══════════════════════════════════════════════════════════════════════════════ */

typedef enum {
  VAL_NONE, VAL_STRING, VAL_INT, VAL_FLOAT, VAL_BOOL,
} ValKind;

typedef struct {
  ValKind     kind;
  const char *text;
  size_t      text_len;
  const char *prefix;
  int64_t     ival;
  double      fval;
  uint8_t     bval;
} NodeValue;

/**
 * @brief Formats a double into a locale-independent string.
 *
 * Some locales use comma as decimal separator (3,14 instead of 3.14).
 * This function always produces a period, ensuring valid JSON/S-expr output.
 *
 * @param buf  Output buffer.
 * @param cap  Buffer capacity.
 * @param val  Value to format.
 */
static void format_double(char *buf, size_t cap, double val) {
  snprintf(buf, cap, "%g", val);
  for (char *c = buf; *c; c++)
    if (*c == ',') { *c = '.'; break; }
}

/**
 * @brief Escapes a string for JSON/S-expression output.
 *
 * Writes the escaped content directly to the output stream.
 * Escapes: `"` `\\` `\n` `\r` `\t` and control chars (< 32) as `\\uXXXX`.
 * All other bytes are passed through verbatim (including valid UTF-8).
 *
 * @param out  Output stream.
 * @param s    String bytes to escape.
 * @param len  Number of bytes.
 */
static void raw_escape(FILE *out, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\')      fprintf(out, "\\%c", c);
    else if (c == '\n')             fputs("\\n", out);
    else if (c == '\r')             fputs("\\r", out);
    else if (c == '\t')             fputs("\\t", out);
    else if (c < 32)                fprintf(out, "\\u%04x", c);
    else                            fputc(c, out);
  }
}

/**
 * @brief Extracts the printable payload from an AST node.
 *
 * Table-driven: looks up the node's DumpMode from dump_modes[], then
 * switches on the mode to pull the right field from the node's union.
 * Returns a NodeValue with kind=VAL_NONE if the node has no payload.
 *
 * This is the single extraction point shared by all three output formats
 * (text, JSON, S-expr).  Adding a new node kind requires only one entry
 * in dump_modes[] — no formatter changes needed.
 *
 * @param node    AST node to extract from.
 * @param src     Source (for resolving token text).
 * @param tokens  Token array (for looking up token positions).
 * @return        Extracted value (kind + text/int/float/bool payload).
 */
static NodeValue extract_value(const ASTNode *node, const Source *src,
                               const Token *tokens) {
  NodeValue v = { .kind = VAL_NONE, .prefix = "" };
  DumpMode m = dump_modes[node->kind];

  switch (m) {
  case DUMP_FUNC_NAME:
  case DUMP_VAR_NAME:
  case DUMP_TOK_IDX:
  case DUMP_KEYPATH:
  case DUMP_AT_NAME: {
    uint32_t tidx;
    if (m == DUMP_AT_NAME || m == DUMP_TOK_IDX || m == DUMP_KEYPATH)
      tidx = node->tok_idx;
    else if (m == DUMP_FUNC_NAME)
      tidx = node->data.func.name_tok;
    else
      tidx = node->data.var.name_tok;
    if (!tidx) return v;
    const Token *t = &tokens[tidx];
    if (t->len == 0) return v;
    v.kind = VAL_STRING;
    v.text = src->data + t->pos;
    v.text_len = t->len;
    if (m == DUMP_AT_NAME) v.prefix = "@";
    break;
  }
  case DUMP_DOT_NAME: {
    if (!node->data.var.name_tok) return v;
    const Token *t = &tokens[node->data.var.name_tok];
    if (t->len == 0) return v;
    v.kind = VAL_STRING;
    v.text = src->data + t->pos;
    v.text_len = t->len;
    v.prefix = ".";
    break;
  }
  case DUMP_OP: {
    if (!node->data.binary.op_tok) return v;
    const Token *t = &tokens[node->data.binary.op_tok];
    if (t->len == 0) return v;
    v.kind = VAL_STRING;
    v.text = src->data + t->pos;
    v.text_len = t->len;
    break;
  }
  case DUMP_STRING_LIT: {
    const Token *t = &tokens[node->tok_idx];
    if (t->len == 0) return v;
    v.kind = VAL_STRING;
    v.text = src->data + t->pos;
    v.text_len = t->len;
    break;
  }
  case DUMP_INT_LIT:
    v.kind = VAL_INT;
    v.ival = node->data.integer.ival;
    break;
  case DUMP_FLOAT_LIT:
    v.kind = VAL_FLOAT;
    v.fval = node->data.flt.fval;
    break;
  case DUMP_BOOL_LIT:
    v.kind = VAL_BOOL;
    v.bval = node->data.boolean.bval;
    break;
  case DUMP_CASE_CLAUSE:
    if (node->data.cas.is_default) {
      v.kind = VAL_STRING; v.text = "default"; v.text_len = 7;
    }
    break;
  default:
    break;
  }
  return v;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Text Dump
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Prints the AST as indented plain text.
 *
 * Recursively walks the tree depth-first.  Each node is printed as:
 *   `<indent><kind>` or `<indent><kind>(<value>)`
 *
 * Example output:
 * @code
 *   source_file
 *     var_decl(x)
 *       integer_literal(42)
 * @endcode
 *
 * @param node    Root of the subtree to print.
 * @param src     Source (for token text lookup).
 * @param tokens  Token array.
 * @param depth   Current indentation depth (0 for root).
 * @param out     Output stream (NULL defaults to stdout).
 */
void ast_print(const ASTNode *node, const Source *src, const Token *tokens,
               int depth, FILE *out) {
  if (!out) out = stdout;
  if (!node) return;

  for (int i = 0; i < depth * 2; i++)
    fputc(' ', out);

  const char *label = ast_kind_name(node->kind);
  NodeValue v = extract_value(node, src, tokens);

  if (v.kind == VAL_NONE)        fprintf(out, "%s\n", label);
  else if (v.kind == VAL_STRING) {
    fprintf(out, "%s(%s", label, v.prefix);
    fwrite(v.text, 1, v.text_len, out);
    fputs(")\n", out);
  }
  else if (v.kind == VAL_INT)    fprintf(out, "%s(%" PRId64 ")\n", label, v.ival);
  else if (v.kind == VAL_FLOAT)  fprintf(out, "%s(%g)\n", label, v.fval);
  else if (v.kind == VAL_BOOL)   fprintf(out, "%s(%s)\n", label, v.bval ? "true" : "false");

  for (const ASTNode *c = node->first_child; c; c = c->next_sibling)
    ast_print(c, src, tokens, depth + 1, out);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * JSON Dump
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Writes a node's value as a JSON field: ,"value":"..." or ,"value":42.
 *
 * Outputs nothing for VAL_NONE.  Strings are escaped via raw_escape().
 *
 * @param v    Extracted value.
 * @param out  Output stream.
 */
static void write_json_value(const NodeValue *v, FILE *out) {
  switch (v->kind) {
  case VAL_STRING:
    fputs(",\"value\":\"", out);
    fputs(v->prefix, out);
    raw_escape(out, v->text, v->text_len);
    fputc('"', out);
    break;
  case VAL_INT:   fprintf(out, ",\"value\":%" PRId64, v->ival); break;
  case VAL_FLOAT: {
    char fbuf[64]; format_double(fbuf, sizeof(fbuf), v->fval);
    fprintf(out, ",\"value\":%s", fbuf); break;
  }
  case VAL_BOOL:  fprintf(out, ",\"value\":%s", v->bval ? "true" : "false"); break;
  case VAL_NONE:  break;
  }
}

/**
 * @brief Recursive helper for JSON serialization.
 *
 * Produces: {"kind":"...","value":...,"children":[...]}
 * Children array is omitted for leaf nodes.
 */
static void ast_dump_json_rec(const ASTNode *node, const Source *src,
                              const Token *tokens, FILE *out) {
  if (!node) return;
  fprintf(out, "{\"kind\":\"%s\"", ast_kind_name(node->kind));
  NodeValue v = extract_value(node, src, tokens);
  write_json_value(&v, out);
  const ASTNode *c = node->first_child;
  if (c) {
    fputs(",\"children\":[", out);
    for (const char *sep = ""; c; c = c->next_sibling, sep = ",") {
      fputs(sep, out);
      ast_dump_json_rec(c, src, tokens, out);
    }
    fputc(']', out);
  }
  fputc('}', out);
}

/**
 * @brief Serializes the AST as a single-line JSON object.
 *
 * Format: {"kind":"source_file","children":[{"kind":"var_decl","value":"x",...},...]}
 * Terminates output with a newline.
 *
 * @param root    Root of the subtree to serialize.
 * @param src     Source (for token text lookup).
 * @param tokens  Token array.
 * @param out     Output stream (NULL defaults to stdout).
 */
void ast_dump_json(const ASTNode *root, const Source *src, const Token *tokens,
                   FILE *out) {
  if (!out) out = stdout;
  if (!root) return;
  ast_dump_json_rec(root, src, tokens, out);
  fputc('\n', out);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * S-Expression Dump
 * ═══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Writes a node's value in S-expression format: " \"...\"" or " 42".
 *
 * Outputs nothing for VAL_NONE.  Strings are escaped via raw_escape().
 *
 * @param v    Extracted value.
 * @param out  Output stream.
 */
static void write_sexpr_value(const NodeValue *v, FILE *out) {
  switch (v->kind) {
  case VAL_STRING:
    fputs(" \"", out);
    fputs(v->prefix, out);
    raw_escape(out, v->text, v->text_len);
    fputc('"', out);
    break;
  case VAL_INT:   fprintf(out, " %" PRId64, v->ival); break;
  case VAL_FLOAT: {
    char fbuf[64]; format_double(fbuf, sizeof(fbuf), v->fval);
    fprintf(out, " %s", fbuf); break;
  }
  case VAL_BOOL:  fprintf(out, " %s", v->bval ? "true" : "false"); break;
  case VAL_NONE:  break;
  }
}

/**
 * @brief Recursive helper for S-expression serialization.
 *
 * Produces: (kind "value" (child1) (child2 "val" ...))
 */
static void ast_dump_sexpr_rec(const ASTNode *node, const Source *src,
                               const Token *tokens, FILE *out) {
  if (!node) return;
  fprintf(out, "(%s", ast_kind_name(node->kind));
  NodeValue v = extract_value(node, src, tokens);
  write_sexpr_value(&v, out);
  for (const ASTNode *c = node->first_child; c; c = c->next_sibling) {
    fputc(' ', out);
    ast_dump_sexpr_rec(c, src, tokens, out);
  }
  fputc(')', out);
}

/**
 * @brief Serializes the AST as an S-expression.
 *
 * Format: (source_file (var_decl "x" (integer_literal 42)))
 * Terminates output with a newline.
 *
 * @param root    Root of the subtree to serialize.
 * @param src     Source (for token text lookup).
 * @param tokens  Token array.
 * @param out     Output stream (NULL defaults to stdout).
 */
void ast_dump_sexpr(const ASTNode *root, const Source *src, const Token *tokens,
                    FILE *out) {
  if (!out) out = stdout;
  if (!root) return;
  ast_dump_sexpr_rec(root, src, tokens, out);
  fputc('\n', out);
}
