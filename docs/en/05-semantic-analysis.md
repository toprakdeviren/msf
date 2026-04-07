# What Is Semantic Analysis and How Does It Work?

> This document explains the semantic analysis (sema) layer of msf from the ground up.
> Previous chapters: [01 -- Lexer](01-lexer.md), [02 -- AST](02-ast.md), [03 -- Parser](03-how-the-parser-works.md), [04 -- Type System](04-type-system.md)

## What Does Sema Do?

The parser gave us a tree (AST). But that tree does not yet carry **meaning**:

```
let x = 1 + 2
```

The parser sees it like this:

```
let_decl(x)
  binary_expr(+)
    integer_literal(1)
    integer_literal(2)
```

But there are unanswered questions:
- What is the type of `x`? (`Int`)
- Is the expression `1 + 2` valid? (`Int + Int -> Int` -- yes)
- Has `x` been defined before? (no, first time -- record it)
- Can other code access this name? (access control)

**Semantic analysis** (or **sema** for short) answers these questions. It assigns a type to every AST node, records names, and enforces rules. An AST that passes sema is **meaningful** -- a backend could take it and generate code (or we can display error messages).

In a single line:

```c
#include <msf.h>

MSFResult *r = msf_analyze("let x: Int = 1 + 2", "main.swift");
// r->root->type is now TY_INT
// r->root->first_child->type is also TY_INT (binary_expr)
```

---

## Where It Sits in the Pipeline

```
"let x = 1 + 2"
       |
       v
 +----------+     +----------+     +----------+
 |  Lexer   |---->|  Parser  |---->|   Sema   |
 | (tokens) |     |  (AST)   |     |  <- here |
 +----------+     +----------+     +----------+
                                        |
                                        v
                                   Typed AST
                                   + errors
```

Sema is the **final** stage of the pipeline. The lexer reads character by character, the parser builds a tree from tokens, and sema walks that tree to add **meaning**. After sema, we have an AST where every node has an assigned type, every name is resolved, and every rule is checked.

---

## The Big Picture: Three Passes

Sema is not a single walk -- it performs **three separate passes**. Each pass walks the AST from top to bottom but does a different job:

```
AST (parser output)
       |
       v
 +------------------+
 |  Pass 1: DECLARE |   Record all names (forward declaration)
 +------------------+
       |
       v
 +------------------+
 |  Pass 2: RESOLVE |   Resolve types, check expressions
 +------------------+
       |
       v
 +------------------+
 |  Pass 3: CONFORM |   Verify protocol conformances
 +------------------+
       |
       v
   Typed AST + errors
```

**Why three passes?** Because Swift has forward references. This code is valid:

```swift
func foo() -> Bar { ... }  // Bar is not yet defined
struct Bar { ... }          // but it is defined here
```

If we did a single pass, when we process `foo` there would be nothing called `Bar`. But if the first pass **records all names**, the second pass can find `Bar`.

Why is the third pass separate? Because checking protocol conformance requires that **all types are resolved**. If you wrote `struct Foo: Equatable`, you can only verify that Foo provides the `==` operator after all types have been resolved.

---

## Code Structure

The sema module consists of 17 files. Roughly ~7,640 lines in total:

```
src/semantic/
  private.h             (434 lines) -- SemaContext, Symbol, Scope, all prototypes
  core.c                (708 lines) -- intern pool, symbol table, scope, error reporting
  declare.c             (519 lines) -- Pass 1: forward declaration
  type_resolution.c     (614 lines) -- AST_TYPE_* -> TypeInfo resolution
  conformance.c         (457 lines) -- Builtin member lookup table (~97 entries)
  conformance_table.c   (322 lines) -- Protocol conformance table
  generics.c            (234 lines) -- Generic constraint checking
  builder.c             (308 lines) -- @resultBuilder AST transformation
  module_stubs.h                    -- SDK module type stubs
  resolve/
    resolver.c         (1184 lines) -- Top-level dispatch + sema_analyze entry point
    declaration.c      (1084 lines) -- Declaration type resolution
    access.c            (137 lines) -- Access control: private -> open ranking
    protocol.c          (171 lines) -- Protocol requirement helpers
    expression/
      dispatch.c        (527 lines) -- Expression case dispatcher
      binary.c          (166 lines) -- Binary operators: assignment, comparison, arithmetic, ??
      call.c            (448 lines) -- Call expression: overload resolution, init delegation
      member.c          (239 lines) -- Member access, optional chaining, implicit members
      helpers.c          (90 lines) -- Shared expression helpers: resolve_children, contextual type
```

Data flow:

```
core.c  (intern pool, symbol table, error reporting)
  |
  +--- declare.c          (Pass 1 -- record names)
  |
  +--- type_resolution.c  (AST_TYPE_* -> TypeInfo)
  |
  +--- resolve/
  |      resolver.c  (dispatch)
  |        |
  |        +--- declaration.c        (declaration resolution)
  |        +--- expression/
  |        |      dispatch.c         (expression resolution)
  |        |      call.c             (call resolution)
  |        |      member.c           (member resolution)
  |        |      binary.c           (binary resolution)
  |        |      helpers.c          (shared helpers)
  |        +--- access.c             (access control)
  |        +--- protocol.c           (protocol helpers)
  |
  +--- conformance.c       (builtin member lookup table)
  +--- conformance_table.c (protocol conformance table)
  +--- generics.c          (generic constraint checking)
  +--- builder.c           (@resultBuilder transformation)
```

---

## SemaContext: The Center of Everything

All of sema's state lives in a single struct:

```c
struct SemaContext {
  /* Input (not owned) */
  const Source *src;
  const Token  *tokens;
  ASTArena     *ast_arena;
  TypeArena    *type_arena;

  /* Symbol table */
  Scope      *current_scope;
  InternPool *intern;

  /* Protocol conformance */
  ConformanceTable *conformance_table;
  AssocTypeTable   *assoc_type_table;

  /* Attribute registries */
  WrapperEntry wrapper_types[WRAPPER_TABLE_MAX];
  BuilderEntry builder_types[BUILDER_TABLE_MAX];

  /* Diagnostics */
  uint32_t error_count;
  char     errors[32][256];

  /* Context state (changes during tree walk) */
  TypeInfo *expected_closure_type;
  uint8_t   requires_explicit_self;
  void     *current_func_decl;

  /* Two-phase class initialization */
  uint8_t  in_class_init_phase1;
  uint8_t  init_is_convenience;
  const char *init_own_props[16];
  uint8_t     init_own_assigned[16];

  /* Opaque return type */
  TypeInfo *opaque_return_constraint;
  TypeInfo *opaque_return_first_type;

  /* ... */
};
```

**Key fields:**

| Field | Purpose |
|-------|---------|
| `current_scope` | Head of the scope chain -- name lookups start here |
| `intern` | String interning pool -- all name comparisons use pointer equality |
| `conformance_table` | Conformance records like "Int: Equatable" |
| `error_count` / `errors[]` | Error messages -- sema does not stop, it keeps recording |
| `in_class_init_phase1` | Are we inside a class init? Which properties have been assigned? |
| `expected_closure_type` | Expected type inside a closure (contextual typing) |

---

## String Interning: FNV-1a + NFC

The most frequent operation in sema is name comparison: "is there a symbol called `x`?", "is `Int` a builtin?", "is `foo` a function?". Calling `strcmp` every time would be expensive.

The solution: **string interning**. Each string is added to a pool exactly once, and after that only **pointer comparison** is needed.

```c
const char *a = sema_intern(ctx, "hello", 5);
const char *b = sema_intern(ctx, "hello", 5);
// a == b (same pointer!)  -- no strcmp needed
```

How it works:

1. **Compute an FNV-1a hash** (simple, fast, good distribution):

```c
static uint32_t intern_hash(const char *s, size_t len) {
  uint32_t h = 2166136261u;         // FNV offset basis
  for (size_t i = 0; i < len; i++)
    h = (h ^ (uint8_t)s[i]) * 16777619u;  // FNV prime
  return h;
}
```

2. **Search the open-addressing hash table** (linear probing, load factor 75%):
   - Found -> return the existing pointer
   - Not found -> copy into the buffer, insert into the table, return the new pointer

3. **NFC normalization**: Swift compilers normalize identifiers to NFC. `'e' + U+0301` (combining accent) and `U+00E9` (precomposed) must be the same thing. But for ASCII identifiers (95% of code) there is a quick-check fast path -- zero extra cost.

```c
const char *sema_intern(SemaContext *ctx, const char *str, size_t len) {
  // NFC quick check -- zero overhead for ASCII
  if (!decoder_is_normalized_utf8(str, len, DECODER_NFC)) {
    // normalize...
  }
  // hash + lookup + insert
}
```

**Trade-off:** Interned memory is never reclaimed (pool lifetime = sema lifetime). But for compilers this is acceptable -- everything is freed when analysis finishes.

---

## Symbol Table and Scope Chain

### What Is a Symbol?

Every name is recorded as a `Symbol`:

```c
struct Symbol {
  const char *name;       // interned name (pointer equality)
  SymbolKind  kind;       // SYM_VAR, SYM_FUNC, SYM_CLASS, ...
  TypeInfo   *type;       // resolved type
  ASTNode    *decl;       // declaration node in the AST
  Symbol     *next;       // hash bucket chain
  uint8_t     is_initialized;
  uint8_t     is_deferred;
  uint8_t     is_resolving;  // infinite loop guard
};
```

Why does the `is_resolving` flag exist? Because circular dependencies can occur during type resolution:

```swift
typealias A = B
typealias B = A    // infinite loop!
```

If `is_resolving == 1`, it means "this symbol is currently being resolved, do not re-enter."

### What Is a Scope?

Every lexical scope is a `Scope`:

```c
struct Scope {
  Symbol  *buckets[SCOPE_HASH_SIZE];   // hash buckets
  Scope   *parent;                      // enclosing scope
  uint32_t depth;                       // nesting depth
};
```

**Scope chain**: A chain of nested scopes. When looking up a name, we first check the current scope, then its parent, then its parent's parent, and so on:

```
Scope 3 (for body)     <- current_scope
  |
  v
Scope 2 (func body)
  |
  v
Scope 1 (class body)
  |
  v
Scope 0 (module/global)
```

Lookup:

```c
Symbol *sema_lookup(SemaContext *ctx, const char *name) {
  for (Scope *s = ctx->current_scope; s; s = s->parent) {
    uint32_t h = sym_hash(name);
    for (Symbol *sym = s->buckets[h]; sym; sym = sym->next)
      if (sym->name == name)    // pointer equality! (interned)
        return sym;
  }
  return NULL;
}
```

**Note:** `sym->name == name` -- no `strcmp`! Because both sides are interned. This makes each lookup `O(scope depth * bucket length)`. In practice buckets are short (good hash distribution) and scope depth is around 5-10.

### Scope Push/Pop

When entering a new `{...}` block, `sema_push_scope()` is called; when leaving, `sema_pop_scope()`:

```c
Scope *sema_push_scope(SemaContext *ctx) {
  Scope *s = calloc(1, sizeof(Scope));
  s->parent = ctx->current_scope;
  s->depth = ctx->current_scope ? ctx->current_scope->depth + 1 : 0;
  ctx->current_scope = s;
  return s;
}
```

When a scope is popped, all symbols in the inner scope become inaccessible -- but they remain in memory (arena-style). Why not free them? Because AST nodes may still hold references to those symbols.

---

## Pass 1: Declaration (declare.c)

The first pass walks the AST and **records all names in the symbol table**. It does not resolve types yet -- it just says "something with this name exists."

```c
void declare_node(SemaContext *ctx, ASTNode *node) {
  switch (node->kind) {
    case AST_FUNC_DECL:    declare_named(ctx, node, SYM_FUNC, 0);    break;
    case AST_CLASS_DECL:   declare_named(ctx, node, SYM_CLASS, 1);   break;
    case AST_STRUCT_DECL:  declare_named(ctx, node, SYM_STRUCT, 1);  break;
    case AST_ENUM_DECL:    declare_named(ctx, node, SYM_ENUM, 1);    break;
    case AST_PROTOCOL_DECL: declare_named(ctx, node, SYM_PROTOCOL, 1); break;
    case AST_VAR_DECL:     declare_named(ctx, node, SYM_VAR, 0);    break;
    case AST_LET_DECL:     declare_named(ctx, node, SYM_LET, 0);    break;
    // ...
  }
}
```

Besides recording names, it also performs these tasks:

| Task | Description |
|------|-------------|
| Protocol conformance recording | `struct Foo: Equatable` -> add to conformance table |
| @propertyWrapper registration | `@propertyWrapper struct Lazy` -> add to wrapper table |
| @resultBuilder registration | `@resultBuilder struct VStack` -> add to builder table |
| Class override/final checking | `override func` conflicting with `final func` |
| Enum indirect checking | `indirect enum` circular reference validation |
| Access level validation | `public var` inside `private class` -- warning |

**Why only record names without resolving types?** Because of forward references. During type resolution you may reference other names. If those names have not been recorded yet, resolution will fail. The first pass records all names so the second pass can find them.

---

## Pass 2: Type Resolution

The second pass walks the AST again and this time **assigns a type** to every node.

### Type Resolution Dispatch Table (type_resolution.c)

AST nodes that represent types start with `AST_TYPE_*`: `AST_TYPE_IDENT`, `AST_TYPE_OPTIONAL`, `AST_TYPE_ARRAY`, etc. There is a separate resolver function for each:

```c
typedef TypeInfo *(*TypeResolver)(SemaContext *ctx, const ASTNode *tnode);

static TypeResolver type_resolvers[AST__COUNT];

void init_type_resolvers(void) {
  type_resolvers[AST_TYPE_IDENT]       = resolve_type_ident;
  type_resolvers[AST_TYPE_OPTIONAL]    = resolve_type_optional;
  type_resolvers[AST_TYPE_ARRAY]       = resolve_type_array;
  type_resolvers[AST_TYPE_DICT]        = resolve_type_dict;
  type_resolvers[AST_TYPE_FUNC]        = resolve_type_func;
  type_resolvers[AST_TYPE_TUPLE]       = resolve_type_tuple;
  type_resolvers[AST_TYPE_SOME]        = resolve_type_passthrough;
  type_resolvers[AST_TYPE_ANY]         = resolve_type_passthrough;
  type_resolvers[AST_TYPE_INOUT]       = resolve_type_passthrough;
  type_resolvers[AST_TYPE_GENERIC]     = resolve_type_generic;
  type_resolvers[AST_TYPE_COMPOSITION] = resolve_type_composition;
}
```

This gives `resolve_type_annotation()` O(1) dispatch:

```c
TypeInfo *resolve_type_annotation(SemaContext *ctx, const ASTNode *tnode) {
  TypeResolver fn = type_resolvers[tnode->kind];
  if (fn) return fn(ctx, tnode);
  return NULL;
}
```

**Why a table instead of a switch?** Because a `switch` requires recompilation every time you add a case. The table only requires adding one line to `init_type_resolvers()`. Also, the compiler may not convert a `switch` into a jump table (if the node kinds are not contiguous), but we already build the table ourselves.

**Generic sugar** -- `Array<Int>` is actually the same as `[Int]`. `resolve_type_generic()` handles this:

```
Array<T>        -> TY_ARRAY(T)
Dictionary<K,V> -> TY_DICT(K, V)
Optional<T>     -> TY_OPTIONAL(T)
```

### Node Resolution (resolve/)

`resolve_node()` (resolve/resolver.c) dispatches based on the AST node's kind, calling either `resolve_node_decl()` or `resolve_node_expr()`. These in turn delegate to more specific resolvers.

**Expression type resolution (expression/dispatch.c + expression/*.c):**

The Pratt approach from the parser continues here: types flow **bottom-up**. Leaf nodes (literals, identifiers) know their own types; parent nodes derive their types from their children:

```
binary_expr(+)           <- Int (derived from children)
  integer_literal(1)     <- Int (literal -> TY_INT)
  integer_literal(2)     <- Int (literal -> TY_INT)
```

Resolver dispatch by expression kind:

| Expression kind | File | What it does |
|-----------------|------|-------------|
| Literal (int, string, bool) | dispatch.c | One of the fixed types: TY_INT, TY_STRING, ... |
| Identifier (`x`, `foo`) | dispatch.c | Look up in symbol table, return the type |
| Binary (`+`, `==`, `??`) | binary.c | Result type based on left + right types |
| Call (`foo()`, `Bar()`) | call.c | Overload resolution, generic constraint checking |
| Member (`x.count`, `.none`) | member.c | Builtin member table + user-defined lookup |
| Unary (`!x`, `-x`) | dispatch.c | Result type based on operand type |
| Ternary (`a ? b : c`) | dispatch.c | Common type of b and c |
| Cast (`x as Int`, `x is String`) | dispatch.c | Resolve target type |
| Subscript (`arr[0]`) | dispatch.c | Collection element type |

### Overload Resolution (expression/call.c)

In Swift, multiple functions can have the same name:

```swift
func f(_ x: Int) -> Int { ... }
func f(_ x: String) -> String { ... }

f(42)      // which one?
```

`sema_lookup_overloads()` collects all `SYM_FUNC` symbols, then matching against argument types is performed. If exactly one candidate matches, it is selected; if more than one matches, an ambiguity error is reported.

### Init Delegation

In class initializers, `self.init(...)` and `super.init(...)` calls are handled specially. `call.c` recognizes these cases and enforces the two-phase initialization rules.

---

## Pass 3: Conformance Checking

The third pass checks protocol conformances.

```swift
protocol Equatable {
  static func == (lhs: Self, rhs: Self) -> Bool
}

struct Point: Equatable {
  var x: Int
  var y: Int
  // where is ==? -> ERROR
}
```

`pass3_check_conformances()` follows these steps:

1. Retrieve all records from the conformance table
2. For each record: find the type declaration + the protocol declaration
3. Examine every requirement of the protocol (func, var, associatedtype)
4. Check whether the type satisfies it
5. If not -> error: "Type 'Point' does not conform to protocol 'Equatable'"

**Builtin conformances** (conformance_table.c): When sema starts, conformances for stdlib types are pre-registered:

```
Int    : Equatable, Hashable, Comparable, Numeric, Codable, ...
String : Equatable, Hashable, Comparable, Codable, ...
Array  : Sequence, Collection, MutableCollection, ...
Bool   : Equatable, Hashable, Codable, ...
```

This way, when asking whether `Array<Int>` conforms to `Sequence`, checking the table is sufficient -- there is no need to search for a matching declaration in the AST.

---

## Builtin Member Lookup (conformance.c)

Swift stdlib types have hundreds of members: `Array.count`, `String.isEmpty`, `Int.description`, `Dictionary.keys`, ... Defining all of these in the AST would be impractical.

The solution: **table-driven builtin member lookup**. A table of ~97 entries:

```c
static const BuiltinMemberEntry BUILTIN_MEMBERS[] = {
  { BMK_ARRAY,  "count",      BMR_INT },
  { BMK_ARRAY,  "isEmpty",    BMR_BOOL },
  { BMK_ARRAY,  "first",      BMR_OPT_INNER },
  { BMK_ARRAY,  "append",     BMR_VOID },
  { BMK_STRING, "count",      BMR_INT },
  { BMK_STRING, "isEmpty",    BMR_BOOL },
  { BMK_STRING, "hasPrefix",  BMR_BOOL },
  { BMK_INT,    "description", BMR_STRING },
  { BMK_DICT,   "keys",       BMR_ARRAY_KEY },
  { BMK_DICT,   "values",     BMR_ARRAY_VALUE },
  // ... ~97 entries
};
```

Lookup:

1. Convert the base type to a `BMKind`: `Array` -> `BMK_ARRAY`, `String` -> `BMK_STRING`, ...
2. Scan the table, find the entry matching `(base_kind, name)`
3. Convert the `BMResult` to a `TypeInfo*`: `BMR_INT` -> `TY_BUILTIN_INT`, `BMR_OPT_INNER` -> the optional's inner type, ...

**Why a table instead of `strcmp` chains?** Because:
- Adding a new member is a single line
- The table lives in `.rodata`, cache-friendly
- The `BMKind` enum filters by base type first -- you do not need to scan all ~97 entries, only those belonging to that type

---

## Generic Constraint Checking (generics.c)

Generic types can be constrained:

```swift
func sort<T: Comparable>(_ arr: [T]) -> [T] { ... }

sort([3, 1, 2])    // T = Int, is Int: Comparable? -> yes
sort([{}, {}])     // T = closure, is closure: Comparable? -> NO
```

`generics.c` checks these kinds of constraints:

| Constraint kind | Example | Check |
|----------------|---------|-------|
| Conformance | `T: Equatable` | Look up in conformance table |
| Same-type | `T == Int` | Verify with `type_equal()` |
| Superclass | `T: UIView` | Walk the inheritance chain |
| Suppressed | `~Copyable` | Special case -- inverted logic |
| Conditional | `Array<T>: Equatable where T: Equatable` | Recursively check the where clause |

**Conditional conformance** is special: `Array<Int>: Equatable` is valid because `Int: Equatable`. But `Array<MyType>: Equatable` is valid only if `MyType: Equatable`. For this check, the where clause AST is stored in the conformance table and `check_constraint_satisfaction()` is called recursively during constraint checking.

---

## @resultBuilder Transformation (builder.c)

Result builders, the core mechanism behind SwiftUI, transform a function body into **synthetic AST** nodes:

```swift
@resultBuilder struct VStackBuilder {
  static func buildBlock(_ components: View...) -> View { ... }
  static func buildOptional(_ component: View?) -> View { ... }
  // ...
}

@VStackBuilder
func body() -> View {
  Text("Hello")        // these lines...
  if showDetail {
    Text("Detail")
  }
}
```

Sema transforms this body as follows:

```
// Original:
Text("Hello")
if showDetail { Text("Detail") }

// Transformed:
VStackBuilder.buildBlock(
  Text("Hello"),
  VStackBuilder.buildOptional(showDetail ? Text("Detail") : nil)
)
```

`builder.c` uses these functions:

| Function | Purpose |
|----------|---------|
| `node_get_builder()` | Does the node have a @resultBuilder attribute? |
| `transform_builder_body()` | Transform the body: if -> buildOptional, for -> buildArray, ... |
| `build_block_call_from_stmts()` | Wrap statements into a `buildBlock(...)` call |
| `wrap_in_build_expression()` | Wrap a single expression with `buildExpression(...)` |
| `wrap_builder_method_call()` | Create a generic synthetic method call node |

**Important:** This transformation happens **at the AST level**. New ASTNodes are created with `synth_node()`, existing nodes are duplicated with `clone_node()`. Token indices may be absent (synthetic nodes) -- error messages account for this by using the parent node's position.

---

## Access Control (resolve/access.c)

Swift has 6 access levels:

```
private < fileprivate < internal < package < public < open
```

`access.c` handles the following:

1. **Ranking**: Each modifier is assigned a rank (0-5). `access_rank(MOD_PRIVATE)` -> 0, `access_rank(MOD_OPEN)` -> 5.

2. **Effective access**: A type's effective access level is the minimum of its own modifier and the lowest level of its contents. `public struct Foo { private var x: SecretType }` -- Foo's effective access is `private`.

3. **Protocol member access**: Protocol members always inherit the protocol's access level.

4. **Extension member access**: Extension members inherit the access level of the extension or the extended type.

5. **Private member visibility**: Extensions in the same file can access `private` members (Swift semantics).

---

## Error Reporting: "Did You Mean?"

When sema cannot find a name, it does not just say "undeclared" -- it **suggests a similar name**:

```
error: cannot find 'prnt' in scope; did you mean 'print'?
```

How this works:

1. `sema_find_similar_type_name()` walks the entire scope chain
2. For each symbol it computes `lev_distance()` (Levenshtein edit distance)
3. If the distance is less than 3 (and better than the current best candidate), it is recorded
4. `sema_error_suggest()` appends "did you mean 'X'?" to the error message

```c
int lev_distance(const char *a, const char *b) {
  // Classic DP matrix -- O(m*n)
  // Fast enough for small strings (identifiers are typically <30 chars)
}
```

**Why Levenshtein?** Because most typos are within 1-2 edit distance: a missing letter, an extra letter, two letters swapped. Levenshtein measures exactly that.

---

## Two-Phase Class Initialization

Swift class initializers have special rules:

```swift
class Base {
  var x: Int
  init() {
    x = 0           // Phase 1: assign your own properties
    super.init()     // call super.init
    doSomething()    // Phase 2: you can now use self
  }
}
```

These fields in SemaContext enforce these rules:

```c
uint8_t  in_class_init_phase1;        // Are we in Phase 1?
const char *init_own_props[16];       // This class's stored property names
uint8_t     init_own_assigned[16];    // Which ones have been assigned?
```

In Phase 1, `self` cannot be used (before super.init is called). All stored properties must be assigned before super.init. These rules are enforced in `resolve/declaration.c` during init resolution.

---

## Closure Capture Analysis

Closures can capture variables from an outer scope:

```swift
var counter = 0
let inc = { counter += 1 }  // counter is captured
```

`identify_captures()` (resolve/resolver.c) follows these steps:

1. Collect all identifiers in the closure body
2. Subtract the closure's own local variables
3. Look up the remainder in the outer scope
4. Those found are recorded as **captures**

```c
typedef struct {
  const char  *name;
  CaptureMode  mode;    // strong, weak, unowned, value
  TypeInfo    *type;
  int          is_outer;
} CaptureInfo;
```

The capture mode is determined from the capture list: `[weak self]`, `[unowned self]`, `[x]` (value capture).

---

## Problems We Encountered and Their Solutions

| Problem | What happened | Solution |
|---------|--------------|----------|
| Single pass | Could not resolve forward references | Three-pass architecture: declare -> resolve -> conform |
| strcmp everywhere | Profiling showed 15% of time spent on string comparison | String interning (FNV-1a) -- pointer equality |
| Switch-case dispatch | 30+ case switch for type resolution | Dispatch table: `type_resolvers[kind]` -- O(1) |
| Hardcoded builtin members | 50 lines of if-else for each new member | ~97-entry table-driven lookup |
| Scope leak | Un-popped scopes caused memory leaks | Push/pop always paired -- `declare_in_scope()` helper |
| Infinite loop | `typealias A = B; typealias B = A` | `is_resolving` flag as a recursion guard |
| Vague error messages | "unknown type" -- but which type? | Levenshtein + "did you mean?" suggestion |
| Class init rules | Use of self in Phase 1 was not caught | `in_class_init_phase1` + property tracking |
| Conformance table overflow | Silent loss beyond 256 entries | `CONFORMANCE_TABLE_MAX` guard + warning |
| Missing tokens in builder transform | Synthetic nodes had tok_idx of 0 | Error messages use the parent node's position |
| NFC normalization | `cafe\u0301` and `cafe` interned differently | NFC quick-check + normalization |
| Conditional conformance | `Array<Int>: Equatable` could not be checked | Where clause AST stored in conformance table |

---

## Next Steps

Sema is the final stage of the msf pipeline. The typed AST + error list produced by sema is ready to be consumed by the caller:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", "main.swift");

// Walk the typed AST
msf_dump_text(r, stdout);

// Read errors
for (uint32_t i = 0; i < msf_error_count(r); i++)
    printf("%u:%u: %s\n",
           msf_error_line(r, i),
           msf_error_col(r, i),
           msf_error_message(r, i));

msf_result_free(r);
```

---

*This document is part of the [msf](https://github.com/toprakdeviren/msf) project.*
