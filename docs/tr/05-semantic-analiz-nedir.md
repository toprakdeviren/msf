# Semantic Analiz Nedir ve Nasil Calisir?

> Bu belge MiniSwiftFrontend'in semantic analiz (sema) katmanini sifirdan acikliyor.
> Onceki bolumler: [01 -- Lexer](01-lexer-nedir.md), [02 -- AST](02-ast-nedir.md), [03 -- Parser](03-parser-nasil-calisir.md), [04 -- Type Sistemi](04-type-sistemi-nedir.md)

## Sema Ne Yapar?

Parser bize bir agac (AST) verdi. Ama bu agac henuz **anlam** tasimaz:

```
let x = 1 + 2
```

Parser soyle goruyor:

```
let_decl(x)
  binary_expr(+)
    integer_literal(1)
    integer_literal(2)
```

Ama su sorularin cevabi yok:
- `x`'in tipi ne? (`Int`)
- `1 + 2` ifadesi gecerli mi? (`Int + Int -> Int` -- evet)
- `x` daha once tanimlanmis mi? (hayir, ilk kez -- kaydet)
- Bu isimleri baskasi kullanabilir mi? (access control)

**Semantic analiz** (kisaca **sema**) bu sorulari cevaplar. AST'nin her node'una tip atar, isimleri kaydeder, kurallari denetler. Sema'dan gecen bir AST artik **anlamli**dir -- backend onu alip kod uretebilir (ya da biz hata mesajlari gosterebiliriz).

Tek satirda:

```c
#include <msf.h>

MSFResult *r = msf_analyze("let x: Int = 1 + 2", "main.swift");
// r->root->type artik TY_INT
// r->root->first_child->type de TY_INT (binary_expr)
```

---

## Pipeline'daki Yeri

```
"let x = 1 + 2"
       |
       v
 +----------+     +----------+     +----------+
 |  Lexer   |---->|  Parser  |---->|   Sema   |
 | (token)  |     |  (AST)   |     |  <- biz  |
 +----------+     +----------+     +----------+
                                        |
                                        v
                                   Tipli AST
                                   + hatalar
```

Sema, pipeline'in **son** asamasidir. Lexer karakter karakter okur, parser token'lardan agac kurar, sema ise bu agaci yuruyerek **anlam** katar. Sema'dan sonra elimizde her node'a tip atanmis, her isim cozulmus, her kural denetlenmis bir AST var.

---

## Buyuk Resim: Uc Pass

Sema tek bir yuruyus degil -- **uc ayri pass** yapar. Her pass AST'yi bastan sona yuruyor ama farkli bir is yapiyor:

```
AST (parser ciktisi)
       |
       v
 +------------------+
 |  Pass 1: DECLARE |   Tum isimleri kaydet (forward declaration)
 +------------------+
       |
       v
 +------------------+
 |  Pass 2: RESOLVE |   Tipleri coz, ifadeleri denetle
 +------------------+
       |
       v
 +------------------+
 |  Pass 3: CONFORM |   Protokol uyumluluklarini kontrol et
 +------------------+
       |
       v
   Tipli AST + hatalar
```

**Neden uc pass?** Cunku Swift'te forward reference var. Su kod gecerli:

```swift
func foo() -> Bar { ... }  // Bar henuz tanimlanmadi
struct Bar { ... }          // ama burada tanimlaniyor
```

Eger tek pass yaparsak, `foo`'yu isle digimizde `Bar` diye bir sey yoktur. Ama ilk pass'te **tum isimleri kaydedersek**, ikinci pass'te `Bar`'i bulabiliriz.

Ucuncu pass neden ayri? Cunku protokol uyumlulugu kontrol ederken **tum tiplerin cozulmus** olmasi gerekir. `struct Foo: Equatable` yazdiysan, Foo'nun `==` operatorunu sagladigini ancak tum tipler cozuldukten sonra dogrulayabilirsin.

---

## Kod Yapisi

Sema modulu 17 dosyadan olusur. Toplam ~7.660 satir:

```
src/semantic/
  private.h             (445 satir) -- SemaContext, Symbol, Scope, tum prototipler
  core.c                (708 satir) -- intern pool, symbol table, scope, hata raporlama
  declare.c             (519 satir) -- Pass 1: forward declaration
  type_resolution.c     (614 satir) -- AST_TYPE_* -> TypeInfo cozumleme
  conformance.c         (457 satir) -- Builtin member tablosu (~97 girdi)
  conformance_table.c   (322 satir) -- Protokol conformance tablosu
  generics.c            (234 satir) -- Generic constraint denetimi
  builder.c             (308 satir) -- @resultBuilder AST donusumu
  resolve/
    node.c             (1184 satir) -- Ust seviye dispatch: resolve_node()
    decl.c             (1081 satir) -- var/let, func, class, struct, enum, protocol, extension
    expr.c              (531 satir) -- Literal, ident, binary, unary, assign, ternary, ...
    expr_call.c         (450 satir) -- Cagri ifadesi: overload resolution, init delegation
    expr_member.c       (241 satir) -- Member erisimi: base.member, optional chaining
    expr_binary.c       (168 satir) -- Binary: atama, karsilastirma, aritmetik, ??
    expr_pre.c           (93 satir) -- Yardimci: resolve_children, contextual type
    access.c            (137 satir) -- Erisim kontrolu: private -> open siralama
    protocol_helpers.c  (171 satir) -- Protokol gereksinim yardimcilari
```

Veri akisi:

```
core.c  (intern pool, symbol table, hata raporlama)
  |
  +--- declare.c          (Pass 1 -- isimleri kaydet)
  |
  +--- type_resolution.c  (AST_TYPE_* -> TypeInfo)
  |
  +--- resolve/
  |      node.c   (dispatch)
  |        |
  |        +--- decl.c          (bildirim cozumleme)
  |        +--- expr.c          (ifade cozumleme)
  |        +--- expr_call.c     (cagri cozumleme)
  |        +--- expr_member.c   (member cozumleme)
  |        +--- expr_binary.c   (binary cozumleme)
  |        +--- expr_pre.c      (yardimcilar)
  |        +--- access.c        (erisim kontrolu)
  |        +--- protocol_helpers.c
  |
  +--- conformance.c       (builtin member tablosu)
  +--- conformance_table.c (protokol uyumluluk tablosu)
  +--- generics.c          (generic constraint denetimi)
  +--- builder.c           (@resultBuilder donusumu)
```

---

## SemaContext: Her Seyin Merkezi

Sema'nin tum durumu tek bir struct'ta yasiar:

```c
struct SemaContext {
  /* Girdi (sahiplik baskasinda) */
  const Source *src;
  const Token  *tokens;
  ASTArena     *ast_arena;
  TypeArena    *type_arena;

  /* Sembol tablosu */
  Scope      *current_scope;
  InternPool *intern;

  /* Protokol uyumlulugu */
  ConformanceTable *conformance_table;
  AssocTypeTable   *assoc_type_table;

  /* Attribute kayitlari */
  WrapperEntry wrapper_types[WRAPPER_TABLE_MAX];
  BuilderEntry builder_types[BUILDER_TABLE_MAX];

  /* Hatalar */
  uint32_t error_count;
  char     errors[32][256];

  /* Baglam durumu (agac yuruyusu sirasinda degisir) */
  TypeInfo *expected_closure_type;
  uint8_t   requires_explicit_self;
  void     *current_func_decl;

  /* Iki fazli class initialization */
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

**Onemli alanlar:**

| Alan | Ne ise yarar |
|------|-------------|
| `current_scope` | Scope zincirinin basi -- isim aramalari buradan baslar |
| `intern` | String interning havuzu -- tum isim karsilastirmalari pointer esitligi |
| `conformance_table` | "Int: Equatable" gibi uyumluluk kayitlari |
| `error_count` / `errors[]` | Hata mesajlari -- sema durmaz, kayit tutar |
| `in_class_init_phase1` | Class init'inde miyiz? Hangi property'ler atandi? |
| `expected_closure_type` | Closure icerisinde beklenen tip (contextual typing) |

---

## String Interning: FNV-1a + NFC

Sema'da en sik yapilan islem isim karsilastirmasidir: "`x` diye bir sembol var mi?", "`Int` builtin mi?", "`foo` bir fonksiyon mu?". Her seferinde `strcmp` yapmak pahali olurdu.

Cozum: **string interning**. Her string havuza bir kez eklenir, sonra sadece **pointer karsilastirmasi** yapilir.

```c
const char *a = sema_intern(ctx, "hello", 5);
const char *b = sema_intern(ctx, "hello", 5);
// a == b (ayni pointer!)  -- strcmp gerekmez
```

Nasil calisiyor:

1. **FNV-1a hash** hesapla (basit, hizli, dagilimi iyi):

```c
static uint32_t intern_hash(const char *s, size_t len) {
  uint32_t h = 2166136261u;         // FNV offset basis
  for (size_t i = 0; i < len; i++)
    h = (h ^ (uint8_t)s[i]) * 16777619u;  // FNV prime
  return h;
}
```

2. **Open-addressing** hash tablosunda ara (linear probing, load factor %75):
   - Bulursan -> mevcut pointer'i don
   - Bulamazsan -> buffer'a kopyala, tabloya ekle, yeni pointer'i don

3. **NFC normalization**: Swift derleyicileri identifier'lari NFC'ye normalize eder. `'e' + U+0301` (combining accent) ile `U+00E9` (precomposed) ayni sey olmali. Ama ASCII identifier'lar (kodun %95'i) icin quick-check fast path var -- sifir ek maliyet.

```c
const char *sema_intern(SemaContext *ctx, const char *str, size_t len) {
  // NFC quick check -- ASCII ise sifir overhead
  if (!decoder_is_normalized_utf8(str, len, DECODER_NFC)) {
    // normalize et...
  }
  // hash + lookup + insert
}
```

**Trade-off:** Interning bellegi asla geri verilmez (pool yasam suresi = sema yasam suresi). Ama compiler'lar icin bu kabul edilebilir -- analiz bitince her sey zaten free ediliyor.

---

## Symbol Table ve Scope Chain

### Symbol Nedir?

Her isim bir `Symbol` olarak kaydedilir:

```c
struct Symbol {
  const char *name;       // interned isim (pointer equality)
  SymbolKind  kind;       // SYM_VAR, SYM_FUNC, SYM_CLASS, ...
  TypeInfo   *type;       // cozulmus tip
  ASTNode    *decl;       // AST'deki bildirim node'u
  Symbol     *next;       // hash bucket zinciri
  uint8_t     is_initialized;
  uint8_t     is_deferred;
  uint8_t     is_resolving;  // sonsuz dongu korumasi
};
```

`is_resolving` flagi neden var? Cunku tip cozumleme sirasinda dongusel bagimlilik olabilir:

```swift
typealias A = B
typealias B = A    // sonsuz dongu!
```

`is_resolving = 1` ise "bu sembol su an cozuluyor, tekrar girme" demek.

### Scope Nedir?

Her lexical kapsam bir `Scope`:

```c
struct Scope {
  Symbol  *buckets[SCOPE_HASH_SIZE];   // hash bucket'lari
  Scope   *parent;                      // dis kapsam
  uint32_t depth;                       // ic ice derinlik
};
```

**Scope chain**: Ic ice kapsam zinciri. Bir isim aradigimizda once mevcut scope'a, bulamazsak parent'a, onun da parent'ina bakariz:

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

Arama:

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

**Dikkat:** `sym->name == name` -- `strcmp` yok! Cunku iki taraf da interned. Bu, her lookup'i `O(scope derinligi * bucket uzunlugu)` yapar. Pratikte bucket'lar kisa (iyi hash dagilimi), scope derinligi de 5-10 civarinda.

### Scope Push/Pop

Yeni bir `{...}` blokuna girdigimizde `sema_push_scope()`, ciktigimizda `sema_pop_scope()` cagrilir:

```c
Scope *sema_push_scope(SemaContext *ctx) {
  Scope *s = calloc(1, sizeof(Scope));
  s->parent = ctx->current_scope;
  s->depth = ctx->current_scope ? ctx->current_scope->depth + 1 : 0;
  ctx->current_scope = s;
  return s;
}
```

Pop yapildiginda ic scope'daki tum semboller erisime kapanir -- ama bellekte kalir (arena-style). Neden free etmiyoruz? Cunku AST node'lari hala o symbol'lere referans tutuyor olabilir.

---

## Pass 1: Declaration (declare.c)

Ilk pass AST'yi yuruyor ve **tum isimleri sembol tablosuna kaydediyor**. Tipleri henuz cozmuyor -- sadece "bu isimde bir sey var" diyor.

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

Kaydetme disinda su isleri de yapar:

| Is | Aciklama |
|----|----------|
| Protokol uyumluluk kaydedme | `struct Foo: Equatable` -> conformance tablosuna ekle |
| @propertyWrapper kaydi | `@propertyWrapper struct Lazy` -> wrapper tablosuna ekle |
| @resultBuilder kaydi | `@resultBuilder struct VStack` -> builder tablosuna ekle |
| Class override/final denetimi | `override func` ile `final func` cakismasi |
| Enum indirect kontrol | `indirect enum` dongusel referans denetimi |
| Erisim seviyesi dogrulama | `private class` icinde `public var` -- uyari |

**Neden sadece isim kaydediyoruz, tipi neden cozemiyoruz?** Cunku forward reference yuzunden. Tip cozumleme sirasinda diger isimlere referans verebilirsin. Eger o isimler henuz kaydedilmediyse cozumleme basarisiz olur. Ilk pass tum isimleri kaydederek ikinci pass'in isini kolaylastirir.

---

## Pass 2: Type Resolution

Ikinci pass AST'yi tekrar yuruyor ve bu sefer her node'a **tip atama** yapiyor.

### Tip Cozumleme Dispatch Tablosu (type_resolution.c)

AST'de tip belirten node'lar `AST_TYPE_*` ile baslar: `AST_TYPE_IDENT`, `AST_TYPE_OPTIONAL`, `AST_TYPE_ARRAY`, vb. Her biri icin ayri bir resolver fonksiyonu var:

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

Bu sayede `resolve_type_annotation()` O(1) dispatch yapar:

```c
TypeInfo *resolve_type_annotation(SemaContext *ctx, const ASTNode *tnode) {
  TypeResolver fn = type_resolvers[tnode->kind];
  if (fn) return fn(ctx, tnode);
  return NULL;
}
```

**Neden switch yerine tablo?** Cunku `switch` her eklediginde yeniden derleme gerektirir. Tablo ise sadece `init_type_resolvers()`'a bir satir eklemeyi gerektirir. Ayrica compiler `switch`'i tabloya ceviremeyebilir (node kind'lar sikiska degilse), ama biz zaten tabloyu kendimiz yapiyoruz.

**Generic sugar** -- `Array<Int>` ifadesi aslinda `[Int]` ile ayni seydir. `resolve_type_generic()` bunu yakalar:

```
Array<T>      -> TY_ARRAY(T)
Dictionary<K,V> -> TY_DICT(K, V)
Optional<T>   -> TY_OPTIONAL(T)
```

### Node Resolution (resolve/)

`resolve_node()` (resolve/node.c) gelen AST node'unun turune gore ya `resolve_node_decl()` ya da `resolve_node_expr()` cagrir. Bunlar da kendi iclerinde daha spesifik resolver'lara dagitir.

**Ifade tip cozumleme (expr.c + expr_*.c):**

Parser'daki Pratt yaklasimi burada da devam eder: tipler **asagidan yukari** (bottom-up) akar. Yaprak node'lar (literal, identifier) kendi tiplerini bilir, parent node'lar cocuklarinin tiplerinden kendi tiplerini cikarir:

```
binary_expr(+)           <- Int (cocuklardan cikarildi)
  integer_literal(1)     <- Int (literal -> TY_INT)
  integer_literal(2)     <- Int (literal -> TY_INT)
```

Ifade turlerine gore resolver dagitimi:

| Ifade turu | Dosya | Ne yapar |
|------------|-------|----------|
| Literal (int, string, bool) | expr.c | Sabit tiplerden biri: TY_INT, TY_STRING, ... |
| Identifier (`x`, `foo`) | expr.c | Symbol table'da ara, tipini don |
| Binary (`+`, `==`, `??`) | expr_binary.c | Sol + sag tiplere gore sonuc tipi |
| Call (`foo()`, `Bar()`) | expr_call.c | Overload resolution, generic constraint denetimi |
| Member (`x.count`, `.none`) | expr_member.c | Builtin member tablosu + user-defined arama |
| Unary (`!x`, `-x`) | expr.c | Operand tipine gore sonuc tipi |
| Ternary (`a ? b : c`) | expr.c | b ve c'nin ortak tipi |
| Cast (`x as Int`, `x is String`) | expr.c | Hedef tip cozumleme |
| Subscript (`arr[0]`) | expr.c | Koleksiyon eleman tipi |

### Overload Resolution (expr_call.c)

Swift'te ayni isimde birden fazla fonksiyon olabilir:

```swift
func f(_ x: Int) -> Int { ... }
func f(_ x: String) -> String { ... }

f(42)      // hangisi?
```

`sema_lookup_overloads()` tum `SYM_FUNC` sembollerini toplar, sonra arguman tipleriyle eslestirme yapilir. Eslesen tek aday varsa secilir, birden fazla varsa belirsizlik hatasi verilir.

### Init Delegation

Class init'lerinde `self.init(...)` ve `super.init(...)` cagrilari ozel olarak islenir. `expr_call.c` bu durumlari tanir ve iki fazli initialization kurallarini denetler.

---

## Pass 3: Conformance Checking

Ucuncu pass protokol uyumluluklarini denetler.

```swift
protocol Equatable {
  static func == (lhs: Self, rhs: Self) -> Bool
}

struct Point: Equatable {
  var x: Int
  var y: Int
  // == nerede? -> HATA
}
```

`pass3_check_conformances()` su adimlari uygular:

1. Conformance tablosundan tum kayitlari al
2. Her kayit icin: tip bildirimi + protokol bildirimi bul
3. Protokolun her gereksinimine bak (func, var, associatedtype)
4. Tipin bunu saglayip saglamadigini denetle
5. Saglamiyorsa -> hata: "Type 'Point' does not conform to protocol 'Equatable'"

**Builtin conformance'lar** (conformance_table.c): Sema baslarken stdlib tipleri icin conformance'lar onceden kaydedilir:

```
Int    : Equatable, Hashable, Comparable, Numeric, Codable, ...
String : Equatable, Hashable, Comparable, Codable, ...
Array  : Sequence, Collection, MutableCollection, ...
Bool   : Equatable, Hashable, Codable, ...
```

Bu sayede `Array<Int>` icin `Sequence` uyumlulugu soruldiginda tablo'ya bakmak yeterli -- AST'de karsilik gelen bir bildirim aramaya gerek yok.

---

## Builtin Member Lookup (conformance.c)

Swift stdlib tiplerinin yuzlerce member'i var: `Array.count`, `String.isEmpty`, `Int.description`, `Dictionary.keys`, ... Bunlarin hepsini AST'de tanimlamak pratik degil.

Cozum: **tablo-driven builtin member lookup**. ~97 girdilik bir tablo:

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
  // ... ~97 girdi
};
```

Arama:

1. Base tipi `BMKind`'a cevir: `Array` -> `BMK_ARRAY`, `String` -> `BMK_STRING`, ...
2. Tabloyu tara, `(base_kind, name)` esleseni bul
3. `BMResult`'i `TypeInfo*`'a cevir: `BMR_INT` -> `TY_BUILTIN_INT`, `BMR_OPT_INNER` -> optional'in ic tipi, ...

**Neden `strcmp` zincirleri yerine tablo?** Cunku:
- Yeni member eklemek tek satir
- Tablo `.rodata`'da yasiar, cache-friendly
- `BMKind` enum'u ile once base tipi filtrelenir -- tum ~97 girdiyi taraman gerekmez, sadece o tipe ait olanlara bakarsin

---

## Generic Constraint Checking (generics.c)

Generic tipler constraint'lerle sinirlandirilabilir:

```swift
func sort<T: Comparable>(_ arr: [T]) -> [T] { ... }

sort([3, 1, 2])    // T = Int, Int: Comparable mi? -> evet
sort([{}, {}])     // T = closure, closure: Comparable mi? -> HAYIR
```

`generics.c` su tur constraint'leri denetler:

| Constraint turu | Ornek | Kontrol |
|----------------|-------|---------|
| Conformance | `T: Equatable` | Conformance tablosunda ara |
| Same-type | `T == Int` | `type_equal()` ile dogrula |
| Superclass | `T: UIView` | Kalitim zincirini yuru |
| Suppressed | `~Copyable` | Ozel durum -- tersine mantik |
| Conditional | `Array<T>: Equatable where T: Equatable` | Where clause'u recursive denetle |

**Conditional conformance** ozel: `Array<Int>: Equatable` gecerli cunku `Int: Equatable`. Ama `Array<MyType>: Equatable` ancak `MyType: Equatable` ise gecerli. Bu kontrol icin where clause AST'si conformance tablosunda saklanir ve constraint denetimi sirasinda `check_conditional_conformance()` recursive olarak cagirilir.

---

## @resultBuilder Donusumu (builder.c)

SwiftUI'nin temel mekanizmasi olan result builder'lar, fonksiyon govdesini **sentetik AST** node'larina donusturur:

```swift
@resultBuilder struct VStackBuilder {
  static func buildBlock(_ components: View...) -> View { ... }
  static func buildOptional(_ component: View?) -> View { ... }
  // ...
}

@VStackBuilder
func body() -> View {
  Text("Hello")        // bu satirlar...
  if showDetail {
    Text("Detail")
  }
}
```

Sema bu govdeyi soyle donusturur:

```
// Orijinal:
Text("Hello")
if showDetail { Text("Detail") }

// Donusmus:
VStackBuilder.buildBlock(
  Text("Hello"),
  VStackBuilder.buildOptional(showDetail ? Text("Detail") : nil)
)
```

`builder.c` su fonksiyonlari kullanir:

| Fonksiyon | Ne yapar |
|-----------|----------|
| `node_get_builder()` | Node'un @resultBuilder attribute'u var mi? |
| `transform_builder_body()` | Govdeyi donustur: if -> buildOptional, for -> buildArray, ... |
| `build_block_call_from_stmts()` | Statement'lari `buildBlock(...)` cagrisina sar |
| `wrap_in_build_expression()` | Tek ifadeyi `buildExpression(...)` ile sar |
| `wrap_builder_method_call()` | Genel sentetik method cagri node'u olustur |

**Onemli:** Bu donusum **AST seviyesinde** yapilir. Yeni ASTNode'lar `synth_node()` ile olusturulur, mevcut node'lar `clone_node()` ile kopyalanir. Token indeksleri olmayabilir (sentetik node'lar) -- hata mesajlarinda bu dikkate alinir.

---

## Erisim Kontrolu (access.c)

Swift'te 6 erisim seviyesi var:

```
private < fileprivate < internal < package < public < open
```

`access.c` su isleri yapar:

1. **Siralama**: Her modifier'a bir rank atar (0-5). `access_rank(MOD_PRIVATE)` -> 0, `access_rank(MOD_OPEN)` -> 5.

2. **Efektif erisim**: Bir tipin efektif erisim seviyesi, kendi modifier'i ve iceriginin en dusuk seviyesinin minimumudur. `public struct Foo { private var x: SecretType }` -- `Foo`'nun efektif erisimi `private`.

3. **Protokol member erisimi**: Protokol member'lari her zaman protokolun erisim seviyesini alir.

4. **Extension member erisimi**: Extension member'lari, extension'in ya da genisletilen tipin erisim seviyesini miras alir.

5. **Private member gorunurlugu**: Ayni dosyadaki extension'lar `private` member'lara erisebilir (Swift semantics).

---

## Hata Raporlama: "Did You Mean?"

Sema bir isim bulamadiginda sadece "undeclared" demez -- **benzer isim onerir**:

```
error: cannot find 'prnt' in scope; did you mean 'print'?
```

Bu nasil calisiyor:

1. `sema_find_similar_type_name()` tum scope zincirini yurur
2. Her sembol icin `lev_distance()` (Levenshtein edit distance) hesaplar
3. Mesafe 3'ten kucukse (ve mevcut en iyi adaydan daha iyiyse) kaydeder
4. `sema_error_suggest()` hata mesajina "did you mean 'X'?" ekler

```c
int lev_distance(const char *a, const char *b) {
  // Klasik DP matrisi -- O(m*n)
  // Kucuk stringler icin (identifier'lar genelde <30 char) yeterince hizli
}
```

**Neden Levenshtein?** Cunku typo'larin cogu 1-2 edit distance'ta: harf eksik, harf fazla, iki harf yer degistirmis. Levenshtein tam olarak bunu olcuyor.

---

## Iki Fazli Class Initialization

Swift class init'lerinin ozel kurallari var:

```swift
class Base {
  var x: Int
  init() {
    x = 0           // Phase 1: kendi property'lerini ata
    super.init()     // super.init cagir
    doSomething()    // Phase 2: self'i kullanabilirsin
  }
}
```

SemaContext'te su alanlar bu kurallari denetler:

```c
uint8_t  in_class_init_phase1;        // Phase 1'de miyiz?
const char *init_own_props[16];       // Bu class'in stored property isimleri
uint8_t     init_own_assigned[16];    // Hangisi atandi?
```

Phase 1'de `self` kullanilamaz (super.init cagrilmadan once). Tum stored property'ler super.init'ten once atanmis olmali. Bu kurallar `resolve/decl.c`'de init cozumleme sirasinda denetlenir.

---

## Closure Capture Analizi

Closure'lar dis scope'tan degisken yakalayabilir:

```swift
var counter = 0
let inc = { counter += 1 }  // counter yakalandi
```

`identify_captures()` (resolve/node.c) su adimlari izler:

1. Closure govdesindeki tum identifier'lari topla
2. Closure'un kendi local degiskenlerini cikar
3. Geri kalanlar icin dis scope'ta ara
4. Bulunanlar **capture** olarak kaydedilir

```c
typedef struct {
  const char  *name;
  CaptureMode  mode;    // strong, weak, unowned, value
  TypeInfo    *type;
  int          is_outer;
} CaptureInfo;
```

Capture mode'u `[weak self]`, `[unowned self]`, `[x]` (value capture) gibi capture list'ten belirlenir.

---

## Hatalarimiz ve Cozumleri

| Hata | Ne oldu | Cozum |
|------|---------|-------|
| Tek pass | Forward reference cozemiyorduk | Uc pass mimarisi: declare -> resolve -> conform |
| strcmp her yerde | Profiling'de %15 zaman string karsilastirmada | String interning (FNV-1a) -- pointer equality |
| Switch-case dispatch | Type resolution icin 30+ case'lik switch | Dispatch table: `type_resolvers[kind]` -- O(1) |
| Builtin member'lar hardcoded | Her yeni member icin 50 satir if-else | ~97 girdilik tablo-driven lookup |
| Scope leak | Pop edilmeyen scope'lar bellek sizintisi | Push/pop her zaman esli -- `declare_in_scope()` helper |
| Sonsuz dongu | `typealias A = B; typealias B = A` | `is_resolving` flag'i ile recursion guard |
| Belirsiz hata mesajlari | "unknown type" -- ama hangi tip? | Levenshtein + "did you mean?" onerisi |
| Class init kurallari | Phase 1'de self kullanimi yakalanamiyordu | `in_class_init_phase1` + property tracking |
| Conformance tablosu tasmasi | 256'dan fazla kayitta sessiz kayip | `CONFORMANCE_TABLE_MAX` guard + uyari |
| Builder donusumunde token eksik | Sentetik node'larin tok_idx'i 0 | Hata mesajlarinda parent node'un pozisyonu kullanilir |
| NFC normalization | `cafe\u0301` ve `cafe` farkli interned | NFC quick-check + normalization |
| Conditional conformance | `Array<Int>: Equatable` kontrol edilemiyordu | Where clause AST'si conformance tablosunda saklanir |

---

## Sonraki Adimlar

Sema, MiniSwiftFrontend pipeline'inin son asamasidir. Sema'dan cikan tipli AST + hata listesi, kullanici tarafindan tuketilmeye hazirdir:

```c
MSFResult *r = msf_analyze("let x = 1 + 2", "main.swift");

// Tipli AST'yi yuru
msf_dump_text(r, stdout);

// Hatalari oku
for (uint32_t i = 0; i < msf_error_count(r); i++)
    printf("%u:%u: %s\n",
           msf_error_line(r, i),
           msf_error_col(r, i),
           msf_error_message(r, i));

msf_result_free(r);
```

---

*Bu belge [MiniSwiftFrontend](https://github.com/ugurtoprakdeviren/MiniSwiftFrontend) projesinin bir parcasidir.*
