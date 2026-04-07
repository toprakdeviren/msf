# What Is an AST and How Does It Work?

> This document explains the AST layer of MiniSwiftFrontend from scratch.
> Previous chapter: [01 -- What Is a Lexer?](01-lexer.md)

## The Big Picture

We have a Swift source file. To analyze it we run it through three stages:

```
"let x = 1 + 2"
       |
       v
 +----------+     +----------+     +----------+
 |  Lexer   |---->|  Parser  |---->|   Sema   |
 | (tokens) |     |  (AST)   |     |  (types) |
 +----------+     +----------+     +----------+
```

1. **Lexer** -- splits source code into tokens: `let`, `x`, `=`, `1`, `+`, `2`
2. **Parser** -- arranges the tokens into a **tree** structure: the **AST**
3. **Sema** -- assigns a type to every node: `x` -> `Int`, `1 + 2` -> `Int`

A library user performs all three steps in a single call:

```c
#include <msf.h>

MSFResult *r = msf_analyze("let x = 1 + 2", "main.swift");
```

Below we explain how each step works internally. This chapter focuses on the output of step 2 -- the AST itself.

---

## What Is an AST?

AST = **Abstract Syntax Tree**.

When you write `let x = 1 + 2` the parser produces this tree:

```
source_file
  +-- let_decl(x)
        +-- type_ident(Int)        <-- type (if present)
        +-- binary_expr(+)         <-- init expression
              +-- integer_literal_expr(1)
              +-- integer_literal_expr(2)
```

Every box is an **ASTNode**. Each node has:
- A **kind** -- what it is (`AST_FUNC_DECL`, `AST_IF_STMT`, `AST_CALL_EXPR`, ...)
- **Children** -- sub-nodes (linked list: `first_child -> next_sibling -> ...`)
- An optional **type** -- filled in by the sema stage (`TypeInfo*`)

**Why "Abstract"?** Parentheses, semicolons, and whitespace from the source code are absent from the tree. Only semantically meaningful structure remains. `(1 + 2)` and `1 + 2` produce the same tree.

To see it in action:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", NULL);
msf_dump_text(r, stdout);    // indented tree
msf_dump_json(r, stdout);    // JSON -- editors, web UI
msf_dump_sexpr(r, stdout);   // S-expression -- testing, diffing
msf_result_free(r);
```

---

## Code Layout

AST-related code lives in two files:

```
src/
  ast/
    ast.c        -- memory management + tree operations  (~275 lines)
    ast_dump.c   -- serialization (text, JSON, S-expr)   (~430 lines)
```

Why two files? **Single Responsibility Principle.** `ast.c` allocates memory and links nodes. `ast_dump.c` reads nodes and prints them. If you ever need a new output format (GraphViz, XML) you only touch `ast_dump.c`.

We did not start with this split -- everything lived in one file. Then we noticed that 60% of the code was printing logic. After the split both files became far more readable.

Real-world compilers follow the same pattern:
- **LLVM:** `AST.cpp` vs `ASTDumper.cpp`
- **Clang:** `Decl.cpp` vs `ASTDumper.cpp` + `JSONNodeDumper.cpp`
- **Swift compiler:** `ASTContext.cpp` vs `ASTDumper.cpp`

---

## `ast.c` -- Infrastructure

This file is the AST's **skeleton**. It allocates nodes, links them into a tree, and looks up kind names. No printing, no formatting, no I/O. ~275 lines. Not a single `fprintf`.

### Arena Allocator

**Problem:** The parser creates thousands of nodes. Calling `malloc()` for each one is slow and causes memory fragmentation.

**Solution:** A chunk-based arena. Allocate a large block, place nodes sequentially, free everything at once when done.

```c
#define AST_ARENA_CHUNK_SIZE 1024

typedef struct ASTArenaChunk {
  ASTNode nodes[1024];       // flat array -- cache-friendly
  ASTArenaChunk *next;       // when full, link a new chunk
} ASTArenaChunk;
```

The allocation function is remarkably simple:

```c
ASTNode *ast_arena_alloc(ASTArena *a) {
  if (!a->tail) return NULL;
  if (a->count >= AST_ARENA_CHUNK_SIZE) {
    // Chunk is full -- append a new one
    ASTArenaChunk *chunk = calloc(1, sizeof(ASTArenaChunk));
    if (!chunk) return NULL;
    a->tail->next = chunk;
    a->tail = chunk;
    a->count = 0;
  }
  return &a->tail->nodes[a->count++];  // just increment the index
}
```

Why an arena?
- **Speed:** `malloc()` ~50 ns, arena ~2 ns (just index++)
- **Cache locality:** All nodes sit in contiguous memory -- minimal CPU cache misses
- **Free:** One operation -- walk the chunk list and free each chunk. No per-node free calls.

**Key design decision:** The arena does **not** know about the contents of the nodes. It only hands out memory and reclaims it. Some nodes hold heap-allocated pointers (e.g. closure capture lists). Those are cleaned up by a separate `ast_arena_cleanup_payloads()` function. This keeps the allocator and the business logic decoupled.

We originally got this wrong -- cleanup code was inside the arena's `free` function. Every time we added a new heap-allocated field we had to modify the arena. The allocator should not be aware of node semantics.

### Tree Operations

Appending a child to a node is O(1):

```c
void ast_add_child(ASTNode *parent, ASTNode *child) {
  if (!parent || !child) return;
  child->parent = parent;
  child->next_sibling = NULL;

  if (!parent->first_child) {
    parent->first_child = child;
    parent->last_child = child;
  } else {
    parent->last_child->next_sibling = child;
    parent->last_child = child;
  }
}
```

The `last_child` pointer eliminates the need to traverse the list to find the tail. Without it every append would be O(n).

### Kind Name Lookup

Every `ASTNodeKind` enum value has a human-readable name:

```c
static const char *const AST_KIND_NAMES[] = {
    /*   0 */ "unknown",
    /*   1 */ "source_file",
    /*   2 */ "func_decl",
    /*   3 */ "var_decl",
    // ... 100+ entries ...
};

const char *ast_kind_name(ASTNodeKind k) {
  if (k < 0 || k >= AST__COUNT) return "?";
  return AST_KIND_NAMES[k];  // O(1) array index
}
```

A compile-time assertion ensures the table stays in sync with the enum:

```c
_Static_assert(sizeof(AST_KIND_NAMES) / sizeof(AST_KIND_NAMES[0]) == AST__COUNT,
    "AST_KIND_NAMES size != AST__COUNT");
```

---

## `ast_dump.c` -- Serialization

This file is a pure **consumer** of the AST -- it reads nodes and prints them. Three formats:

| Format | When to use | Example |
|--------|-------------|---------|
| **Text** | Debug, terminal | `func_decl(main)\n  brace_stmt\n    ...` |
| **JSON** | Web UI, Monaco editor | `{"kind":"func_decl","value":"main","children":[...]}` |
| **S-expr** | Testing, tooling, diffing | `(func_decl "main" (brace_stmt ...))` |

### Table-Driven Design

**Problem:** 3 formats x 100+ node kinds = do we write 300+ switch cases?

**No.** Each node kind maps to a "dump mode":

```c
typedef enum {
  DUMP_PLAIN,       // no payload
  DUMP_FUNC_NAME,   // function name
  DUMP_VAR_NAME,    // variable name
  DUMP_DOT_NAME,    // member expression (.name)
  DUMP_AT_NAME,     // attribute (@name)
  DUMP_TOK_IDX,     // identifier, type, import token
  DUMP_OP,          // operator
  DUMP_INT_LIT,     // integer value
  DUMP_FLOAT_LIT,   // floating-point value
  DUMP_STRING_LIT,  // string/regex literal token
  DUMP_BOOL_LIT,    // boolean
  DUMP_CASE_CLAUSE, // case clause + optional "default" annotation
  DUMP_KEYPATH,     // key path expression
} DumpMode;

static const DumpMode dump_modes[AST__COUNT] = {
  [AST_FUNC_DECL]       = DUMP_FUNC_NAME,
  [AST_VAR_DECL]        = DUMP_VAR_NAME,
  [AST_BINARY_EXPR]     = DUMP_OP,
  [AST_INTEGER_LITERAL]  = DUMP_INT_LIT,
  // ...
};
```

Then a single `extract_value()` function pulls the payload, and each format just renders it:

```c
NodeValue v = extract_value(node, src, tokens);
// -> v.kind = VAL_STRING, v.text = "main"
// -> v.kind = VAL_INT, v.ival = 42
```

Adding a new node kind = one line in the table. Adding a new output format = writing a new renderer. The two dimensions are independent.

---

## The `ASTNode` Structure

```c
struct ASTNode {
  ASTNodeKind kind;        // what it is: FUNC_DECL, VAR_DECL, IF_STMT, ...
  uint32_t    tok_idx;     // index of the first token in source
  uint32_t    tok_end;     // last token (for error messages)

  // Tree links
  ASTNode *parent;         // parent node
  ASTNode *first_child;    // first child
  ASTNode *last_child;     // last child (enables O(1) append)
  ASTNode *next_sibling;   // next sibling

  // Semantic info (populated after sema)
  TypeInfo *type;          // NULL -> sema has not run yet
  uint32_t  modifiers;     // public, static, async, ...
  uint32_t  arg_label_tok; // argument label in a function call

  // Kind-specific data -- which field is active depends on `kind`
  union {
    struct { uint32_t name_tok; }                                     func;     // FUNC_DECL
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
             uint8_t  setter_access, is_async_let; }                  var;      // VAR_DECL / LET_DECL
    struct { uint32_t op_tok; }                                       binary;   // BINARY / ASSIGN / CAST
    struct { int64_t  ival; }                                         integer;  // INTEGER_LITERAL
    struct { double   fval; }                                         flt;      // FLOAT_LITERAL
    struct { uint8_t  bval; }                                         boolean;  // BOOL_LITERAL
    struct { uint8_t  is_default, has_guard; ASTNode *where_expr; }   cas;      // CASE_CLAUSE
    struct { void    *captures; }                                     closure;  // CLOSURE_EXPR
    struct { ASTNode *resolved_callee_decl; }                         call;     // CALL_EXPR
    struct { uint32_t name_tok; uint8_t kind; }                       aux;      // ACCESSOR / MACRO
  } data;
};
```

**Why a linked list for children, not an array?**
- The parser does not know how many children a node will have while building the tree
- Linked-list append is O(1); array append is O(n) due to `realloc`
- Tree depth is shallow (typically 5-10 levels) so traversal cost is low

**Why a union?**
All kind-specific fields share the same memory region. This keeps `ASTNode` compact. Without the union the struct would be substantially larger.

But **be careful**: writing to the wrong union field corrupts another. We hit this in production -- a multi-trailing closure path wrote to `data.binary.op_tok`, which overwrote `data.closure.captures`, and the compiler crashed with `SIGSEGV`.

**Lesson:** If `node->kind == AST_CLOSURE_EXPR`, **never** touch `node->data.binary`.

**Why `tok_idx` instead of copying strings?**
Nodes do not copy text from the source. They store the index into the token stream:

```c
node->data.func.name_tok = 42;  // token #42 = "main"
// When the name is needed: tokens[42].pos -> offset into source
```

Advantage: Zero allocation, and original line/column information is preserved.
Trade-off: The source code and token stream must stay in memory as long as the AST is alive.

---

## Lessons Learned from Mistakes

Mistakes we made while building this project and how we fixed them:

| Mistake | What happened | Fix |
|---------|---------------|-----|
| `exit()` in arena | OOM killed the entire process | Arena is left zeroed; `alloc` returns NULL |
| `perror()` in arena | Wrote to stderr from a library | A library should not write to stderr -- removed |
| Cleanup inside arena | Every new heap field required modifying the arena | Separate `cleanup_payloads()` function |
| Union clash | `data.binary` and `data.closure` overlapped | Moved shared field to `arg_label_tok` outside the union |
| Everything in one file | `ast.c` was 60% print code | Split into `ast.c` + `ast_dump.c` |

---

## Using the AST as a Library Consumer

```c
#include <msf.h>

// 1. Analyze
MSFResult *r = msf_analyze("func add(a: Int, b: Int) -> Int { return a + b }", NULL);

// 2. Check for errors
for (uint32_t i = 0; i < msf_error_count(r); i++)
    fprintf(stderr, "%u:%u: %s\n",
            msf_error_line(r, i), msf_error_col(r, i),
            msf_error_message(r, i));

// 3. Walk the tree
const ASTNode *root = msf_root(r);
for (const ASTNode *c = root->first_child; c; c = c->next_sibling)
    printf("  %s\n", ast_kind_name(c->kind));  // "func_decl"

// 4. Dump as JSON
msf_dump_json(r, stdout);

// 5. Read type information
char buf[64];
printf("type: %s\n", type_to_string(root->first_child->type, buf, sizeof(buf)));

// 6. Clean up
msf_result_free(r);
```

Note: You never create the arena, token stream, parser, or sema context yourself. `msf_analyze()` runs the entire pipeline, and `msf_result_free()` releases everything.

---

## Next Chapter

-> [03 -- Parser: From Tokens to AST](03-parser.md)

---

*This document is part of the [msf](https://github.com/toprakdeviren/msf) project.*
