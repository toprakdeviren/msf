/**
 * @file msf.h
 * @brief MiniSwiftFrontend — Swift lexer, parser, and type analyzer.
 * @version 0.1.0
 *
 * "Bad code tries to do too much; its purpose is unclear.
 *  Clean code is focused. Every function, every module
 *  serves a single purpose — free from distraction."
 *
 * DESIGN PRINCIPLES
 *
 *   - One-shot API first (msf_analyze does everything)
 *   - Zero-copy tokens (point into source, no string allocations)
 *   - Immutable AST after analysis (read-only tree walking)
 *   - Pointer identity for builtin types (compare with ==, not strcmp)
 *   - No hidden global state (each MSFResult is self-contained)
 *   - Arena allocation (bulk-free, no per-node cleanup)
 *
 * QUICK START
 *
 *   #include <msf.h>
 *
 *   MSFResult *r = msf_analyze("let x: Int = 42", "main.swift");
 *
 *   // check errors
 *   for (uint32_t i = 0; i < msf_error_count(r); i++)
 *       fprintf(stderr, "%u:%u: %s\n",
 *               msf_error_line(r, i), msf_error_col(r, i),
 *               msf_error_message(r, i));
 *
 *   // walk the AST
 *   const ASTNode *root = msf_root(r);
 *   for (const ASTNode *c = root->first_child; c; c = c->next_sibling)
 *       printf("%s\n", ast_kind_name(c->kind));
 *
 *   // dump as JSON
 *   msf_dump_json(r, stdout);
 *
 *   msf_result_free(r);
 *
 * SECTIONS
 *
 *   1.  Source              — what you feed in
 *   2.  Tokens              — what the lexer produces
 *   3.  Types               — what sema resolves
 *   4.  AST                 — the syntax tree
 *   5.  Analyze             — one-call entry point
 *   6.  Read                — inspect the result
 *   7.  Errors              — what went wrong
 *   8.  Dump                — serialize the AST
 *
 *   ───  Backend ABI  ──────────────────────────────────────────────────────
 *   Below this line are the runtime shapes msf_analyze() writes into its
 *   output.  Read-only IDE clients can stop at section 8.  Compiler
 *   backends (miniswift, etc.) that need to monomorphize generics, walk
 *   conformance, or interpret ASTNode.modifiers bits use sections 9-15.
 *
 *   9.  Modifier bits       — ASTNode.modifiers bitmask
 *   10. Type arena          — backing store for TypeInfo
 *   11. Type constraints    — where-clause requirements
 *   12. Type substitution   — generic monomorphization
 *   13. Type predicates     — small inline helpers
 *   14. Conformance table   — (type, protocol) records
 *   15. Associated types    — assoc-type bindings
 *   16. Bare expressions    — re-parse expression strings
 */
#ifndef MSF_H
#define MSF_H

#define MSF_VERSION_MAJOR 0
#define MSF_VERSION_MINOR 1
#define MSF_VERSION_PATCH 0
#define MSF_VERSION_STRING "0.1.0"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. SOURCE — what you feed in                                          │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Source src = { .data = code, .len = strlen(code), .filename = "main.swift" };
 */

/** @brief Describes a Swift source file to analyze. */
typedef struct {
  const char *data;      /**< NUL-terminated source text.       */
  size_t      len;       /**< Byte count (excluding NUL).       */
  const char *filename;  /**< File path (shown in diagnostics). */
} Source;

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. TOKENS — what the lexer produces                                   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Each token points back into the original source by byte offset + length.
 *  Use token_text(&src, &tok) to get a NUL-terminated copy.
 */

/* — What kind of token? --------------------------------------------------- */

/** @brief Classification of a single token.
 *  Values are stable for serialization; do not reorder or reuse gaps. */
typedef enum {
  TOK_UNKNOWN     = 0,    TOK_IDENTIFIER = 1,    TOK_KEYWORD    = 2,
  TOK_INTEGER_LIT = 3,    TOK_FLOAT_LIT  = 4,    TOK_STRING_LIT = 5,
  TOK_OPERATOR    = 6,    TOK_PUNCT      = 7,
  TOK_COMMENT     = 17,   TOK_WHITESPACE = 19,   TOK_NEWLINE    = 20,
  TOK_REGEX_LIT   = 21,   TOK_EOF        = 255,
} TokenType;

/* — Which keyword?  (KW_NONE if not a keyword) ---------------------------- */

/** @brief Swift keyword identifier.  Check tok.keyword when tok.type == TOK_KEYWORD. */
typedef enum {
  KW_NONE = 0,
  /* declarations  */ KW_FUNC, KW_VAR, KW_LET, KW_CLASS, KW_STRUCT, KW_ENUM,
                      KW_PROTOCOL, KW_EXTENSION, KW_IMPORT, KW_TYPEALIAS,
                      KW_INIT, KW_DEINIT, KW_ACTOR,
  /* statements    */ KW_RETURN, KW_IF, KW_ELSE, KW_FOR, KW_WHILE, KW_GUARD,
                      KW_SWITCH, KW_CASE, KW_DEFAULT, KW_BREAK, KW_CONTINUE,
                      KW_FALLTHROUGH, KW_THROW, KW_DO, KW_DEFER, KW_REPEAT,
  /* expressions   */ KW_TRY, KW_AWAIT, KW_AS, KW_IS, KW_IN,
                      KW_TRUE, KW_FALSE, KW_NIL,
                      KW_SELF, KW_SUPER, KW_SOME, KW_ANY,
  /* modifiers     */ KW_PUBLIC, KW_PRIVATE, KW_INTERNAL, KW_FILEPRIVATE,
                      KW_OPEN, KW_PACKAGE, KW_STATIC, KW_FINAL, KW_OVERRIDE,
                      KW_LAZY, KW_MUTATING, KW_ASYNC, KW_THROWS, KW_RETHROWS,
                      KW_WHERE, KW_CATCH, KW_SUBSCRIPT,
  /* operators     */ KW_PRECEDENCEGROUP, KW_PREFIX, KW_POSTFIX, KW_INFIX,
                      KW_OPERATOR,
  /* ownership     */ KW_CONSUME, KW_DISCARD, KW_BORROWING, KW_CONSUMING,
  KW__COUNT
} Keyword;

/* — Which multi-char operator?  (OP_NONE if single-char) ------------------ */

/** @brief Pre-classified multi-character operator.  Check tok.op_kind when tok.type == TOK_OPERATOR. */
typedef enum {
  OP_NONE = 0,
  OP_ARROW,      /* ->  */  OP_FAT_ARROW,  /* =>  */
  OP_EQ,         /* ==  */  OP_NEQ,        /* !=  */
  OP_LEQ,        /* <=  */  OP_GEQ,        /* >=  */
  OP_AND,        /* &&  */  OP_OR,         /* ||  */
  OP_NIL_COAL,   /* ??  */
  OP_LSHIFT,     /* <<  */  OP_RSHIFT,     /* >>  */
  OP_RANGE_EXCL, /* ..< */  OP_RANGE_INCL, /* ... */
  OP_ADD_ASSIGN, /* +=  */  OP_SUB_ASSIGN, /* -=  */
  OP_MUL_ASSIGN, /* *=  */  OP_DIV_ASSIGN, /* /=  */
  OP_MOD_ASSIGN, /* %=  */
  OP_AND_ASSIGN, /* &=  */  OP_OR_ASSIGN,  /* |=  */
  OP_XOR_ASSIGN, /* ^=  */
  OP_WRAP_ADD,   /* &+  */  OP_WRAP_SUB,   /* &-  */
  OP_WRAP_MUL,   /* &*  */
  OP_IDENTITY_EQ,/* === */  OP_IDENTITY_NEQ,/* !== */
  /* Appended (keep at end — preserves enum numbering / lib ABI). */
  OP_SHL_ASSIGN,      /* <<=  */  OP_SHR_ASSIGN,      /* >>=  */
  OP_WRAP_ADD_ASSIGN, /* &+=  */  OP_WRAP_SUB_ASSIGN, /* &-=  */
  OP_WRAP_MUL_ASSIGN, /* &*=  */
  OP_MASK_SHL,        /* &<<  */  OP_MASK_SHR,        /* &>>  */
  OP_MASK_SHL_ASSIGN, /* &<<= */  OP_MASK_SHR_ASSIGN, /* &>>= */
} OpKind;

/* — The token itself ------------------------------------------------------ */

/** @brief A single token from the source code. */
typedef struct {
  TokenType type;       /**< What kind of token.                         */
  uint32_t  pos;        /**< Byte offset in source.                      */
  uint32_t  len;        /**< Byte length.                                */
  uint32_t  line;       /**< 1-based line number.                        */
  uint32_t  col;        /**< 1-based column number.                      */
  Keyword   keyword;    /**< Which keyword (KW_NONE if not a keyword).   */
  OpKind    op_kind;    /**< Which operator (OP_NONE if single-char).    */
  uint8_t   has_leading_newline; /**< 1 if at least one newline was
                                  *   filtered out between the previous
                                  *   token and this one — preserved by
                                  *   lexer_tokenize even when skip_ws
                                  *   discards the newline token itself. */
} Token;

/* — Token stream (flat array, filled by lexer) ---------------------------- */

/** @brief A flat array of tokens produced by the lexer.
 *  Ownership: tokens are owned by the stream and freed by token_stream_free(). */
typedef struct {
  Token  *tokens;       /**< Array of tokens (owned).  */
  size_t  count;        /**< Number of tokens.         */
  size_t  capacity;     /* (internal)                  */
} TokenStream;

/* — Token helpers --------------------------------------------------------- */

/**
 * @brief Returns a human-readable name for a token type.
 * @param t  Token type.
 * @return   Static string: "keyword", "identifier", "int_lit", "eof", etc.
 */
const char *token_type_name(TokenType t);

/**
 * @brief Extracts the text of a token as a NUL-terminated string.
 * @param src  Source descriptor.
 * @param tok  Token to extract.
 * @return     NUL-terminated copy (thread-local buffer — overwritten on next call).
 *             Not reentrant; copy the result if you need to persist it.
 */
const char *token_text(const Source *src, const Token *tok);

/* — Tokenize (only needed for advanced/step-by-step use) ------------------ */

/**
 * @brief Allocates storage for a token stream.
 * @param ts        Stream to initialize.
 * @param capacity  Expected number of tokens (hint).
 */
void token_stream_init(TokenStream *ts, size_t capacity);

/**
 * @brief Frees all memory owned by a token stream.
 * @param ts  Stream to free.  The struct itself is not freed.
 */
void token_stream_free(TokenStream *ts);

/** @brief Opaque lexer diagnostics.  Pass NULL unless you need warnings. */
typedef struct LexerDiagnostics LexerDiagnostics;

/**
 * @brief Tokenizes a source file into a token stream.
 * @param src      Source descriptor.
 * @param out      Pre-initialized stream (receives all tokens).
 * @param skip_ws  Non-zero to omit whitespace/comment tokens.
 * @param diag     Diagnostic sink, or NULL.
 * @return         0 on success.
 */
int lexer_tokenize(const Source *src, TokenStream *out, int skip_ws,
                   LexerDiagnostics *diag);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. TYPES — what sema resolves                                         │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  After msf_analyze(), every ASTNode.type points to a TypeInfo.
 *  Use type_kind_of() to switch, type_to_string() to print.
 *
 *  QUICK GUIDE — Swift type → TypeInfo kind → how to read:
 *
 *    Int           → TY_INT           (builtin singleton, compare with ==)
 *    String        → TY_STRING        (builtin singleton)
 *    [T]           → TY_ARRAY         (read .inner for element type)
 *    T?            → TY_OPTIONAL      (read .inner for wrapped type)
 *    [K: V]        → TY_DICT          (read .dict.key, .dict.value)
 *    (A, B)        → TY_TUPLE         (read .tuple.elems[])
 *    (A) -> B      → TY_FUNC          (read .func.params[], .func.ret)
 *    MyStruct      → TY_NAMED         (read .named.name)
 *    Array<Int>    → TY_GENERIC_INST  (read .generic.base, .generic.args[])
 *
 *    switch (type_kind_of(node->type)) {
 *        case TY_INT:    ...
 *        case TY_STRING: ...
 *        case TY_ARRAY:  element = node->type->inner; ...
 *    }
 */

/* — What kind of type? ---------------------------------------------------- */

/** @brief Classification of a resolved type. */
typedef enum {
  TY_UNKNOWN = 0,
  /* @generated primitive TypeKind cases (scripts/codegen.py) */
  TY_VOID,            /* Void          */
  TY_BOOL,            /* Bool          */
  TY_INT,             /* Int           */
  TY_INT8,            /* Int8          */
  TY_INT16,           /* Int16         */
  TY_INT32,           /* Int32         */
  TY_INT64,           /* Int64         */
  TY_UINT,            /* UInt          */
  TY_UINT8,           /* UInt8         */
  TY_UINT16,          /* UInt16        */
  TY_UINT32,          /* UInt32        */
  TY_UINT64,          /* UInt64        */
  TY_FLOAT,           /* Float         */
  TY_DOUBLE,          /* Double        */
  TY_STRING,          /* String        */
  TY_CHARACTER,       /* Character     */
  TY_JSONENCODER,     /* JSONEncoder   */
  TY_JSONDECODER,     /* JSONDecoder   */
  TY_DATA,            /* Data          */
  TY_SUBSTRING,       /* Substring     */
  TY_OPTIONAL,               /**< T?               */
  TY_ARRAY,                  /**< [T]              */
  TY_DICT,                   /**< [K: V]           */
  TY_SET,                    /**< Set\<T\>         */
  TY_TUPLE,                  /**< (A, B)           */
  TY_FUNC,                   /**< (A) -> B         */
  TY_NAMED,                  /**< MyStruct         */
  TY_METATYPE,               /**< Foo.Type         */
  TY_ERROR,                  /**< failed to resolve */
  TY_GENERIC_PARAM,          /**< T                */
  TY_GENERIC_INST,           /**< Array\<Int\>     */
  TY_ASSOC_REF,              /**< T.Item           */
  TY_PROTOCOL_COMPOSITION,   /**< P & Q            */
} TypeKind;

/* — The type itself ------------------------------------------------------- */

typedef struct TypeConstraint TypeConstraint;  /**< Opaque generic constraint. */
typedef struct TypeInfo TypeInfo;

/**
 * @brief A resolved type.  Read node->type after msf_analyze().
 *
 * Most users do NOT construct TypeInfo manually.
 * Types are produced by msf_analyze(), owned by MSFResult — do not free.
 *
 * Which union field is active depends on `kind`:
 * | kind                     | read this                          |
 * |--------------------------|------------------------------------|
 * | TY_OPTIONAL/ARRAY/SET    | .inner                             |
 * | TY_DICT                  | .dict.key, .dict.value             |
 * | TY_FUNC                  | .func.params, .func.ret            |
 * | TY_TUPLE                 | .tuple.elems, .tuple.labels        |
 * | TY_NAMED                 | .named.name                        |
 * | TY_GENERIC_INST          | .generic.base, .generic.args       |
 */
struct TypeInfo {
  TypeKind kind;
  uint32_t array_fixed_len;                   /**< >0 for fixed-length arrays. */
  union {
    TypeInfo *inner;                           /* OPTIONAL / ARRAY / SET     */
    struct { TypeInfo *key, *value; } dict;    /* DICT                       */
    struct { TypeInfo **params; size_t param_count;
             TypeInfo *ret;
             uint8_t is_async, throws, escaping; } func;
    struct { TypeInfo **elems; const char **labels;
             size_t elem_count; } tuple;
    struct { const char *name; void *decl; } named;
    struct { const char *name; uint32_t index;
             TypeConstraint *constraints;
             uint32_t constraint_count; } param;
    struct { TypeInfo *base; TypeInfo **args;
             uint32_t arg_count; } generic;
    struct { const char *param_name;
             const char *assoc_name; } assoc_ref;
    struct { TypeInfo **protocols;
             uint32_t protocol_count; } composition;
  };
};

/* — Builtin singletons (compare by pointer: node->type == TY_BUILTIN_INT) - */

extern TypeInfo
  *TY_BUILTIN_VOID,   *TY_BUILTIN_BOOL,   *TY_BUILTIN_INT,
  *TY_BUILTIN_STRING, *TY_BUILTIN_DOUBLE,  *TY_BUILTIN_FLOAT,
  *TY_BUILTIN_DATA,   *TY_BUILTIN_SUBSTRING,
  *TY_BUILTIN_UINT,   *TY_BUILTIN_UINT8,   *TY_BUILTIN_UINT16,
  *TY_BUILTIN_UINT32, *TY_BUILTIN_UINT64,
  *TY_BUILTIN_JSONENCODER, *TY_BUILTIN_JSONDECODER;

/* — Type helpers ---------------------------------------------------------- */

/**
 * @brief Canonicalizes a type into a switch-friendly TypeKind.
 *
 * This is the preferred way to inspect types.  Builtin types are
 * singletons (TY_BUILTIN_INT, TY_BUILTIN_STRING, ...) — comparing
 * their .kind field would always give TY_INT for all of them.
 * This function hides that detail so you can write clean switches.
 *
 * @param ty  Type to inspect (NULL is treated as TY_VOID).
 * @return    The canonical TypeKind.
 */
static inline TypeKind type_kind_of(const TypeInfo *ty) {
  if (!ty)                         return TY_VOID;
  if (ty == TY_BUILTIN_VOID)      return TY_VOID;
  if (ty == TY_BUILTIN_BOOL)      return TY_BOOL;
  if (ty == TY_BUILTIN_INT)       return TY_INT;
  if (ty == TY_BUILTIN_DOUBLE)    return TY_DOUBLE;
  if (ty == TY_BUILTIN_FLOAT)     return TY_FLOAT;
  if (ty == TY_BUILTIN_STRING)    return TY_STRING;
  if (ty == TY_BUILTIN_DATA)      return TY_DATA;
  if (ty == TY_BUILTIN_SUBSTRING) return TY_SUBSTRING;
  if (ty == TY_BUILTIN_UINT64)    return TY_UINT64;
  if (ty == TY_BUILTIN_UINT)      return TY_UINT;
  if (ty == TY_BUILTIN_UINT32)    return TY_UINT32;
  if (ty == TY_BUILTIN_UINT16)    return TY_UINT16;
  if (ty == TY_BUILTIN_UINT8)     return TY_UINT8;
  return ty->kind;
}

/**
 * @brief Produces a human-readable string for a type.
 * @param t     Type to stringify (NULL becomes "nil").
 * @param buf   Output buffer.
 * @param size  Buffer size in bytes.
 * @return      Pointer to buf.
 *
 * @code
 * char buf[64];
 * type_to_string(node->type, buf, sizeof(buf));  // "Int", "[String]", "Int?", ...
 * @endcode
 */
const char *type_to_string(const TypeInfo *t, char *buf, size_t size);

/**
 * @brief Shallow type equality (kind + name, ignores generic args).
 * @return Non-zero if equal.
 */
int type_equal(const TypeInfo *a, const TypeInfo *b);

/**
 * @brief Deep type equality (including generic arguments recursively).
 * @return Non-zero if structurally equal.
 */
int type_equal_deep(const TypeInfo *a, const TypeInfo *b);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. AST — the syntax tree                                              │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Walk with first_child / next_sibling.  Read kind + data union.
 *  AST nodes are immutable after analysis.  Do not modify.
 *
 *    AST_FOREACH_CHILD(root, c) {
 *        printf("%s\n", ast_kind_name(c->kind));
 *    }
 */

/* — Node kinds (auto-generated from data/ast_nodes.def) ------------------- */

/** @brief What kind of syntax this node represents. @generated */
typedef enum {
  /* @generated AST node kinds (data/ast_nodes.def via scripts/gen_ast_names.py) */

  /* ── Top-level ─────────────────────────────────────────────── */
  AST_UNKNOWN = 0,
  AST_SOURCE_FILE,

  /* ── Declarations ──────────────────────────────────────────── */
  AST_FUNC_DECL,
  AST_VAR_DECL,
  AST_LET_DECL,
  AST_CLASS_DECL,
  AST_STRUCT_DECL,
  AST_ENUM_DECL,
  AST_PROTOCOL_DECL,
  AST_EXTENSION_DECL,
  AST_IMPORT_DECL,
  AST_TYPEALIAS_DECL,
  AST_INIT_DECL,
  AST_DEINIT_DECL,
  AST_ACTOR_DECL,
  AST_PARAM,
  AST_ATTRIBUTE,
  AST_OWNERSHIP_SPEC,
  AST_THROWS_CLAUSE,
  AST_ACCESSOR_DECL,
  AST_GENERIC_PARAM,
  AST_WHERE_CLAUSE,
  AST_PROTOCOL_REQ,
  AST_CONFORMANCE,
  AST_ENUM_CASE_DECL,
  AST_ENUM_ELEMENT_DECL,
  AST_SUBSCRIPT_DECL,
  AST_PRECEDENCE_GROUP_DECL,
  AST_OPERATOR_DECL,
  AST_PG_HIGHER_THAN,
  AST_PG_LOWER_THAN,
  AST_PG_ASSOCIATIVITY,
  AST_PG_ASSIGNMENT,
  AST_PG_GROUP_REF,

  /* ── Statements ────────────────────────────────────────────── */
  AST_RETURN_STMT,
  AST_THROW_STMT,
  AST_IF_STMT,
  AST_GUARD_STMT,
  AST_FOR_STMT,
  AST_WHILE_STMT,
  AST_REPEAT_STMT,
  AST_DO_STMT,
  AST_SWITCH_STMT,
  AST_CASE_CLAUSE,
  AST_BREAK_STMT,
  AST_CONTINUE_STMT,
  AST_FALLTHROUGH_STMT,
  AST_DEFER_STMT,
  AST_DISCARD_STMT,
  AST_EXPR_STMT,
  AST_BLOCK,

  /* ── Expressions ───────────────────────────────────────────── */
  AST_IDENT_EXPR,
  AST_INTEGER_LITERAL,
  AST_FLOAT_LITERAL,
  AST_STRING_LITERAL,
  AST_REGEX_LITERAL,
  AST_BOOL_LITERAL,
  AST_NIL_LITERAL,
  AST_CALL_EXPR,
  AST_MEMBER_EXPR,
  AST_SUBSCRIPT_EXPR,
  AST_BINARY_EXPR,
  AST_UNARY_EXPR,
  AST_CONSUME_EXPR,
  AST_ASSIGN_EXPR,
  AST_PAREN_EXPR,
  AST_ARRAY_LITERAL,
  AST_DICT_LITERAL,
  AST_TUPLE_EXPR,
  AST_CLOSURE_EXPR,
  AST_CLOSURE_CAPTURE,
  AST_TRY_EXPR,
  AST_AWAIT_EXPR,
  AST_CAST_EXPR,
  AST_OPTIONAL_CHAIN,
  AST_FORCE_UNWRAP,
  AST_TERNARY_EXPR,
  AST_INOUT_EXPR,
  AST_IF_EXPR,
  AST_CATCH_CLAUSE,
  AST_KEY_PATH_EXPR,
  AST_MACRO_EXPANSION,
  AST_AVAILABILITY_EXPR,

  /* ── Types ─────────────────────────────────────────────────── */
  AST_TYPE_IDENT,
  AST_TYPE_OPTIONAL,
  AST_TYPE_ARRAY,
  AST_TYPE_DICT,
  AST_TYPE_TUPLE,
  AST_TYPE_FUNC,
  AST_TYPE_GENERIC,
  AST_TYPE_INOUT,
  AST_TYPE_SOME,
  AST_TYPE_ANY,
  AST_TYPE_COMPOSITION,

  /* ── Patterns ──────────────────────────────────────────────── */
  AST_PATTERN_ENUM,
  AST_PATTERN_TUPLE,
  AST_PATTERN_TYPE,
  AST_PATTERN_GUARD,
  AST_PATTERN_RANGE,
  AST_PATTERN_WILDCARD,
  AST_PATTERN_VALUE_BINDING,
  AST_OPTIONAL_BINDING,

  /* Macros (Swift 5.9) — appended so existing values stay put. */
  AST_MACRO_DECL,

  AST__COUNT
} ASTNodeKind;

/* — The node itself ------------------------------------------------------- */

/** @brief A single node in the abstract syntax tree. */
typedef struct ASTNode ASTNode;
struct ASTNode {
  ASTNodeKind kind;                 /**< What this node is.                   */
  uint32_t    tok_idx, tok_end;     /**< Token range [tok_idx, tok_end).      */

  /* tree links */
  ASTNode  *parent;                 /**< Parent (NULL for root).              */
  ASTNode  *first_child;            /**< First child (NULL if leaf).          */
  ASTNode  *last_child;             /**< Last child (internal).               */
  ASTNode  *next_sibling;           /**< Next sibling (NULL if last).         */

  /* semantic info (populated after msf_analyze) */
  TypeInfo *type;                   /**< Resolved type (NULL before analysis). */
  uint32_t  modifiers;              /**< Declaration modifiers bitmask.       */
  uint32_t  arg_label_tok;          /**< Argument label token (0 if none).    */

  /** @brief Kind-specific payload — check `kind` before reading. */
  union {
    struct { uint32_t name_tok; }                                     func;     /**< FUNC_DECL / INIT_DECL  */
    struct { uint32_t name_tok;
             uint8_t  is_computed, has_getter, has_setter;
             uint8_t  has_will_set, has_did_set;
             ASTNode *getter_body, *setter_body;
             ASTNode *will_set_body, *did_set_body;
             uint8_t  has_wrapper, is_class_var;
             uint32_t wrapper_type_tok;
             ASTNode *wrapper_init;
             uint32_t setter_param_name_tok;
             uint32_t will_set_param_name_tok;
             uint32_t did_set_param_name_tok;
             uint8_t  setter_access, is_async_let; }                  var;      /**< VAR_DECL / LET_DECL    */
    struct { uint32_t op_tok; }                                       binary;   /**< BINARY / ASSIGN / CAST */
    struct { int64_t  ival; }                                         integer;  /**< INTEGER_LITERAL        */
    struct { double   fval; }                                         flt;      /**< FLOAT_LITERAL          */
    struct { uint8_t  bval; }                                         boolean;  /**< BOOL_LITERAL           */
    struct { uint8_t  is_default, has_guard; ASTNode *where_expr; }   cas;      /**< CASE_CLAUSE            */
    struct { void    *captures; }                                     closure;  /**< CLOSURE_EXPR           */
    struct { ASTNode *resolved_callee_decl; }                         call;     /**< CALL_EXPR              */
    struct { uint32_t name_tok; uint8_t kind; }                       aux;      /**< ACCESSOR / MACRO       */
  } data;
};

/**
 * @brief Returns the human-readable name for an AST node kind.
 * @param k  Node kind.
 * @return   Static string: "func_decl", "if_stmt", "call_expr", etc.
 */
const char *ast_kind_name(ASTNodeKind k);

/** @brief Iterates over the children of an AST node.
 *  @code
 *    AST_FOREACH_CHILD(root, child) {
 *        printf("%s\n", ast_kind_name(child->kind));
 *    }
 *  @endcode */
#define AST_FOREACH_CHILD(node, it) \
  for (const ASTNode *it = (node)->first_child; it; it = it->next_sibling)

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. ANALYZE — tokenize, parse, and resolve types in one call           │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  MSFResult *r = msf_analyze("let x = 42", "main.swift");
 *  msf_dump_json(r, stdout);
 *  msf_result_free(r);
 *
 *  ERROR MODEL
 *
 *  - Analysis never fails silently.
 *  - msf_analyze() returns NULL only on allocation failure.
 *  - Syntax and semantic errors are reported via msf_error_* API.
 *  - AST is still produced even if errors exist (best-effort recovery).
 */

/** @brief Opaque result of msf_analyze().  Free with msf_result_free(). */
typedef struct MSFResult MSFResult;

/** @brief Opaque module vocabulary (defined in §5c) — forward-declared so the
 *  analyze prototypes below can take it by pointer. */
typedef struct MSFVocab MSFVocab;

/**
 * @brief Returns the library version string.
 * @return  Static string, e.g. "0.1.0".
 */
const char *msf_version(void);

/**
 * @brief Analyzes Swift source code: tokenize, parse, and resolve all types.
 *
 * This is the main entry point.  One call does everything.
 *
 * The result owns its own copy of @p code and @p filename — the caller may
 * free both buffers as soon as msf_analyze returns.  Tokens, AST source
 * ranges, and diagnostics all point into the owned copy.
 *
 * @param code      NUL-terminated Swift source code.
 * @param filename  File name for diagnostics, or NULL for "\<input\>".
 * @return          Analysis result (opaque).  NULL on allocation failure.
 *                  Free with msf_result_free().
 */
MSFResult *msf_analyze(const char *code, const char *filename);

/**
 * @brief Like msf_analyze(), but with module-level type names predeclared.
 *
 * msf analyzes one file at a time, so a reference to a type declared in a
 * sibling file of the same module — or in an imported module — would otherwise
 * report "use of undeclared type". Pass those type names here (e.g. collected
 * from the module's other files, or from an SDK's `.swiftinterface`) and they
 * are injected into the global scope so such references resolve.
 *
 * @param code               NUL-terminated Swift source code.
 * @param filename           File name for diagnostics, or NULL.
 * @param module_types       Array of type names known to be in scope.
 * @param module_type_count  Number of entries in @p module_types.
 * @return                   Analysis result (opaque). NULL on allocation failure.
 */
MSFResult *msf_analyze_in_module(const char *code, const char *filename,
                                 const char *const *module_types, size_t module_type_count);

/**
 * @brief Like msf_analyze(), but resolves the file's `import X` statements
 *        against a loaded module vocabulary (see §5c).
 *
 * For each `import X`, X's public type names from @p vocab are predeclared, so
 * references resolve with no SDK present.  Falls back to the host's compiled
 * module table for imports the vocabulary doesn't cover.  @p vocab is borrowed.
 *
 * @param code      Swift source (NUL-terminated).  Copied internally.
 * @param filename  File name for diagnostics, or NULL.
 * @param vocab     Loaded vocabulary (msf_vocab_parse / msf_vocab_create), or NULL.
 * @return          Result (free with msf_result_free), or NULL on allocation failure.
 */
MSFResult *msf_analyze_with_vocab(const char *code, const char *filename,
                                  const struct MSFVocab *vocab);

/**
 * @brief Frees all resources held by an analysis result.
 * @param r  Result to free (may be NULL).
 */
void msf_result_free(MSFResult *r);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5b. WHOLE-MODULE — analyze several files as one unit                   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * A Swift module is a SET of files compiled together; a type declared in one
 * file is visible to its siblings.  Analyzing files in isolation reports those
 * sibling types as "undeclared".  MSFModule analyzes them as a unit: all files
 * are parsed, their declarations collected into one shared symbol table, then
 * every file is resolved against it — so cross-file references resolve, with no
 * SDK stubs and no text concatenation (each file keeps its own source/tokens).
 *
 *   MSFModule *m = msf_module_create();
 *   msf_module_add_file(m, codeA, "A.swift");
 *   msf_module_add_file(m, codeB, "B.swift");
 *   msf_module_analyze(m);
 *   for (uint32_t i = 0; i < msf_module_error_count(m); i++) ...
 *   msf_module_free(m);
 */

/** @brief Opaque whole-module analysis unit.  Free with msf_module_free(). */
typedef struct MSFModule MSFModule;

/** @brief Creates an empty module.  NULL on allocation failure. */
MSFModule *msf_module_create(void);

/**
 * @brief Lexes + parses one source file and adds it to the module.
 *
 * Copies @p code (caller may free immediately).  Must be called before
 * msf_module_analyze().
 *
 * @return 0 on success, non-zero on tokenize/allocation failure (file skipped).
 */
int msf_module_add_file(MSFModule *m, const char *code, const char *filename);

/**
 * @brief Attaches a runtime module vocabulary (see §5c) to resolve `import`s.
 *
 * Call before msf_module_analyze().  Each file's `import X` resolves X's types
 * from @p vocab, falling back to the host's compiled module table.  Borrowed.
 *
 * @return 0 on success, non-zero if the module was already analyzed.
 */
int msf_module_set_vocabulary(MSFModule *m, const struct MSFVocab *vocab);

/**
 * @brief Attaches an SDK vocabulary used as a global last-resort fallback.
 *
 * Unlike msf_module_set_vocabulary() (per-import resolution), a type name found
 * in ANY module of @p vocab resolves regardless of imports — SDK types are
 * reachable through re-export chains not modeled per-import.  Keep it separate
 * from the per-import vocabulary so project modules' types stay import-scoped.
 * Borrowed.  Call before msf_module_analyze().
 */
int msf_module_set_sdk_vocabulary(MSFModule *m, const struct MSFVocab *vocab);

/**
 * @brief Runs whole-module semantic analysis over all added files.
 * @return 0 if no diagnostics, 1 if any, negative on error. Call once.
 */
int msf_module_analyze(MSFModule *m);

/** @brief Total semantic diagnostics across the module (after analyze). */
uint32_t msf_module_error_count(const MSFModule *m);

/** @brief Diagnostic message at @p index, or "" if out of range. */
const char *msf_module_error_message(const MSFModule *m, uint32_t index);

/** @brief File path the diagnostic at @p index belongs to (whole-module: the
 *  offending node's origin file), or NULL.  The pointer is owned by the module;
 *  valid until msf_module_free(). */
const char *msf_module_error_file(const MSFModule *m, uint32_t index);

/** @brief 1-based line of the diagnostic at @p index (0 if unknown). */
uint32_t msf_module_error_line(const MSFModule *m, uint32_t index);

/** @brief 1-based column of the diagnostic at @p index (0 if unknown). */
uint32_t msf_module_error_col(const MSFModule *m, uint32_t index);

/** @brief Byte length of the offending range at @p index (0 if unknown). */
uint32_t msf_module_error_length(const MSFModule *m, uint32_t index);

/** @brief Number of files successfully added to the module. */
size_t msf_module_file_count(const MSFModule *m);

/* — Per-file results -------------------------------------------------------- *
 *
 * After msf_module_analyze(), each file's typed AST, source, and token stream
 * remain available individually — the same handles msf_root/source/tokens give
 * for a single MSFResult, but indexed per file.  A backend (e.g. IR generation)
 * can iterate the module's files and lower each file's resolved AST into one
 * shared output.  All pointers are owned by @p m and valid until
 * msf_module_free(); @p i must be < msf_module_file_count(). */

/** @brief File name of file @p i (as passed to msf_module_add_file), or NULL. */
const char *msf_module_file_name(const MSFModule *m, size_t i);

/** @brief Root AST node of file @p i, or NULL if out of range. */
const ASTNode *msf_module_file_root(const MSFModule *m, size_t i);

/** @brief Source buffer of file @p i, or NULL if out of range. */
const Source *msf_module_file_source(const MSFModule *m, size_t i);

/** @brief Token array of file @p i (TOK_EOF-terminated), or NULL. */
const Token *msf_module_file_tokens(const MSFModule *m, size_t i);

/** @brief Token count of file @p i (0 if out of range). */
size_t msf_module_file_token_count(const MSFModule *m, size_t i);

/**
 * @brief Index of the file carrying top-level executable code — the module's
 *        entry (Swift permits this only in main.swift / an @main file), or
 *        SIZE_MAX if the module has none.  Call after msf_module_analyze().
 */
size_t msf_module_entry_file_index(const MSFModule *m);

/**
 * @brief Like msf_parse_expression(), but parses into the module's shared arena.
 *
 * For re-parsing a bare expression (e.g. a string-interpolation segment) while
 * lowering one of the module's files.  The returned node and its tokens live in
 * @p m and need no cleanup; @p out_tokens receives the token array for
 * token_text() (or NULL if unavailable).  Call after msf_module_analyze().
 */
const ASTNode *msf_module_parse_expression(MSFModule *m, const char *expr_text,
                                           const Token **out_tokens);

/** @brief Frees the module and everything it owns (may be NULL). */
void msf_module_free(MSFModule *m);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5c. MODULE VOCABULARY — portable type index from .swiftinterface       │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * A "vocabulary" is the set of public type names a module exports — the data
 * needed to resolve cross-module references (`import SwiftUI` → `View`, `Text`,
 * ...) without the module's compiled form.  msf extracts it by parsing a
 * module's textual `.swiftinterface` with its OWN parser, then serializes it to
 * a compact, portable text format (`.msfvocab`) that ships as a build artifact.
 *
 * This decouples type resolution from the host SDK: the generator runs once on
 * a machine that has the SDK; the resulting `.msfvocab` is then loaded anywhere
 * — browser (WASM), Windows — where no SDK, `xcrun`, or `.swiftinterface` exists.
 *
 *   // Generate (on a machine with the SDK):
 *   MSFVocab *v = msf_vocab_create();
 *   msf_vocab_add_interface(v, "SwiftUI", swiftui_interface_src);
 *   char *text = msf_vocab_serialize(v);          // write to SwiftUI.msfvocab
 *
 *   // Load (anywhere) and resolve against it:
 *   MSFVocab *v = msf_vocab_parse(text);
 *   size_t n; const char *const *names = msf_vocab_module_types(v, "SwiftUI", &n);
 *   MSFResult *r = msf_analyze_in_module(code, "View.swift", names, n);
 */

/** @brief A type's declaration kind within a module vocabulary. */
typedef enum {
  MSF_VOCAB_STRUCT    = 0,
  MSF_VOCAB_CLASS     = 1,
  MSF_VOCAB_ENUM      = 2,
  MSF_VOCAB_PROTOCOL  = 3,
  MSF_VOCAB_ACTOR     = 4,
  MSF_VOCAB_TYPEALIAS = 5,
} MSFVocabKind;

/** @brief A member's declaration kind within a type's vocabulary entry. */
typedef enum {
  MSF_VOCAB_MEMBER_FUNC      = 0,  /**< method / function          */
  MSF_VOCAB_MEMBER_PROPERTY  = 1,  /**< var / let                  */
  MSF_VOCAB_MEMBER_INIT      = 2,  /**< initializer                */
  MSF_VOCAB_MEMBER_SUBSCRIPT = 3,  /**< subscript                  */
  MSF_VOCAB_MEMBER_CASE      = 4,  /**< enum case / element        */
  MSF_VOCAB_MEMBER_TYPEALIAS = 5,  /**< nested typealias           */
} MSFVocabMemberKind;

/* MSFVocab is forward-declared in §5 (opaque; free with msf_vocab_free()). */

/** @brief Creates an empty vocabulary.  NULL on allocation failure. */
MSFVocab *msf_vocab_create(void);

/**
 * @brief Returns the SDK vocabulary compiled into the library.
 *
 * This is generated/sdk_vocab.h (produced by `make sdk-vocab`) parsed into a
 * fresh MSFVocab — the public types of the installed Swift SDK modules, baked
 * in so `import SwiftUI`/`Foundation`/… resolve with no SDK present (browser,
 * Windows).  Empty if the project was built without a generated vocabulary.
 * Caller frees with msf_vocab_free().
 */
MSFVocab *msf_vocab_builtin(void);

/**
 * @brief Materialize types that are only REFERENCED (in a conformance /
 *        superclass clause) but never declared in the vocab.
 *
 * A `.swiftinterface` references ObjC/C-originated SDK types (UICoordinateSpace,
 * OSStatus, CFIndex, …) without declaring them, so the vocab has no entry and
 * Swift uses report "use of undeclared type".  This adds each such name as a
 * type (into a synthetic `__objc_referenced` module).  Called by
 * msf_vocab_builtin(); safe to call on any fully-built vocab.
 *
 * @return Number of types materialized.
 */
size_t    msf_vocab_materialize_referenced_types(MSFVocab *v);

/**
 * @brief Parses one module's `.swiftinterface` source and merges its public
 *        type vocabulary into @p v under @p module_name.
 *
 * Uses msf's own lexer + parser (no sema); tolerant of declarations msf cannot
 * fully parse.  Collects public/open nominal types (struct/class/enum/protocol/
 * actor) and typealiases, including those nested inside types and extensions.
 * Names already present for the module are not duplicated.
 *
 * @return Number of new type names added (0 on parse/allocation failure).
 */
size_t msf_vocab_add_interface(MSFVocab *v, const char *module_name,
                               const char *interface_src);

/**
 * @brief Like msf_vocab_add_interface, but records the module's INTERNAL
 *        (non-private) types/members too, not just public/open ones.
 *
 * Models `@testable import`: a test target sees the imported module's internal
 * declarations.  Pair with msf_vocab_module_type_count + msf_vocab_module_truncate
 * to add a dependency's internals for one importer's analysis and then restore,
 * so internals do not leak to plain importers.
 */
size_t msf_vocab_add_interface_testable(MSFVocab *v, const char *module_name,
                                        const char *interface_src);

/** @brief Number of types currently recorded for a module (by name), 0 if absent. */
size_t msf_vocab_module_type_count(const MSFVocab *v, const char *module_name);

/** @brief Drops a module's types down to @p count (freeing the removed entries);
 *  no-op if the module is absent or already at/below count. */
void msf_vocab_module_truncate(MSFVocab *v, const char *module_name,
                               size_t count);

/**
 * @brief Serializes the vocabulary to the portable `.msfvocab` text format.
 * @return Heap-allocated NUL-terminated text (caller frees with free()), or NULL.
 */
char *msf_vocab_serialize(const MSFVocab *v);

/** @brief Parses `.msfvocab` text into a vocabulary.  NULL on error. */
MSFVocab *msf_vocab_parse(const char *text);

/**
 * @brief Adds a single (name, kind) type to @p module (creating it if needed),
 *        deduplicating by name.  For sources other than `.swiftinterface` — e.g.
 *        type names harvested from Objective-C framework headers.
 * @return 1 if newly added, 0 if duplicate / on failure.
 */
int msf_vocab_add_type(MSFVocab *v, const char *module, const char *name,
                       MSFVocabKind kind);

/** @brief Number of modules in the vocabulary. */
size_t msf_vocab_module_count(const MSFVocab *v);

/** @brief Name of module @p index, or NULL if out of range. */
const char *msf_vocab_module_name(const MSFVocab *v, size_t index);

/** @brief Number of types in module @p index. */
size_t msf_vocab_type_count(const MSFVocab *v, size_t index);

/** @brief Name of type @p t in module @p m, or NULL if out of range. */
const char *msf_vocab_type_name(const MSFVocab *v, size_t m, size_t t);

/** @brief Kind of type @p t in module @p m (meaningful only when name != NULL). */
MSFVocabKind msf_vocab_type_kind(const MSFVocab *v, size_t m, size_t t);

/** @brief Space-joined direct protocol conformances of type @p t in module @p m
 *  (`.msfvocab` v3), or NULL if none recorded. */
const char *msf_vocab_type_conformances(const MSFVocab *v, size_t m, size_t t);

/* — Members (`.msfvocab` v2): per-type method/property/init/… signatures ---- */

/** @brief Number of recorded members of type @p t in module @p m. */
size_t msf_vocab_member_count(const MSFVocab *v, size_t m, size_t t);

/**
 * @brief Signature text of member @p i of type @p t in module @p m, or NULL.
 *
 * The signature is the declaration's source spelling with the body and
 * surrounding whitespace removed, e.g. "addSubview(_ view: UIView)" or
 * "layer: CALayer".  Owned by @p v; valid until msf_vocab_free().
 */
const char *msf_vocab_member_signature(const MSFVocab *v, size_t m, size_t t,
                                       size_t i);

/** @brief Kind of member @p i of type @p t in module @p m. */
MSFVocabMemberKind msf_vocab_member_kind(const MSFVocab *v, size_t m, size_t t,
                                         size_t i);

/**
 * @brief Finds member @p member_name of type @p type_name anywhere in the
 *        vocabulary; returns its signature (borrowed, owned by @p v) and, via
 *        @p out_kind, its kind.  NULL if not found.
 *
 * Matches by the member's name — the signature up to its first '(', ':', '<',
 * '?', '!' or space.  Used to resolve SDK member accesses (`view.layer`) with no
 * SDK present.  When a type has both a property and a same-named method,
 * @p prefer_callable picks the method (a call site `obj.m(...)`) over the
 * property (a plain access `obj.m`); the other is used if the preferred kind is
 * absent.
 */
const char *msf_vocab_find_member(const MSFVocab *v, const char *type_name,
                                  const char *member_name, int prefer_callable,
                                  MSFVocabMemberKind *out_kind);

/* — Dependency graph (`.msfvocab` v2): which modules each module imports ---- */

/** @brief Number of modules that module @p m imports (its dependency edges). */
size_t msf_vocab_module_dep_count(const MSFVocab *v, size_t m);

/** @brief Name of dependency @p d of module @p m, or NULL.  Borrowed by @p v. */
const char *msf_vocab_module_dep(const MSFVocab *v, size_t m, size_t d);

/**
 * @brief Computes the transitive import closure of @p seeds over the dependency
 *        graph — the full set of modules reachable from a project's imports
 *        (e.g. `import SwiftUI` pulls in Foundation, Combine, CoreGraphics, …),
 *        which is exactly the set of vocabularies to load for that project.
 *
 * Writes up to @p out_cap module names (borrowed, owned by @p v) into @p out and
 * returns the closure size — which may exceed @p out_cap, so pass NULL/0 to size
 * first, then call again with a buffer.  Seeds and edges naming a module not
 * present in @p v are skipped.
 */
size_t msf_vocab_import_closure(const MSFVocab *v, const char *const *seeds,
                                size_t seed_count, const char **out,
                                size_t out_cap);

/**
 * @brief Returns the flat array of type names for @p module_name, ready to pass
 *        to msf_analyze_in_module().
 *
 * The array and its strings are owned by @p v and stay valid until
 * msf_vocab_free().  Writes the count to *out_count.
 *
 * @return NULL (and *out_count = 0) if the module is not present.
 */
const char *const *msf_vocab_module_types(const MSFVocab *v,
                                          const char *module_name,
                                          size_t *out_count);

/**
 * @brief Is @p name a public type in ANY module of the vocabulary?
 *
 * A whole-SDK presence check used as a resolution fallback: an SDK type is
 * reachable in real Swift via re-export chains (Foundation → CoreFoundation,
 * CoreLocation → _LocationEssentials, …) that the per-module import view does
 * not model.  Treating the vocabulary as one global name pool resolves such
 * names without enumerating every re-export edge.
 */
int msf_vocab_has_type(const MSFVocab *v, const char *name);

/** @brief Frees the vocabulary and everything it owns (may be NULL). */
void msf_vocab_free(MSFVocab *v);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5d. PROJECT — discover the module graph of an Xcode / SwiftPM project   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Open a project directory and get its modules (one per Xcode target / SwiftPM
 * target), each with its discovered Swift source files — then analyze any module
 * as a unit.  Discovery is generic and reads only the project's own metadata
 * (no project is special-cased): modern Xcode "synchronized folder" targets and
 * SwiftPM `.target/.executableTarget/.testTarget` declarations.
 *
 *   MSFProject *proj = msf_project_open("/path/to/MyApp");
 *   for (size_t i = 0; i < msf_project_module_count(proj); i++) {
 *     MSFModule *m = msf_project_analyze_module(proj, i);   // whole-module
 *     printf("%s: %u diagnostics\n",
 *            msf_project_module_name(proj, i), msf_module_error_count(m));
 *     msf_module_free(m);
 *   }
 *   msf_project_free(proj);
 *
 * Reads the filesystem, so it is a native-host feature; the WebAssembly build
 * (no host filesystem) returns an empty project / NULL module.  Each module is
 * analyzed independently — references to OTHER modules' types appear as
 * undeclared, which a loaded vocabulary (§5c) is meant to supply.
 */

/** @brief Opaque discovered project (module graph).  Free with msf_project_free(). */
typedef struct MSFProject MSFProject;

/**
 * @brief Discovers every Xcode/SwiftPM module under @p root_dir.
 * @return Project handle (possibly with zero modules), or NULL on alloc failure
 *         / unsupported build.  Free with msf_project_free().
 */
MSFProject *msf_project_open(const char *root_dir);

/** @brief Number of discovered modules. */
size_t      msf_project_module_count(const MSFProject *p);

/** @brief Module @p i's name (target name), or NULL if out of range. */
const char *msf_project_module_name(const MSFProject *p, size_t i);

/** @brief Module @p i's kind: "xcode-target", "spm-target", or "spm-test". */
const char *msf_project_module_kind(const MSFProject *p, size_t i);

/** @brief Module @p i's source root directory (absolute), or NULL. */
const char *msf_project_module_dir(const MSFProject *p, size_t i);

/** @brief Number of discovered .swift files in module @p i. */
size_t      msf_project_module_file_count(const MSFProject *p, size_t i);

/** @brief Path of source file @p j in module @p i, or NULL if out of range. */
const char *msf_project_module_file(const MSFProject *p, size_t i, size_t j);

/** @brief Number of in-project modules that module @p i depends on (derived
 *         from its `import` statements that name another discovered module). */
size_t      msf_project_module_dep_count(const MSFProject *p, size_t i);

/** @brief Index of dependency @p k of module @p i, or (size_t)-1 if out of range. */
size_t      msf_project_module_dep(const MSFProject *p, size_t i, size_t k);

/**
 * @brief Index of the project's executable target — the build entry — or
 *        SIZE_MAX if the project has no executable (pure library / unresolved).
 *        Distinguished from libraries by `.executableTarget` in Package.swift.
 */
size_t      msf_project_entry_module(const MSFProject *p);

/**
 * @brief Writes module indices in dependency order (each module after the
 *        modules it depends on) into @p out, up to @p cap entries, and returns
 *        the count written.  This is the order to analyze/compile in so a
 *        module's `import <sibling>` resolves the sibling's already-built types.
 *        Cycles are tolerated (a node already on the DFS stack is not re-entered).
 */
size_t      msf_project_compile_order(const MSFProject *p, size_t *out, size_t cap);

/**
 * @brief Harvests public type NAMES from the project's bundled binary
 *        frameworks (.xcframework ObjC headers, which ship in the repo) into
 *        @p v, each under its framework's module name.
 *
 * Lets `import <Framework>` references resolve with no toolchain, on any OS —
 * the headers travel with the project.  Generic: reads only @interface /
 * @protocol / @class / NS_ENUM / typedef forms (incl. macro-wrapped class
 * names), no per-framework knowledge.  Positive-only — names, not signatures.
 *
 * @return Number of frameworks harvested.
 */
size_t      msf_project_harvest_frameworks(const MSFProject *p, MSFVocab *v);

/**
 * @brief Harvest the public type surface of vendored dependency managers
 *        (CocoaPods `Pods/`, Carthage `Carthage/`) into the vocab.
 *
 * The vendored dependency SOURCE is NOT analyzed as project modules (that would
 * count each dependency's own sema errors against the app); only the public
 * type names are published, so the app's `import <Dep>` references resolve.
 * Harvests the Pods source tree (.swift/.h), Carthage's prebuilt
 * `.framework`s (ObjC Headers + textual `.swiftinterface`), and Carthage's
 * checked-out source (.swift).
 *
 * @return Number of pods + Carthage frameworks/checkouts harvested.
 */
size_t      msf_project_harvest_packages(const MSFProject *p, MSFVocab *v);

/**
 * @brief Harvest the project's OWN Objective-C header type surface into the vocab.
 *
 * Mixed ObjC/Swift apps define types in `.h` headers inside their own modules
 * and use them from Swift via ObjC interop; analysis reads only the `.swift`
 * files, so those ObjC classes are otherwise "undeclared".  Walks each
 * discovered module's directory for `.h` files and publishes their type names
 * (no analysis).  Pass the vocab that serves as the global fallback (the one set
 * via msf_project_set_sdk_vocabulary) so the names resolve unqualified.
 *
 * @return Number of modules scanned.
 */
size_t      msf_project_harvest_own_headers(const MSFProject *p, MSFVocab *v);

/** @brief Number of distinct modules imported across the project that are NOT
 *         project modules — i.e. external/SDK frameworks (SwiftUI, Foundation,
 *         …).  These are the modules a vocabulary (§5c) would supply. */
size_t      msf_project_external_import_count(const MSFProject *p);

/** @brief Name of external import @p k, or NULL if out of range. */
const char *msf_project_external_import(const MSFProject *p, size_t k);

/**
 * @brief Builds and runs whole-module analysis for module @p i (reads its files
 *        from disk).
 * @return Analyzed module — inspect via msf_module_error_*; free with
 *         msf_module_free().  NULL on error / unsupported build.
 */
MSFModule  *msf_project_analyze_module(const MSFProject *p, size_t i);

/**
 * @brief Like msf_project_analyze_module(), but resolves cross-module references
 *        through @p shared_vocab and then publishes module @p i's own public
 *        types into it.
 *
 * Analyze modules in dependency order (see msf_project_module_dep): a module's
 * `import X` resolves X's types from @p shared_vocab once X has been analyzed.
 * Pass the SAME vocabulary (msf_vocab_create()) across the whole sweep; free it
 * after.  This collapses the cross-module "undeclared type" surface.
 *
 * @return Analyzed module (free with msf_module_free()), or NULL.
 */
MSFModule  *msf_project_analyze_module_resolved(const MSFProject *p, size_t i,
                                                MSFVocab *shared_vocab);

/**
 * @brief Sets a global-fallback SDK vocabulary for resolved analysis.
 *
 * Applied to every module analyzed via msf_project_analyze_module_resolved():
 * an SDK type name resolves globally (regardless of imports), while the
 * per-import shared vocabulary keeps project modules' types import-scoped.
 * Borrowed — keep it alive across analysis.
 */
void        msf_project_set_sdk_vocabulary(MSFProject *p, const struct MSFVocab *v);

/** @brief Frees the project and everything it owns (may be NULL). */
void        msf_project_free(MSFProject *p);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  6. READ — inspect the result                                          │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/**
 * @brief Returns the root AST node (AST_SOURCE_FILE).
 * @param r  Analysis result.
 * @return   Root node (always AST_SOURCE_FILE on success), or NULL if r is NULL.
 */
const ASTNode *msf_root(const MSFResult *r);

/**
 * @brief Returns the source descriptor (for dump functions and token_text).
 * @param r  Analysis result.
 * @return   Pointer to the Source.  Owned by r; valid until msf_result_free().
 */
const Source *msf_source(const MSFResult *r);

/**
 * @brief Returns the token array (for dump functions and iteration).
 * @param r  Analysis result.
 * @return   Pointer to the first token.  Owned by r; valid until msf_result_free().
 */
const Token *msf_tokens(const MSFResult *r);

/**
 * @brief Returns the number of tokens in the token array.
 * @param r  Analysis result.
 * @return   Token count (always >= 1 — at minimum TOK_EOF).
 */
size_t msf_token_count(const MSFResult *r);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  7. ERRORS — what went wrong                                             │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Errors are stable for the lifetime of MSFResult.
 *
 *  for (uint32_t i = 0; i < msf_error_count(r); i++)
 *      fprintf(stderr, "%u:%u: %s\n",
 *              msf_error_line(r, i), msf_error_col(r, i),
 *              msf_error_message(r, i));
 */

/**
 * @brief Returns the total number of errors (parser + semantic combined).
 * @param r  Analysis result.
 * @return   Error count (0 means success).
 */
uint32_t msf_error_count(const MSFResult *r);

/**
 * @brief Returns the error message at the given index.
 * @param r  Analysis result.
 * @param i  0-based error index.
 * @return   NUL-terminated message (valid for the lifetime of r).
 */
const char *msf_error_message(const MSFResult *r, uint32_t i);

/**
 * @brief Returns the 1-based line number for an error.
 * @param r  Analysis result.
 * @param i  0-based error index.
 * @return   Line number, or 0 if index is out of range.
 */
uint32_t msf_error_line(const MSFResult *r, uint32_t i);

/**
 * @brief Returns the 1-based column number for an error.
 * @param r  Analysis result.
 * @param i  0-based error index.
 * @return   Column number, or 0 if index is out of range.
 */
uint32_t msf_error_col(const MSFResult *r, uint32_t i);

/**
 * @brief Returns the source byte offset where the error starts (inclusive).
 *
 * For LSP-style highlighting: the [start, end) byte range covers the
 * smallest AST node responsible for the diagnostic. Parser errors
 * currently report the same offset for start and end.
 *
 * @param r  Analysis result.
 * @param i  0-based error index.
 * @return   Byte offset, or 0 if index is out of range.
 */
uint32_t msf_error_start_offset(const MSFResult *r, uint32_t i);

/**
 * @brief Returns the source byte offset where the error ends (exclusive).
 * @param r  Analysis result.
 * @param i  0-based error index.
 * @return   Byte offset, or 0 if index is out of range.
 */
uint32_t msf_error_end_offset(const MSFResult *r, uint32_t i);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  8. DUMP — serialize the AST                                             │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  msf_dump_json(r, stdout);
 *  // {"kind":"source_file","children":[{"kind":"var_decl","value":"x",...}]}
 */

/**
 * @brief Dumps the AST as indented text (human-readable).
 * @param r    Analysis result.
 * @param out  Output stream (e.g. stdout).
 */
void msf_dump_text(const MSFResult *r, FILE *out);

/**
 * @brief Dumps the AST as JSON (for editors, visualizers, web UIs).
 * @param r    Analysis result.
 * @param out  Output stream.
 */
void msf_dump_json(const MSFResult *r, FILE *out);

/**
 * @brief Dumps the AST as an S-expression (for testing and diffing).
 * @param r    Analysis result.
 * @param out  Output stream.
 */
void msf_dump_sexpr(const MSFResult *r, FILE *out);

/* ═════════════════════════════════════════════════════════════════════════
 *
 *                            B A C K E N D   A B I
 *
 * Everything below this line is for compiler backends that turn the typed
 * AST into code.  It exposes the runtime shapes msf_analyze() writes into
 * its output: modifier bit layout, type arena, constraints, substitution
 * helpers, conformance tables.
 *
 * Read-only consumers (editors, linters, pretty-printers) do not need any
 * of this — the sections above are enough.
 *
 * Stability: this is a *backend contract* and its shape changes when msf's
 * type/AST runtime changes.  Pin your msf version.
 *
 * ═════════════════════════════════════════════════════════════════════════ */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  9. MODIFIER BITS — ASTNode.modifiers bitmask                          │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  uint32_t mods = node->modifiers;
 *  if (mods & MOD_STATIC) { ... }
 *
 *  Some bits are reused across unrelated node kinds (e.g. bit 22 is
 *  MOD_TESTABLE_IMPORT on AST_IMPORT_DECL but MOD_CAPTURE_WEAK on
 *  AST_CLOSURE_CAPTURE).  Always qualify by node->kind.
 */

/* — Access control -------------------------------------------------------- */
#define MOD_PUBLIC                        (1u << 0)
#define MOD_PRIVATE                       (1u << 1)
#define MOD_INTERNAL                      (1u << 2)
#define MOD_FILEPRIVATE                   (1u << 3)
#define MOD_OPEN                          (1u << 4)
#define MOD_PACKAGE                       (1u << 30)

/* — Declaration modifiers ------------------------------------------------- */
#define MOD_STATIC                        (1u << 5)
#define MOD_FINAL                         (1u << 6)
#define MOD_OVERRIDE                      (1u << 7)
#define MOD_MUTATING                      (1u << 8)
#define MOD_NONMUTATING                   (1u << 9)
#define MOD_LAZY                          (1u << 10)
#define MOD_WEAK                          (1u << 11)
#define MOD_UNOWNED                       (1u << 12)
#define MOD_INDIRECT                      (1u << 16)
#define MOD_REQUIRED                      (1u << 17)
#define MOD_CONVENIENCE                   (1u << 18)
#define MOD_DYNAMIC                       (1u << 19)

/* — Concurrency ----------------------------------------------------------- */
#define MOD_ASYNC                         (1u << 13)
#define MOD_THROWS                        (1u << 14)
#define MOD_RETHROWS                      (1u << 15)
#define MOD_NONISOLATED                   (1u << 20)
#define MOD_ISOLATED                      (1u << 21)
#define MOD_MAIN_ACTOR                    (1u << 25)

/* — Closure / parameter attributes --------------------------------------- */
#define MOD_ESCAPING                      (1u << 26)
#define MOD_AUTOCLOSURE                   (1u << 27)
#define MOD_VARIADIC                      (1u << 28)

/* — Initializer modifiers ------------------------------------------------- */
#define MOD_FAILABLE                      (1u << 29)
#define MOD_IMPLICITLY_UNWRAPPED_FAILABLE (1u << 30)
#define MOD_SUPPRESSED_CONFORMANCE        (1u << 31)

/* — Import ---------------------------------------------------------------- */
#define MOD_TESTABLE_IMPORT               (1u << 22)

/* — Protocol requirements (context-dependent, share bits with capture) ---- */
#define MOD_PROTOCOL_PROP_SET             (1u << 24)
#define MOD_PROTOCOL_ASSOC_TYPE            (1u << 22)

/* — Closure capture qualifiers (on AST_CLOSURE_CAPTURE nodes) ------------- */
#define MOD_CAPTURE_STRONG                (0u)
#define MOD_CAPTURE_WEAK                  (1u << 22)
#define MOD_CAPTURE_UNOWNED               (1u << 23)
#define MOD_CAPTURE_SAFE                  (1u << 24)

/* — Swift 5.9 parameter ownership (on AST_PARAM; shares capture bits) ----- */
#define MOD_BORROWING                     (1u << 22)
#define MOD_CONSUMING                     (1u << 23)

/* — @Sendable on closure / function type (shares bit 23 with the capture
 *   mode and MOD_CONSUMING, but lives on AST_CLOSURE_EXPR / AST_TYPE_FUNC
 *   nodes — different AST kinds, so the overlap is harmless). */
#define MOD_SENDABLE                      (1u << 23)

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  10. TYPE ARENA — chunked allocator for TypeInfo                       │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  TypeArena arena;
 *  type_arena_init(&arena, 0);
 *  TypeInfo *ti = type_arena_alloc(&arena);
 *  ti->kind = TY_ARRAY;
 *  ti->inner = TY_BUILTIN_INT;
 *  type_arena_free(&arena);
 */

#define TYPE_ARENA_CHUNK_SIZE 1024

typedef struct TypeArenaChunk TypeArenaChunk;
struct TypeArenaChunk {
  TypeInfo       types[TYPE_ARENA_CHUNK_SIZE];
  TypeArenaChunk *next;
};

typedef struct TypeArena {
  TypeArenaChunk *head;
  TypeArenaChunk *tail;
  size_t          count;   /**< Used slots in the tail chunk. */
} TypeArena;

/** @brief Initializes the arena with one empty chunk. */
void      type_arena_init(TypeArena *a, size_t capacity);

/** @brief Frees all chunks and heap-allocated sub-arrays inside types. */
void      type_arena_free(TypeArena *a);

/** @brief Allocates a zeroed TypeInfo.  Returns NULL on OOM. */
TypeInfo *type_arena_alloc(TypeArena *a);

/** @brief Initializes all TY_BUILTIN_* singletons from the arena. */
void      type_builtins_init(TypeArena *a);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  11. TYPE CONSTRAINTS — generic where-clause requirements              │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Stored in TypeInfo.param.constraints[] for TY_GENERIC_PARAM types.
 *
 *  where T: Equatable       → TC_CONFORMANCE
 *  where T == Int           → TC_SAME_TYPE
 *  where T: SomeClass       → TC_SUPERCLASS
 *  where T: ~Copyable       → TC_SUPPRESSED
 *  where T.Item == U.Item   → TC_SAME_TYPE_ASSOC
 */

typedef enum {
  TC_CONFORMANCE,       /**< T: Protocol           */
  TC_SAME_TYPE,         /**< T == ConcreteType     */
  TC_SUPERCLASS,        /**< T: SomeClass          */
  TC_SUPPRESSED,        /**< ~Copyable (opt-out)   */
  TC_SAME_TYPE_ASSOC,   /**< T.Assoc == U.Assoc    */
} TypeConstraintKind;

struct TypeConstraint {
  TypeConstraintKind kind;
  const char        *protocol_name;   /**< For TC_CONFORMANCE.           */
  TypeInfo          *rhs_type;        /**< For TC_SAME_TYPE.             */
  const char        *assoc_name;      /**< For TC_SAME_TYPE_ASSOC (LHS). */
  const char        *rhs_param_name;  /**< For TC_SAME_TYPE_ASSOC (RHS). */
  const char        *rhs_assoc_name;  /**< For TC_SAME_TYPE_ASSOC (RHS). */
};

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  12. GENERIC TYPE SUBSTITUTION                                         │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  TypeSubstitution sub = {0};
 *  type_sub_set(&sub, "T", TY_BUILTIN_INT);
 *  TypeInfo *concrete = type_substitute(generic, &sub, &arena);
 */

#define TYPE_SUB_MAX 16

typedef struct {
  const char *param_name;  /**< Generic parameter name (e.g. "T"). */
  TypeInfo   *concrete;    /**< Concrete type to substitute.       */
} TypeSubEntry;

typedef struct {
  TypeSubEntry entries[TYPE_SUB_MAX];
  uint32_t     count;
} TypeSubstitution;

/** @brief Adds or overwrites a (param → concrete) mapping. */
void      type_sub_set(TypeSubstitution *sub, const char *param,
                       TypeInfo *concrete);

/** @brief Looks up a concrete type for a param name.  Returns NULL if absent. */
TypeInfo *type_sub_lookup(const TypeSubstitution *sub, const char *param_name);

/**
 * @brief Recursively replaces generic params with concrete types.
 *
 * Non-destructive: returns the original pointer if nothing changes.
 * Allocates new TypeInfo from @p arena only when substitution applies.
 */
TypeInfo *type_substitute(const TypeInfo *ty, const TypeSubstitution *sub,
                          TypeArena *arena);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  13. TYPE PREDICATES — small inline helpers                            │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/** @brief Returns 1 if ty is a TY_NAMED type with the given name. */
static inline int type_is_named(const TypeInfo *ty, const char *name) {
  return ty && ty->kind == TY_NAMED && ty->named.name &&
         strcmp(ty->named.name, name) == 0;
}

/** @brief True if ty is `Any`. */
static inline int type_is_any(const TypeInfo *ty) {
  return type_is_named(ty, "Any");
}

/** @brief True if ty is `AnyObject`. */
static inline int type_is_anyobject(const TypeInfo *ty) {
  return type_is_named(ty, "AnyObject");
}

/** @brief True if ty is `Never`. */
static inline int type_is_never(const TypeInfo *ty) {
  return type_is_named(ty, "Never");
}

/** @brief Returns the primary protocol name from a type or composition. */
static inline const char *type_primary_protocol_name(const TypeInfo *ty) {
  if (!ty) return NULL;
  if (ty->kind == TY_NAMED && ty->named.name) return ty->named.name;
  if (ty->kind == TY_PROTOCOL_COMPOSITION &&
      ty->composition.protocol_count > 0 &&
      ty->composition.protocols[0] &&
      ty->composition.protocols[0]->kind == TY_NAMED)
    return ty->composition.protocols[0]->named.name;
  return NULL;
}

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  14. CONFORMANCE TABLE — (type, protocol) records                      │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  Each entry answers: "does type T conform to protocol P?"
 *  Populated by msf's sema pass; backends read it to emit witness
 *  tables or drive protocol dispatch.
 */

/** @brief Initial conformance-table capacity.  Grows on demand. */
#define CONFORMANCE_TABLE_INIT_CAP 64

typedef struct {
  const char *type_name;
  const char *protocol_name;
  const void *where_ast;  /**< AST_WHERE_CLAUSE for conditional conformance. */
} ConformanceRecord;

/** @brief Dynamic conformance table.  Entries are heap-allocated and
 *  grow geometrically; conformance_table_free() releases them. */
typedef struct ConformanceTable {
  ConformanceRecord *entries;
  uint32_t           count;
  uint32_t           capacity;
  /* (internal) msf-private lookup-acceleration index — a (type,protocol)
   * hash over `entries`, maintained by conformance_table_add().  Lets
   * conformance_table_has()/_get_where() answer in O(1) instead of an O(count)
   * scan.  Read-only consumers must ignore this field; it is not part of the
   * stable record data. */
  void              *index;
} ConformanceTable;

/** @brief Register all built-in Swift conformances (Int: Equatable, …). */
void        conformance_table_init_builtins(ConformanceTable *ct);

/** @brief Record an unconditional conformance. */
void        conformance_table_add(ConformanceTable *ct, const char *type_name,
                                  const char *protocol_name);

/** @brief Record a conditional conformance (`extension X: P where T: Q`). */
void        conformance_table_add_conditional(ConformanceTable *ct,
                                              const char *type_name,
                                              const char *protocol_name,
                                              const void *where_ast);

/** @brief Return the where-clause for a recorded conformance, or NULL. */
const void *conformance_table_get_where(const ConformanceTable *ct,
                                        const char *type_name,
                                        const char *protocol_name);

/** @brief Non-zero if the (type, protocol) pair is recorded. */
int         conformance_table_has(const ConformanceTable *ct,
                                  const char *type_name,
                                  const char *protocol_name);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  15. ASSOCIATED-TYPE TABLE — typealias bindings                        │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  (type, protocol, assoc_name) → concrete_type_name
 */

/** @brief Initial associated-type binding capacity.  Grows on demand. */
#define ASSOC_TYPE_TABLE_INIT_CAP 32

typedef struct {
  const char *type_name;
  const char *protocol_name;
  const char *assoc_name;
  const char *concrete_type_name;
} AssocTypeBinding;

/** @brief Dynamic associated-type binding table. */
typedef struct {
  AssocTypeBinding *entries;
  uint32_t          count;
  uint32_t          capacity;
} AssocTypeTable;

void        assoc_type_table_init(AssocTypeTable *at);
void        assoc_type_table_add(AssocTypeTable *at, const char *type_name,
                                 const char *protocol_name,
                                 const char *assoc_name,
                                 const char *concrete_type_name);
const char *assoc_type_table_get(const AssocTypeTable *at,
                                 const char *type_name,
                                 const char *protocol_name,
                                 const char *assoc_name);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  16. BARE EXPRESSIONS — re-parse expression strings                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *  String interpolation lowering:
 *
 *      "Hello, \(person.name)!"
 *
 *  needs a Swift expression parse for the text inside \( … ).  This
 *  helper parses a bare expression against an already-analyzed
 *  MSFResult so backends do not have to ship their own parser or spin
 *  up a fresh MSFResult per interpolation.
 *
 *  The returned ASTNode and its token array live inside @p r and are
 *  released together with @p r by msf_result_free().
 *
 *  @code
 *    const Token *sub_tokens = NULL;
 *    const ASTNode *e = msf_parse_expression(r, "person.name", &sub_tokens);
 *    // walk e with sub_tokens for token_text()
 *  @endcode
 */

/**
 * @brief Parses a bare expression string against an analyzed result.
 * @param r           Analyzed result providing the backing arena.
 * @param expr_text   NUL-terminated Swift expression.
 * @param out_tokens  If non-NULL, receives the token array for the
 *                    sub-expression.  Same lifetime as @p r.
 * @return            Parsed expression node, or NULL on failure.
 */
const ASTNode *msf_parse_expression(MSFResult *r, const char *expr_text,
                                    const Token **out_tokens);

#ifdef __cplusplus
}
#endif
#endif /* MSF_H */
