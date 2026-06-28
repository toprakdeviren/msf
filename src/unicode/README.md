# src/unicode — msf's Unicode layer

msf's own, in-tree Unicode support. It started as the NFC-normalization slice of
the `decoder` Unicode engine (MIT — see `LICENSE`, commit `c5ecda4`) but is now **first-class msf
source**, pruned to exactly what the frontend needs. We do not track upstream.

## What's here (and why)

msf's string interner (`sema_intern`) NFC-normalizes identifiers. That is the
**only** thing this module does for msf, so the public API is just:

- `decoder_init` (no-op; no global state)
- `decoder_is_normalized_utf8`, `decoder_normalize_utf8`  (NFC / NFD)

Everything else from the vendored engine — IDNA, confusables, case folding,
segmentation, emoji properties, scripts, URL/security, UTF-16, the NFKC/NFKD
compatibility forms, combining-class / general-category queries, the runtime
SIMD-dispatch machinery — was removed (~6 MB of tables plus the matching
sources/headers).

## Files (5 total)

Three handwritten, two generated:

| File | Role |
|------|------|
| `include/decoder.h` | Public API: `decoder_status_t`, `decoder_normalization_form_t`, the three entry points. |
| `src/decoder.c` | The entire handwritten implementation: thread-local workspace, SIMD helpers (portable SWAR + WebAssembly SIMD128), generated-table lookups, recursive decomposition + canonical ordering, recomposition, the NFC/NFD engine, and UTF-8 ⇄ UTF-32 conversion. |
| `src/internal.h` | Internal declarations shared inside `decoder.c`. |
| `src/normalization.c` | **Generated** UAX #15 tables (CCC, canonical decomposition, composition — perfect-hash). |
| `src/normalization.h` | **Generated** table types and `extern` declarations. |

> Note: **Swift identifier/operator character classes are NOT here.** Those are
> the Swift *language grammar* (frozen ranges, not UAX #31) and live with the
> lexer in `src/lexer/unicode_ranges.h`. This module is generic Unicode
> *algorithms* (NFC/NFD); the lexer owns Swift's *grammar*.

## Build

Compiled into `libMiniSwiftFrontend.a` by the top-level Makefile (`DECODER_DIR`,
native + wasm; wasm uses SIMD128 via `-msimd128`). `make test` covers it, and
Unicode identifier behavior (NFC composed/decomposed, Hangul round-trip, emoji)
is exercised end-to-end.
