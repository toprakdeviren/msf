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
 *   1. Source     — what you feed in
 *   2. Tokens     — what the lexer produces
 *   3. Types      — what sema resolves
 *   4. AST        — the syntax tree
 *   5. Analyze    — one-call entry point
 *   6. Read       — inspect the result
 *   7. Errors     — what went wrong
 *   8. Dump       — serialize the AST
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
  /* expressions   */ KW_RETURN_VAL, KW_TRY, KW_AWAIT, KW_AS, KW_IS, KW_IN,
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
 * @param code      NUL-terminated Swift source code.
 * @param filename  File name for diagnostics, or NULL for "\<input\>".
 * @return          Analysis result (opaque).  NULL on allocation failure.
 *                  Free with msf_result_free().
 */
MSFResult *msf_analyze(const char *code, const char *filename);

/**
 * @brief Frees all resources held by an analysis result.
 * @param r  Result to free (may be NULL).
 */
void msf_result_free(MSFResult *r);

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

#ifdef __cplusplus
}
#endif
#endif /* MSF_H */
