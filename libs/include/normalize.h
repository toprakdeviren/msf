/**
 * @file normalize.h
 * @brief Unicode Normalization (UAX #15) and Decomposition/Composition primitives.
 *
 * Implements the four Unicode Normalization Forms: NFC, NFD, NFKC, NFKD.
 * Provides both UTF-32 and UTF-8 convenience interfaces, along with low-level
 * primitives for decomposition, composition, and combining class queries.
 *
 * The UTF-8 entry points are convenience wrappers over the UTF-32 engine: they
 * decode, normalize, and then re-encode.
 */
#ifndef DECODER_NORMALIZE_H
#define DECODER_NORMALIZE_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Normalization (UTF-32)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Normalizes a sequence of Unicode code points.
 *
 * @param src          Source code points.
 * @param src_len      Number of source code points.
 * @param form         Normalization form (NFC, NFD, NFKC, NFKD).
 * @param dst          Destination buffer for normalized code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len      Output: number of code points written to dst.
 *                     When the function returns `DECODER_ERROR_BUFFER_TOO_SMALL`,
 *                     `*dst_len` receives the required number of code points.
 * @return `DECODER_SUCCESS` on success, or a negative `decoder_status_t` on error.
 */
int decoder_normalize(const uint32_t *src, size_t src_len, decoder_normalization_form_t form,
                      uint32_t *dst, size_t dst_capacity, size_t *dst_len);

/**
 * @brief Checks if a sequence of Unicode code points is normalized.
 *
 * Performs a full check (not just quick-check) by normalizing and comparing.
 *
 * @param str   Sequence of code points to check.
 * @param len   Length of the sequence.
 * @param form  Normalization form.
 * @return true if the sequence is already in the given normalization form.
 */
bool decoder_is_normalized(const uint32_t *str, size_t len, decoder_normalization_form_t form);

/**
 * @brief Performs a quick check for normalization status (UAX #15, §9).
 *
 * The quick check uses precomputed property tables and may return MAYBE
 * when a full check would be needed for a definitive answer.
 *
 * @param str   Sequence of code points to check.
 * @param len   Length of the sequence.
 * @param form  Normalization form.
 * @return DECODER_YES if definitely normalized, DECODER_NO if definitely not,
 *         or DECODER_MAYBE if a full check is needed.
 */
decoder_quick_check_t decoder_quick_check(const uint32_t *str, size_t len,
                                          decoder_normalization_form_t form);

/**
 * @brief Compares two sequences for canonical/compatibility equivalence.
 *
 * Both sequences are normalized to the given form before comparison.
 *
 * @param s1    First sequence of code points.
 * @param len1  Length of the first sequence.
 * @param s2    Second sequence of code points.
 * @param len2  Length of the second sequence.
 * @param form  Normalization form to apply before comparison.
 * @return 0 if the sequences are normalization-equivalent, <0 if s1 < s2, >0 if s1 > s2.
 */
int decoder_normalize_compare(const uint32_t *s1, size_t len1, const uint32_t *s2, size_t len2,
                              decoder_normalization_form_t form);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Normalization (UTF-8 convenience wrappers)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Normalizes a UTF-8 encoded string.
 *
 * @param src          Source UTF-8 bytes.
 * @param src_len      Number of source bytes.
 * @param form         Normalization form (NFC, NFD, NFKC, NFKD).
 * @param dst          Destination buffer for normalized UTF-8 bytes.
 * @param dst_capacity Capacity of the destination buffer in bytes.
 * @param dst_len      Output: number of bytes written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_normalize_utf8(const uint8_t *src, size_t src_len, decoder_normalization_form_t form,
                           uint8_t *dst, size_t dst_capacity, size_t *dst_len);

/**
 * @brief Checks if a UTF-8 encoded string is normalized.
 *
 * @param str   UTF-8 byte sequence.
 * @param len   Number of bytes.
 * @param form  Normalization form.
 * @return true if the string is already in the given normalization form.
 */
bool decoder_is_normalized_utf8(const uint8_t *str, size_t len, decoder_normalization_form_t form);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Low-Level Primitives
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Returns the Canonical Combining Class (CCC) of a code point.
 *
 * The CCC determines the order in which combining marks are sorted during
 * canonical ordering (UAX #15, §3.11). Non-combining characters have CCC 0.
 *
 * @param cp  Unicode code point.
 * @return The combining class (0..254).
 */
uint8_t decoder_get_combining_class(uint32_t cp);

/**
 * @brief Checks if a code point is a combining character (CCC > 0 or Category M).
 *
 * @param cp  Unicode code point.
 * @return true if the code point has a non-zero canonical combining class or
 *         belongs to one of the Unicode mark categories (`Mn`, `Mc`, `Me`).
 */
bool decoder_is_combining(uint32_t cp);

/**
 * @brief Checks if two code points can be composed into a single code point.
 *
 * Tests whether the canonical composition of `(a, b)` exists and is not
 * excluded from recomposition.
 *
 * @param a  First code point (typically a starter).
 * @param b  Second code point (typically a combining mark or a starter).
 * @return true if compose(a, b) yields a valid composed form.
 */
bool decoder_can_compose(uint32_t a, uint32_t b);

/**
 * @brief Composes two code points into a single canonically-equivalent code point.
 *
 * @param a  First code point.
 * @param b  Second code point.
 * @return The composed code point, or 0 if no composition exists.
 *
 * @note Example: decoder_compose(0x0041, 0x0301) → 0x00C1 (Á)
 */
uint32_t decoder_compose(uint32_t a, uint32_t b);

/**
 * @brief Performs Canonical Decomposition of a code point.
 *
 * Returns the recursive canonical decomposition if one exists.
 *
 * @param cp        Code point to decompose.
 * @param out       Output buffer for the decomposed code points.
 * @param capacity  Maximum number of code points to write.
 * @return Number of code points written to `out`, or 0 if the code point has
 *         no canonical decomposition or the buffer is too small.
 *
 * @note Example: decoder_decompose(0x00C1, out, 4) → writes [0x0041, 0x0301], returns 2.
 */
size_t decoder_decompose(uint32_t cp, uint32_t *out, size_t capacity);

/**
 * @brief Performs Compatibility Decomposition of a code point.
 *
 * Similar to decoder_decompose() but also applies NFKD compatibility mappings
 * (e.g. ligatures, circled forms, width variants).
 *
 * @param cp        Code point to decompose.
 * @param out       Output buffer for the decomposed code points.
 * @param capacity  Maximum number of code points to write.
 * @return Number of code points written to `out`, or 0 if no decomposition is
 *         available or the buffer is too small.
 *
 * @note Example: decoder_decompose_compat(0xFB01, out, 4) → writes [0x0066, 0x0069], returns 2.
 *       (ﬁ ligature → "fi")
 */
size_t decoder_decompose_compat(uint32_t cp, uint32_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_NORMALIZE_H */
