/**
 * @file internal.h
 * @brief Internal declarations shared within msf's Unicode decoder (decoder.c).
 *
 * Bridges the public API (decoder.h) and the generated normalization tables
 * (normalization.h): the UTF conversion result type, the thread-local
 * workspace, the SIMD helpers, and the handwritten normalization-engine
 * primitives.  Implementation lives entirely in decoder.c.
 */
#ifndef DECODER_INTERNAL_H
#define DECODER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/decoder.h"        // public status / normalization-form enums + entry points
#include "normalization.h"  // generated tables + types (normalization_form_t, decomposition_entry_t,
                            //  quick_check_result_t, simd_range_t, get_ccc/get_composition/…)

#ifdef __cplusplus
extern "C" {
#endif

// Branch-prediction hints.
#ifndef LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif
#endif

// ── SIMD helpers ─────────────────────────────────────────────────────────────
size_t simd_count_codepoints_utf8(const uint8_t *data, size_t len);
bool simd_is_ascii_text(const uint8_t *text, size_t length);
bool simd_all_below_threshold(const uint32_t *text, size_t len, uint32_t threshold);

// ── UTF-8 → UTF-32 conversion ────────────────────────────────────────────────
typedef enum {
    UTF_OK = 0,
    UTF_INVALID_INPUT = -1,
    UTF_BUFFER_OVERFLOW = -2,
    UTF_INVALID_UTF8 = -3,
    UTF_INVALID_UTF16 = -4, /* kept for status mapping */
    UTF_INVALID_UTF32 = -5
} utf_result_t;

utf_result_t utf8_to_utf32_auto(const char *utf8, size_t utf8_len, uint32_t *stack_buf,
                                size_t stack_cap, uint32_t **utf32_out, size_t *utf32_len,
                                bool *is_allocated);

// ── Thread-local workspace (reused across normalization calls) ───────────────
typedef struct {
    uint32_t *decode_buf;
    size_t decode_cap;
    uint32_t *process_buf;
    size_t process_cap;
    uint32_t *scratch_buf;
    size_t scratch_cap;
} decoder_workspace_t;

decoder_workspace_t *decoder_thread_workspace(void);
bool decoder_workspace_ensure_decode(decoder_workspace_t *ws, size_t required);
bool decoder_workspace_ensure_process(decoder_workspace_t *ws, size_t required);
bool decoder_workspace_ensure_scratch(decoder_workspace_t *ws, size_t required);

// ── Normalization engine ─────────────────────────────────────────────────────

// Internal alias for the generated quick-check enum (QC_YES/QC_NO/QC_MAYBE).
typedef enum {
    QC_YES_ALT = QC_YES,
    QC_NO_ALT = QC_NO,
    QC_MAYBE_ALT = QC_MAYBE
} quick_check_t;

// Hangul algorithmic decomposition/composition constants.
#define HANGUL_S_BASE 0xAC00
#define HANGUL_L_BASE 0x1100
#define HANGUL_V_BASE 0x1161
#define HANGUL_T_BASE 0x11A7
#define HANGUL_L_COUNT 19
#define HANGUL_V_COUNT 21
#define HANGUL_T_COUNT 28
#define HANGUL_N_COUNT (HANGUL_V_COUNT * HANGUL_T_COUNT)
#define HANGUL_S_COUNT (HANGUL_L_COUNT * HANGUL_N_COUNT)

// Below this code point, text is normalization-stable for NFC/NFD.
#define NQC_SAFE_THRESHOLD 0x80

// A starter code point safe to flush a normalization chunk at.
static inline bool is_safe_normalization_chunk_boundary(uint32_t cp) {
    if (cp <= 0x007F)
        return true;
    if (cp >= 0x4E00 && cp <= 0x9FFF)
        return true;
    if (cp == 0x3000 || cp == 0x3001 || cp == 0x3002)
        return true;
    return false;
}

int normalize(normalization_form_t form, const uint32_t *input, size_t len, uint32_t *output,
              size_t cap, size_t *out_len);
int normalize_with_workspace(normalization_form_t form, const uint32_t *input, size_t len,
                             uint32_t *output, size_t cap, size_t *out_len,
                             decoder_workspace_t *workspace);
bool is_normalized(normalization_form_t form, const uint32_t *text, size_t len);
bool recursive_decompose(uint32_t cp, normalization_form_t form, uint32_t *buffer, size_t cap,
                         size_t *len);
void canonical_ordering(uint32_t *buffer, size_t len);
size_t recompose(uint32_t *buffer, size_t len);
uint32_t find_composition(uint32_t first, uint32_t second);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_INTERNAL_H */
