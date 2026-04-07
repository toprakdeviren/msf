# What Is the Type System and How Does It Work?

> This document explains MiniSwiftFrontend's type module from scratch.
> Previous chapters: [01 — Lexer](01-what-is-a-lexer.md), [02 — AST](02-what-is-an-ast.md), [03 — Parser](03-how-the-parser-works.md)

## What Does the Type Module Do?

The parser gave us an AST:

```
source_file
  func_decl(add)
    param(a: Int)
    param(b: Int)
    return_type(Int)
    binary_expr(+)
      ident_expr(a)
      ident_expr(b)
```

But this tree has no answers for questions like "what is Int?", "can a and b be added?", or "what replaces T in Array<T>?". The **type module** provides the infrastructure to answer these questions.

Every type is represented by a `TypeInfo` struct. These structs live inside an **arena**. The module handles the following tasks:

- Type creation and memory management (arena allocator)
- Exposing builtin types as singletons (`Int`, `String`, `Bool`, ...)
- Comparing two types for equality
- Converting a type to a human-readable string (`[String: Any]`, `(Int) -> Bool`)
- Substituting generic type parameters with concrete types (`T -> Int`)

---

## Place in the Pipeline

```
Token list
     |
     v
+----------+     +----------+     +----------+
|  Parser  |---->|  Type    |---->|   Sema   |
|  (AST)   |     |  <- us   |     |  (check) |
+----------+     +----------+     +----------+
                      |
                      v
                TypeInfo tree
```

The parser builds the AST. Sema (semantic analysis) walks this AST and assigns a type to each node. But to do so, sema needs to **store** type information somewhere, compare types, and transform them. This is exactly where the type module comes in — it is the type infrastructure that sema consumes.

The type module is **never run standalone**. It is consumed by sema. But to understand sema, you first need to understand the type module.

---

## Code Layout

The type module consists of 4 files. Each file does one job:

```
src/type/
  type.c    (153 lines) -- arena allocator + builtin singletons
  equal.c   (123 lines) -- structural type equality: type_equal(), type_equal_deep()
  str.c     (160 lines) -- type_to_string(): TypeInfo -> "Int", "[String: Any]"
  sub.c     (224 lines) -- generic type substitution: T -> Int

src/internal/
  type.h    (187 lines) -- TypeArena, TypeConstraint, TypeSubstitution, predicates
```

Data flow:

```
type.c  (arena + builtins)
  |
  +--- equal.c   (compare two types)
  +--- str.c     (convert type to string)
  +--- sub.c     (replace generic params with concrete types)
  |
  v
consumed by sema
```

---

## TypeInfo and TypeKind

Every type is a `TypeInfo` struct. The `kind` field determines what the type is:

### Primitive Types

| TypeKind | Swift equivalent |
|----------|-----------------|
| `TY_INT` | `Int` |
| `TY_STRING` | `String` |
| `TY_BOOL` | `Bool` |
| `TY_DOUBLE` | `Double` |
| `TY_FLOAT` | `Float` |
| `TY_VOID` | `Void` |
| `TY_UINT`, `TY_UINT8`, ... | `UInt`, `UInt8`, ... |

For primitive types, matching the kind is sufficient — no other fields need to be examined.

### Compound Types

| TypeKind | Swift equivalent | Important fields |
|----------|-----------------|-----------------|
| `TY_OPTIONAL` | `Int?` | `inner` -> wrapped type |
| `TY_ARRAY` | `[Int]` | `inner` -> element type |
| `TY_SET` | `Set<Int>` | `inner` -> element type |
| `TY_DICT` | `[String: Any]` | `dict.key`, `dict.value` |
| `TY_FUNC` | `(Int) -> Bool` | `func.params[]`, `func.ret`, `func.throws`, `func.is_async` |
| `TY_TUPLE` | `(Int, String)` | `tuple.elems[]`, `tuple.labels[]` |

### Named and Generic Types

| TypeKind | Example | Important fields |
|----------|---------|-----------------|
| `TY_NAMED` | `MyStruct` | `named.name`, `named.decl` |
| `TY_GENERIC_PARAM` | `T` | `param.name`, `param.index`, `param.constraints[]` |
| `TY_GENERIC_INST` | `Array<Int>` | `generic.base`, `generic.args[]` |
| `TY_ASSOC_REF` | `T.Element` | `assoc_ref.param_name`, `assoc_ref.assoc_name` |
| `TY_PROTOCOL_COMPOSITION` | `P1 & P2` | `composition.protocols[]` |

---

## Arena Allocator (type.c)

### Problem

Sema produces thousands of types. Calling `malloc()` for each one is slow and leads to memory fragmentation.

### Solution

Same pattern as the AST arena: chunk-based arena. Allocate a large block, place types sequentially, then free everything at once.

```c
#define TYPE_ARENA_CHUNK_SIZE 1024

typedef struct TypeArenaChunk {
  TypeInfo       types[1024];     // flat array — cache-friendly
  TypeArenaChunk *next;           // new chunk when full
} TypeArenaChunk;
```

The alloc function:

```c
TypeInfo *type_arena_alloc(TypeArena *a) {
  if (!a->tail) return NULL;        // arena init failed
  if (a->count >= TYPE_ARENA_CHUNK_SIZE) {
    TypeArenaChunk *new_chunk = calloc(1, sizeof(TypeArenaChunk));
    if (!new_chunk) return NULL;    // OOM — arena stays intact
    a->tail->next = new_chunk;
    a->tail = new_chunk;
    a->count = 0;
  }
  return &a->tail->types[a->count++];  // just increment the index
}
```

Why an arena?
- **Speed:** `malloc()` ~50ns, arena ~2ns (just index++)
- **Cache:** All TypeInfo values sit in contiguous memory — no CPU cache misses
- **Free:** Single operation — walk the chunk list and free everything

### The Special Case in Free

Unlike the AST arena, the type arena **also cleans up heap-allocated sub-arrays** during free. This is because some TypeInfo values hold dynamically allocated arrays:

```c
void type_arena_free(TypeArena *a) {
  TypeArenaChunk *curr = a->head;
  while (curr) {
    size_t limit = (curr == a->tail) ? a->count : TYPE_ARENA_CHUNK_SIZE;
    for (size_t i = 0; i < limit; i++) {
      TypeInfo *t = &curr->types[i];
      if (t->kind == TY_FUNC && t->func.params)
        free(t->func.params);
      else if (t->kind == TY_TUPLE) {
        if (t->tuple.elems)  free(t->tuple.elems);
        if (t->tuple.labels) free(t->tuple.labels);
      } else if (t->kind == TY_GENERIC_PARAM && t->param.constraints)
        free(t->param.constraints);
      else if (t->kind == TY_GENERIC_INST && t->generic.args)
        free(t->generic.args);
      else if (t->kind == TY_PROTOCOL_COMPOSITION && t->composition.protocols)
        free(t->composition.protocols);
    }
    TypeArenaChunk *next = curr->next;
    free(curr);
    curr = next;
  }
  a->head = a->tail = NULL;
  a->count = 0;
}
```

Which sub-arrays each TypeKind can hold is encoded right here. Whenever a new heap field is added, this list must be updated as well — otherwise you get a memory leak.

---

## Builtin Singletons (type.c)

### Problem

Should sema allocate a new TypeInfo with `kind = TY_INT` every time it sees `Int`? And then do a full struct comparison every time it needs to check two types for equality?

### Solution

A **single** TypeInfo is created for each builtin type and stored in a global pointer:

```c
TypeInfo *TY_BUILTIN_VOID   = NULL;
TypeInfo *TY_BUILTIN_INT    = NULL;
TypeInfo *TY_BUILTIN_STRING = NULL;
TypeInfo *TY_BUILTIN_BOOL   = NULL;
TypeInfo *TY_BUILTIN_DOUBLE = NULL;
// ... 15 singletons in total
```

`type_builtins_init()` allocates these from the arena:

```c
void type_builtins_init(TypeArena *a) {
  TypeInfo *t;
  #include "type_builtins.inc"   // @generated — arena alloc + kind set for each
  // unsigned integers (not yet in yaml)
  t = type_arena_alloc(a); t->kind = TY_UINT64; TY_BUILTIN_UINT64 = t;
  t = type_arena_alloc(a); t->kind = TY_UINT;   TY_BUILTIN_UINT   = t;
  // ...
}
```

Now type checking in sema is a pointer comparison:

```c
// Fast: pointer comparison O(1)
if (node->type == TY_BUILTIN_INT) { ... }

// Slow: string comparison O(n)
if (strcmp(node->type->name, "Int") == 0) { ... }  // WE DON'T DO THIS
```

**Singleton advantage:** All "Int" types point to the **same pointer**. The `==` operator is sufficient, `strcmp` is unnecessary.

---

## Type Equality (equal.c)

There are two functions to check whether two types are equal:

### type_equal() — Shallow Comparison

```c
int type_equal(const TypeInfo *a, const TypeInfo *b);
```

Structural equality. Two types are equal if:
- They have the same `kind` **and**
- All sub-components are recursively equal

Special cases:

| TypeKind | How it compares |
|----------|----------------|
| Primitive (`TY_INT`, ...) | Kind match is sufficient |
| `TY_NAMED` | **Pointer equality** — since strings are interned, `a->named.name == b->named.name` |
| `TY_OPTIONAL`, `TY_ARRAY`, `TY_SET` | Recursive comparison of `inner` |
| `TY_DICT` | Recursive comparison of `key` and `value` |
| `TY_FUNC` | Param count, async, throws, all params + ret recursively |
| `TY_TUPLE` | Element count, all elems recursively |
| `TY_GENERIC_PARAM` | Name match (index is **ignored**) |
| `TY_GENERIC_INST` | base + all args recursively |

Why does pointer equality work for `TY_NAMED`? Because sema ensures that all types with the same name have their `named.name` field pointing to the **same interned string**. Not two different "MyStruct" strings, but two pointers to the same address.

### type_equal_deep() — Deep Comparison

```c
int type_equal_deep(const TypeInfo *a, const TypeInfo *b);
```

Same as `type_equal()` with one difference: for `TY_GENERIC_PARAM`, the **index** field is also compared.

When is this needed? To distinguish generic parameters with the same name but at different positions:

```swift
func foo<T, U>(a: T, b: U) where T == U { }
// T.index = 0, U.index = 1
// type_equal:      T == U -> true  (only checks name)
// type_equal_deep: T == U -> false (index differs)
```

### Implementation Detail

Both functions delegate to the same `type_cmp()` function:

```c
static int type_cmp(const TypeInfo *a, const TypeInfo *b, int deep) {
  if (!a || !b) return a == b;       // both NULL -> equal
  if (a->kind != b->kind) return 0;  // different kind -> not equal
  switch (a->kind) {
    // ... recursive comparison
    case TY_GENERIC_PARAM:
      if (strcmp(a->param.name, b->param.name) != 0) return 0;
      return deep ? (a->param.index == b->param.index) : 1;
    default:
      return 1;  // primitive: kind match is sufficient
  }
}
```

---

## Converting a Type to String (str.c)

```c
const char *type_to_string(const TypeInfo *t, char *buf, size_t sz);
```

Converts a TypeInfo tree into a human-readable string. Used by error messages, debug dumps, and the public API.

Examples:

| Input (TypeInfo) | Output (string) |
|-----------------|----------------|
| `TY_INT` | `"Int"` |
| `TY_OPTIONAL { inner: TY_STRING }` | `"String?"` |
| `TY_ARRAY { inner: TY_INT }` | `"[Int]"` |
| `TY_DICT { key: TY_STRING, value: TY_INT }` | `"[String: Int]"` |
| `TY_FUNC { params: [TY_INT], ret: TY_BOOL, is_async: 1 }` | `"(Int) async -> Bool"` |
| `TY_TUPLE { elems: [TY_INT, TY_STRING], labels: ["x", NULL] }` | `"(x: Int, String)"` |
| `TY_GENERIC_INST { base: Array, args: [TY_INT] }` | `"Array<Int>"` |
| `TY_PROTOCOL_COMPOSITION { protocols: [Equatable, Hashable] }` | `"Equatable & Hashable"` |
| `NULL` | `"nil"` |

### How It Works

The caller provides a buffer; the function writes into it:

```c
char buf[128];
printf("type: %s\n", type_to_string(node->type, buf, sizeof(buf)));
// type: (Int, String) -> Bool
```

For recursive types (e.g. `Optional<Array<T>>`), **stack-allocated sub-buffers** are used — no heap allocation:

```c
case TY_OPTIONAL: {
  char inner[128];                                 // sub-buffer on the stack
  type_to_string(t->inner, inner, sizeof(inner));  // recursive call
  snprintf(buf, sz, "%s?", inner);                 // combine
  break;
}
```

Primitive type names are generated via codegen (`type_str.inc`); compound types are hand-written in the `switch`.

---

## Generic Type Substitution (sub.c)

Generic code works like this:

```swift
func identity<T>(_ x: T) -> T { return x }
let result = identity(42)  // T -> Int
```

When sema sees `identity(42)`, it establishes the mapping `T = Int` and replaces every `T` in the function's type tree with `Int`. This is done by `type_substitute()`.

### TypeSubstitution: The Mapping Table

```c
typedef struct {
  TypeSubEntry entries[TYPE_SUB_MAX];  // at most 16 mappings
  uint32_t     count;
} TypeSubstitution;
```

Usage:

```c
TypeSubstitution sub = {0};
type_sub_set(&sub, "T", TY_BUILTIN_INT);       // T -> Int
type_sub_set(&sub, "U", TY_BUILTIN_STRING);    // U -> String
```

`type_sub_set` updates an existing entry with the same name, or adds a new one. `type_sub_lookup` searches by name.

### type_substitute(): Recursive Substitution

```c
TypeInfo *type_substitute(const TypeInfo *ty, const TypeSubstitution *sub,
                          TypeArena *arena);
```

**Critical design decision: Non-destructive.** If nothing changes, the **original pointer is returned**. A new TypeInfo is allocated from the arena only when an actual change occurs.

Example: substituting `T -> Int`, `U -> String` in the type `(T, [U]) -> Bool`:

```
Input:  TY_FUNC { params: [TY_GENERIC_PARAM("T"), TY_ARRAY { inner: TY_GENERIC_PARAM("U") }],
                   ret: TY_BOOL }

Output: TY_FUNC { params: [TY_BUILTIN_INT, TY_ARRAY { inner: TY_BUILTIN_STRING }],
                   ret: TY_BOOL }  <- same pointer, unchanged
```

`TY_BOOL` did not change, so the `ret` field still points to the original pointer. But the `params` array changed, so a new `TY_FUNC` was allocated.

### Switch Cases

There is a separate branch for each compound type:

| TypeKind | What it does |
|----------|-------------|
| `TY_GENERIC_PARAM` | Looks up in `sub`; if found, returns the concrete type |
| `TY_NAMED` | Looks up in `sub` (for type alias substitution) |
| `TY_OPTIONAL`, `TY_ARRAY`, `TY_SET` | Substitutes `inner`; if changed, creates a new TypeInfo |
| `TY_DICT` | Substitutes `key` and `value` |
| `TY_FUNC` | Substitutes all `params[]` and `ret`, tracked via a `changed` flag |
| `TY_GENERIC_INST` | Substitutes all `args[]` |
| `TY_TUPLE` | Substitutes all `elems[]`, **deep-copies labels** |
| Other | Returns the original pointer |

### Tuple Labels Deep-Copy

There is a special case in `TY_TUPLE` substitution: the `labels` array is deep-copied. Why?

The original tuple and the substituted tuple **live in the same arena**. `type_arena_free()` tries to free the `labels` array of both. If they shared the same pointer, it would be a double-free.

```c
// From sub.c:
if (ty->tuple.labels && ty->tuple.elem_count > 0) {
  const char **new_labels = malloc(ty->tuple.elem_count * sizeof(const char *));
  if (new_labels)
    memcpy(new_labels, ty->tuple.labels, ty->tuple.elem_count * sizeof(const char *));
  result->tuple.labels = new_labels;
}
```

---

## Type Predicates (type.h)

Inline functions that simplify frequently asked questions in sema:

```c
type_is_named(ty, "MyStruct")   // Is it TY_NAMED with matching name?
type_is_any(ty)                 // Is it Any?
type_is_anyobject(ty)           // Is it AnyObject?
type_is_never(ty)               // Is it Never?
type_primary_protocol_name(ty)  // First protocol name from a protocol composition
```

These are defined as `static inline` in `src/internal/type.h`. Each call is O(1). Using them instead of `switch` in sema makes the code readable:

```c
// Hard to read:
if (ty && ty->kind == TY_NAMED && ty->named.name && strcmp(ty->named.name, "Never") == 0)

// Easy to read:
if (type_is_never(ty))
```

---

## TypeConstraint System

Generic `where` clauses are represented by `TypeConstraint`:

```swift
func sort<T>(arr: [T]) where T: Comparable { }
//                           ^^^^^^^^^^^^^^
//                           TC_CONFORMANCE: T, protocol_name = "Comparable"
```

### Constraint Kinds

| TypeConstraintKind | Swift equivalent | Example |
|-------------------|-----------------|---------|
| `TC_CONFORMANCE` | `T: Protocol` | `where T: Equatable` |
| `TC_SAME_TYPE` | `T == ConcreteType` | `where T == Int` |
| `TC_SUPERCLASS` | `T: SomeClass` | `where T: UIView` |
| `TC_SUPPRESSED` | `~Copyable` | `where T: ~Copyable` |
| `TC_SAME_TYPE_ASSOC` | `T.Assoc == U.Assoc` | `where T.Element == U.Element` |

### Where Are They Stored?

Constraints are stored inside the `TY_GENERIC_PARAM` type:

```c
struct TypeConstraint {
  TypeConstraintKind kind;
  const char        *protocol_name;   // For TC_CONFORMANCE
  TypeInfo          *rhs_type;        // For TC_SAME_TYPE
  const char        *assoc_name;      // For TC_SAME_TYPE_ASSOC (LHS)
  const char        *rhs_param_name;  // For TC_SAME_TYPE_ASSOC (RHS)
  const char        *rhs_assoc_name;  // For TC_SAME_TYPE_ASSOC (RHS)
};
```

A generic parameter can carry multiple constraints:

```swift
func foo<T: Equatable & Hashable>(x: T) where T: Codable { }
// T.constraints = [
//   TC_CONFORMANCE("Equatable"),
//   TC_CONFORMANCE("Hashable"),
//   TC_CONFORMANCE("Codable"),
// ]
```

The `constraints` array is heap-allocated and cleaned up by `type_arena_free()`.

---

## Usage Example

When all the pieces come together:

```c
#include "internal/type.h"

// 1. Initialize the arena and builtins
TypeArena arena;
type_arena_init(&arena, 0);
type_builtins_init(&arena);

// 2. Create an [Int] type
TypeInfo *arr = type_arena_alloc(&arena);
arr->kind  = TY_ARRAY;
arr->inner = TY_BUILTIN_INT;

// 3. Convert to string
char buf[64];
printf("%s\n", type_to_string(arr, buf, sizeof(buf)));
// [Int]

// 4. Generic substitution: T -> [Int]
TypeInfo *param_t = type_arena_alloc(&arena);
param_t->kind = TY_GENERIC_PARAM;
param_t->param.name = "T";
param_t->param.index = 0;

TypeSubstitution sub = {0};
type_sub_set(&sub, "T", arr);

TypeInfo *result = type_substitute(param_t, &sub, &arena);
printf("%s\n", type_to_string(result, buf, sizeof(buf)));
// [Int]

// 5. Equality check
printf("equal? %d\n", type_equal(result, arr));
// equal? 1

// 6. Clean up
type_arena_free(&arena);
```

---

## Mistakes We Made and Their Solutions

| Mistake | What happened | Solution |
|---------|--------------|----------|
| `strcmp` for builtins | String comparison on every type check — slow | Singleton pointer equality |
| Leak in arena `free` | New heap field added, `free` was not updated | Explicit `free` list for each `kind` |
| Tuple double-free | Original and substituted tuple shared the same `labels` | `labels` deep-copy |
| Destructive `type_sub` | Original type was mutated, sema could not reuse it | Non-destructive: original pointer is preserved |
| 16+ generic params | `TYPE_SUB_MAX` exceeded, silently truncated | 16 is enough — 16+ generic params never occur in practice in Swift |
| Named type `strcmp` | String comparison O(n) in `type_equal` | Interned pointer equality O(1) |
| `type_to_string` heap alloc | Every recursive call did a `malloc` | Stack-allocated sub-buffers |
| Arena init OOM | Dereference after `calloc` failure | NULL check + zeroed arena — `alloc` safely returns NULL |

---

## Next Chapter

-> [05 — What Is Semantic Analysis?](05-semantic-analysis.md)

---

*This document is part of the [msf](https://github.com/toprakdeviren/msf) project.*
