# Type Sistemi Nedir ve Nasil Calisir?

> Bu belge MiniSwiftFrontend'in type modülünü sifirdan acikliyor.
> Onceki bolumler: [01 — Lexer](01-lexer-nedir.md), [02 — AST](02-ast-nedir.md), [03 — Parser](03-parser-nasil-calisir.md)

## Type Modulu Ne Yapar?

Parser bize bir AST verdi:

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

Ama bu agacta "Int nedir?", "a ile b toplanabilir mi?", "Array<T>'deki T yerine ne gelecek?" gibi sorularin cevabi yok. **Type modulu** bu sorulari cevaplayacak altyapiyi saglar.

Her tip bir `TypeInfo` struct'i ile temsil edilir. Bu struct'lar bir **arena** icerisinde yasarlar. Modül su isleri yapar:

- Tip olusturma ve bellek yonetimi (arena allocator)
- Builtin tiplerin singleton olarak sunulmasi (`Int`, `String`, `Bool`, ...)
- Iki tipin esit olup olmadiginin karsilastirilmasi
- Bir tipin insan-okunur string'e donusturulmesi (`[String: Any]`, `(Int) -> Bool`)
- Generic tip parametrelerinin somut tiplerle yer degistirilmesi (`T -> Int`)

---

## Pipeline'daki Yeri

```
Token listesi
     |
     v
+----------+     +----------+     +----------+
|  Parser  |---->|  Type    |---->|   Sema   |
|  (AST)   |     |  <- biz  |     |  (check) |
+----------+     +----------+     +----------+
                      |
                      v
                TypeInfo agaci
```

Parser AST'yi insa eder. Sema (semantic analysis) bu AST'yi dolasar ve her node'a tip atamasi yapar. Ama sema bunu yaparken tip bilgisini **bir yerde** tutmasi, karsilastirmasi, donusturmesi gerekir. Iste type modulu tam burada devreye girer — sema'nin kullandigi tip altyapisi.

Type modulu **tek basina** calistirilmaz. Sema tarafindan tuketilir. Ama sema'yi anlamak icin once type modülünü anlamak gerekir.

---

## Kod Yapisi

Type modulu 4 dosyadan olusur. Her dosya tek bir is yapar:

```
src/type/
  type.c    (153 satir) -- arena allocator + builtin singleton'lar
  equal.c   (123 satir) -- yapisal tip esitligi: type_equal(), type_equal_deep()
  str.c     (160 satir) -- type_to_string(): TypeInfo -> "Int", "[String: Any]"
  sub.c     (224 satir) -- generic tip ikamesi: T -> Int

src/internal/
  type.h    (187 satir) -- TypeArena, TypeConstraint, TypeSubstitution, predicate'ler
```

Veri akisi:

```
type.c  (arena + builtins)
  |
  +--- equal.c   (iki tipi karsilastir)
  +--- str.c     (tipi string'e donustur)
  +--- sub.c     (generic parametreleri somut tiplerle degistir)
  |
  v
sema tarafindan tuketilir
```

---

## TypeInfo ve TypeKind

Her tip bir `TypeInfo` struct'idir. `kind` alani tipin ne oldugunu belirler:

### Primitif Tipler

| TypeKind | Swift karsiligi |
|----------|----------------|
| `TY_INT` | `Int` |
| `TY_STRING` | `String` |
| `TY_BOOL` | `Bool` |
| `TY_DOUBLE` | `Double` |
| `TY_FLOAT` | `Float` |
| `TY_VOID` | `Void` |
| `TY_UINT`, `TY_UINT8`, ... | `UInt`, `UInt8`, ... |

Primitif tipler icin kind eslesmesi yeterlidir — baska alana bakmaya gerek yok.

### Bilesik Tipler

| TypeKind | Swift karsiligi | Onemli alanlar |
|----------|----------------|----------------|
| `TY_OPTIONAL` | `Int?` | `inner` -> ic tip |
| `TY_ARRAY` | `[Int]` | `inner` -> eleman tipi |
| `TY_SET` | `Set<Int>` | `inner` -> eleman tipi |
| `TY_DICT` | `[String: Any]` | `dict.key`, `dict.value` |
| `TY_FUNC` | `(Int) -> Bool` | `func.params[]`, `func.ret`, `func.throws`, `func.is_async` |
| `TY_TUPLE` | `(Int, String)` | `tuple.elems[]`, `tuple.labels[]` |

### Isimli ve Generic Tipler

| TypeKind | Ornek | Onemli alanlar |
|----------|-------|----------------|
| `TY_NAMED` | `MyStruct` | `named.name`, `named.decl` |
| `TY_GENERIC_PARAM` | `T` | `param.name`, `param.index`, `param.constraints[]` |
| `TY_GENERIC_INST` | `Array<Int>` | `generic.base`, `generic.args[]` |
| `TY_ASSOC_REF` | `T.Element` | `assoc_ref.param_name`, `assoc_ref.assoc_name` |
| `TY_PROTOCOL_COMPOSITION` | `P1 & P2` | `composition.protocols[]` |

---

## Arena Allocator (type.c)

### Problem

Sema binlerce tip uretir. Her biri icin `malloc()` cagirmak yavas ve bellek parcalanmasina yol acar.

### Cozum

AST arena ile ayni pattern: chunk-based arena. Buyuk bir blok al, tipleri sirayla yerlestir, sonunda hepsini bir kerede serbest birak.

```c
#define TYPE_ARENA_CHUNK_SIZE 1024

typedef struct TypeArenaChunk {
  TypeInfo       types[1024];     // duz array — cache-friendly
  TypeArenaChunk *next;           // dolu olunca yeni chunk
} TypeArenaChunk;
```

Alloc fonksiyonu:

```c
TypeInfo *type_arena_alloc(TypeArena *a) {
  if (!a->tail) return NULL;        // arena init basarisiz
  if (a->count >= TYPE_ARENA_CHUNK_SIZE) {
    TypeArenaChunk *new_chunk = calloc(1, sizeof(TypeArenaChunk));
    if (!new_chunk) return NULL;    // OOM — arena bozulmaz
    a->tail->next = new_chunk;
    a->tail = new_chunk;
    a->count = 0;
  }
  return &a->tail->types[a->count++];  // sadece index arttir
}
```

Neden arena?
- **Hiz:** `malloc()` ~50ns, arena ~2ns (sadece index++)
- **Cache:** Tum TypeInfo'lar bitisik bellekte — CPU cache miss yok
- **Free:** Tek operasyon — chunk listesini dolas, hepsini free et

### Free'nin Ozel Durumu

AST arena'sindan farkli olarak, type arena `free` ederken **icerideki heap array'leri de temizler**. Cunku bazi TypeInfo'lar dinamik diziler tutar:

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

Her TypeKind'in hangi alt-dizileri tutabilecegi burada kodlanmis. Yeni bir heap field eklendiginde bu listeye de eklenmesi gerekir — yoksa memory leak.

---

## Builtin Singleton'lar (type.c)

### Problem

Sema her `Int` gordugunde yeni bir TypeInfo allocate edip `kind = TY_INT` mi koyacak? Sonra iki tipin esit olup olmadigini kontrol etmek icin her seferinde struct karsilastirmasi mi yapacak?

### Cozum

Her builtin tip icin **tek bir** TypeInfo olusturulur ve global pointer'da tutulur:

```c
TypeInfo *TY_BUILTIN_VOID   = NULL;
TypeInfo *TY_BUILTIN_INT    = NULL;
TypeInfo *TY_BUILTIN_STRING = NULL;
TypeInfo *TY_BUILTIN_BOOL   = NULL;
TypeInfo *TY_BUILTIN_DOUBLE = NULL;
// ... toplam 15 singleton
```

`type_builtins_init()` bunlari arena'dan allocate eder:

```c
void type_builtins_init(TypeArena *a) {
  TypeInfo *t;
  #include "type_builtins.inc"   // @generated — her biri icin arena alloc + kind set
  // unsigned integer'lar (henuz yaml'da degil)
  t = type_arena_alloc(a); t->kind = TY_UINT64; TY_BUILTIN_UINT64 = t;
  t = type_arena_alloc(a); t->kind = TY_UINT;   TY_BUILTIN_UINT   = t;
  // ...
}
```

Artik sema'da tip kontrolu pointer karsilastirmasi:

```c
// Hizli: pointer karsilastirmasi O(1)
if (node->type == TY_BUILTIN_INT) { ... }

// Yavas: string karsilastirmasi O(n)
if (strcmp(node->type->name, "Int") == 0) { ... }  // BUNU YAPMIYORUZ
```

**Singleton avantaji:** Butun "Int" tipleri **ayni pointer**'i gosterir. `==` operatoru yeter, `strcmp` gereksiz.

---

## Tip Esitligi (equal.c)

Iki tipin esit olup olmadigini kontrol etmek icin iki fonksiyon var:

### type_equal() — Sig Karsilastirma

```c
int type_equal(const TypeInfo *a, const TypeInfo *b);
```

Yapisal esitlik. Iki tip esittir eger:
- Ayni `kind`'a sahiplerse **ve**
- Tum alt-bilesenler recursive olarak esitse

Ozel durumlar:

| TypeKind | Nasil karsilastirir |
|----------|-------------------|
| Primitif (`TY_INT`, ...) | Kind eslesmesi yeterli |
| `TY_NAMED` | **Pointer esitligi** — interned string oldugu icin `a->named.name == b->named.name` |
| `TY_OPTIONAL`, `TY_ARRAY`, `TY_SET` | `inner` recursive karsilastirma |
| `TY_DICT` | `key` ve `value` recursive |
| `TY_FUNC` | param sayisi, async, throws, tum params + ret recursive |
| `TY_TUPLE` | elem sayisi, tum elems recursive |
| `TY_GENERIC_PARAM` | Isim eslesmesi (index **gormezden gelinir**) |
| `TY_GENERIC_INST` | base + tum args recursive |

`TY_NAMED` icin pointer esitligi neden calisir? Cunku sema, ayni isimli tiplerin `named.name` alanini **ayni interned string**'e point ettirir. Iki farkli "MyStruct" string'i degil, ayni adrese isaret eden iki pointer.

### type_equal_deep() — Derin Karsilastirma

```c
int type_equal_deep(const TypeInfo *a, const TypeInfo *b);
```

`type_equal()` ile ayni, tek farkla: `TY_GENERIC_PARAM` icin **index** alani da karsilastirilir.

Ne zaman lazim? Generic fonksiyonlarda ayni isimli ama farkli pozisyondaki parametreleri ayirt etmek icin:

```swift
func foo<T, U>(a: T, b: U) where T == U { }
// T.index = 0, U.index = 1
// type_equal:      T == U -> true  (sadece isim bakar)
// type_equal_deep: T == U -> false (index farkli)
```

### Implementasyon Detayi

Her iki fonksiyon da ayni `type_cmp()` fonksiyonuna delege eder:

```c
static int type_cmp(const TypeInfo *a, const TypeInfo *b, int deep) {
  if (!a || !b) return a == b;       // ikisi de NULL -> esit
  if (a->kind != b->kind) return 0;  // farkli kind -> esit degil
  switch (a->kind) {
    // ... recursive karsilastirma
    case TY_GENERIC_PARAM:
      if (strcmp(a->param.name, b->param.name) != 0) return 0;
      return deep ? (a->param.index == b->param.index) : 1;
    default:
      return 1;  // primitif: kind eslesmesi yeter
  }
}
```

---

## Tipi String'e Donusturme (str.c)

```c
const char *type_to_string(const TypeInfo *t, char *buf, size_t sz);
```

TypeInfo agacini insan-okunur string'e cevirir. Hata mesajlari, debug dump'lari ve public API tarafindan kullanilir.

Ornekler:

| Girdi (TypeInfo) | Cikti (string) |
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

### Nasil Calisir

Caller buffer saglar, fonksiyon icerisine yazar:

```c
char buf[128];
printf("tip: %s\n", type_to_string(node->type, buf, sizeof(buf)));
// tip: (Int, String) -> Bool
```

Recursive tipler (mesela `Optional<Array<T>>`) icin **stack-allocated alt buffer'lar** kullanilir — heap allocation yok:

```c
case TY_OPTIONAL: {
  char inner[128];                                 // stack'te alt buffer
  type_to_string(t->inner, inner, sizeof(inner));  // recursive cagri
  snprintf(buf, sz, "%s?", inner);                 // birlestur
  break;
}
```

Primitif tip isimleri codegen ile uretilir (`type_str.inc`), bilesik tipler `switch` icinde elle yazilmistir.

---

## Generic Tip Ikamesi (sub.c)

Generic kod su sekilde calisir:

```swift
func identity<T>(_ x: T) -> T { return x }
let result = identity(42)  // T -> Int
```

Sema `identity(42)` gordugunde `T = Int` eslemesini kurar ve fonksiyonun tip agacindaki her `T`'yi `Int` ile degistirir. Bu is `type_substitute()` ile yapilir.

### TypeSubstitution: Esleme Haritasi

```c
typedef struct {
  TypeSubEntry entries[TYPE_SUB_MAX];  // en fazla 16 esleme
  uint32_t     count;
} TypeSubstitution;
```

Kullanim:

```c
TypeSubstitution sub = {0};
type_sub_set(&sub, "T", TY_BUILTIN_INT);       // T -> Int
type_sub_set(&sub, "U", TY_BUILTIN_STRING);    // U -> String
```

`type_sub_set` ayni isim varsa gunceller, yoksa yeni entry ekler. `type_sub_lookup` isimle arama yapar.

### type_substitute(): Recursive Ikame

```c
TypeInfo *type_substitute(const TypeInfo *ty, const TypeSubstitution *sub,
                          TypeArena *arena);
```

**Kritik tasarim karari: Non-destructive.** Eger hicbir sey degismediyse **orijinal pointer'i doner**. Yeni TypeInfo sadece gercekten bir degisiklik oldugunda arena'dan allocate edilir.

Ornek: `(T, [U]) -> Bool` tipinde `T -> Int`, `U -> String` ikamesi:

```
Girdi:  TY_FUNC { params: [TY_GENERIC_PARAM("T"), TY_ARRAY { inner: TY_GENERIC_PARAM("U") }],
                   ret: TY_BOOL }

Cikti:  TY_FUNC { params: [TY_BUILTIN_INT, TY_ARRAY { inner: TY_BUILTIN_STRING }],
                   ret: TY_BOOL }  <- ayni pointer, degismedi
```

`TY_BOOL` degismedi, dolayisiyla `ret` alani orijinal pointer'i gosterir. Ama `params` dizisi degisti, dolayisiyla yeni bir `TY_FUNC` allocate edildi.

### Switch Case'ler

Her bilesik tip icin ayri bir dal var:

| TypeKind | Ne yapar |
|----------|----------|
| `TY_GENERIC_PARAM` | `sub`'da arar, bulduysa somut tipi doner |
| `TY_NAMED` | `sub`'da arar (type alias ikamesi icin) |
| `TY_OPTIONAL`, `TY_ARRAY`, `TY_SET` | `inner`'i ikame et, degisdiyse yeni TypeInfo olustur |
| `TY_DICT` | `key` ve `value`'yu ikame et |
| `TY_FUNC` | Tum `params[]` ve `ret`'i ikame et, `changed` flag'i ile takip et |
| `TY_GENERIC_INST` | Tum `args[]`'i ikame et |
| `TY_TUPLE` | Tum `elems[]`'i ikame et, **labels'i deep-copy et** |
| Diger | Orijinal pointer'i doner |

### Tuple Labels Deep-Copy

`TY_TUPLE` ikamesinde ozel bir durum var: `labels` dizisi deep-copy edilir. Neden?

Orijinal tuple ve ikame edilmis tuple **ayni arena'da** yasarlar. `type_arena_free()` her ikisinin de `labels` dizisini free etmeye calisir. Eger ayni pointer'i paylassalardi, double-free olurdu.

```c
// sub.c'den:
if (ty->tuple.labels && ty->tuple.elem_count > 0) {
  const char **new_labels = malloc(ty->tuple.elem_count * sizeof(const char *));
  if (new_labels)
    memcpy(new_labels, ty->tuple.labels, ty->tuple.elem_count * sizeof(const char *));
  result->tuple.labels = new_labels;
}
```

---

## Type Predicate'ler (type.h)

Sema'da sikca sorulan sorulari kolaylastiran inline fonksiyonlar:

```c
type_is_named(ty, "MyStruct")   // TY_NAMED ve isim eslesiyor mu?
type_is_any(ty)                 // Any mi?
type_is_anyobject(ty)           // AnyObject mi?
type_is_never(ty)               // Never mi?
type_primary_protocol_name(ty)  // Protocol composition'dan ilk protokol adi
```

Bunlar `src/internal/type.h`'de `static inline` olarak tanimli. Her cagri O(1). Sema'da `switch` yerine bunlari kullanmak kodu okunur kilar:

```c
// Okunmasi zor:
if (ty && ty->kind == TY_NAMED && ty->named.name && strcmp(ty->named.name, "Never") == 0)

// Okunmasi kolay:
if (type_is_never(ty))
```

---

## TypeConstraint Sistemi

Generic `where` clause'lari `TypeConstraint` ile temsil edilir:

```swift
func sort<T>(arr: [T]) where T: Comparable { }
//                           ^^^^^^^^^^^^^^
//                           TC_CONFORMANCE: T, protocol_name = "Comparable"
```

### Constraint Turleri

| TypeConstraintKind | Swift karsiligi | Ornek |
|-------------------|----------------|-------|
| `TC_CONFORMANCE` | `T: Protocol` | `where T: Equatable` |
| `TC_SAME_TYPE` | `T == ConcreteType` | `where T == Int` |
| `TC_SUPERCLASS` | `T: SomeClass` | `where T: UIView` |
| `TC_SUPPRESSED` | `~Copyable` | `where T: ~Copyable` |
| `TC_SAME_TYPE_ASSOC` | `T.Assoc == U.Assoc` | `where T.Element == U.Element` |

### Nerede Saklanir?

Constraint'ler `TY_GENERIC_PARAM` tipinin icerisinde tutulur:

```c
struct TypeConstraint {
  TypeConstraintKind kind;
  const char        *protocol_name;   // TC_CONFORMANCE icin
  TypeInfo          *rhs_type;        // TC_SAME_TYPE icin
  const char        *assoc_name;      // TC_SAME_TYPE_ASSOC (sol taraf)
  const char        *rhs_param_name;  // TC_SAME_TYPE_ASSOC (sag taraf)
  const char        *rhs_assoc_name;  // TC_SAME_TYPE_ASSOC (sag taraf)
};
```

Bir generic parametre birden fazla constraint tasiyabilir:

```swift
func foo<T: Equatable & Hashable>(x: T) where T: Codable { }
// T.constraints = [
//   TC_CONFORMANCE("Equatable"),
//   TC_CONFORMANCE("Hashable"),
//   TC_CONFORMANCE("Codable"),
// ]
```

`constraints` dizisi heap'te allocate edilir ve `type_arena_free()` tarafindan temizlenir.

---

## Kullanim Ornegi

Butun parcalar bir araya gelince:

```c
#include "internal/type.h"

// 1. Arena ve builtins'i baslat
TypeArena arena;
type_arena_init(&arena, 0);
type_builtins_init(&arena);

// 2. [Int] tipi olustur
TypeInfo *arr = type_arena_alloc(&arena);
arr->kind  = TY_ARRAY;
arr->inner = TY_BUILTIN_INT;

// 3. String'e donustur
char buf[64];
printf("%s\n", type_to_string(arr, buf, sizeof(buf)));
// [Int]

// 4. Generic ikame: T -> [Int]
TypeInfo *param_t = type_arena_alloc(&arena);
param_t->kind = TY_GENERIC_PARAM;
param_t->param.name = "T";
param_t->param.index = 0;

TypeSubstitution sub = {0};
type_sub_set(&sub, "T", arr);

TypeInfo *result = type_substitute(param_t, &sub, &arena);
printf("%s\n", type_to_string(result, buf, sizeof(buf)));
// [Int]

// 5. Esitlik kontrolu
printf("esit mi? %d\n", type_equal(result, arr));
// esit mi? 1

// 6. Temizle
type_arena_free(&arena);
```

---

## Hatalarimiz ve Cozumleri

| Hata | Ne oldu | Cozum |
|------|---------|-------|
| Builtin icin `strcmp` | Her tip kontrolunde string karsilastirmasi — yavas | Singleton pointer esitligi |
| Arena `free`'de leak | Yeni heap field eklendi, `free` guncellenmeyi unuttu | Her `kind` icin acik `free` listesi |
| Tuple double-free | Orijinal ve substituted tuple ayni `labels`'i paylasiyordu | `labels` deep-copy |
| `type_sub` destructive | Orijinal tipi bozuyordu, sema tekrar kullanamiyor | Non-destructive: orijinal pointer korunur |
| 16'dan fazla generic param | `TYPE_SUB_MAX` asildi, sessizce kesildi | 16 yeterli — Swift'te 16+ generic param pratikte yok |
| Named tip `strcmp` | `type_equal`'da string karsilastirmasi O(n) | Interned pointer esitligi O(1) |
| `type_to_string` heap alloc | Her recursive cagri `malloc` yapiyordu | Stack-allocated alt buffer'lar |
| Arena init OOM | `calloc` basarisiz olunca dereference | NULL check + zeroed arena — `alloc` guvenle NULL doner |

---

## Sonraki Bolum

-> [05 — Semantic Analiz Nedir?](05-semantic-analiz-nedir.md)

---

*Bu belge [MiniSwiftFrontend](https://github.com/ugurtoprakdeviren/MiniSwiftFrontend) projesinin bir parcasidir.*
