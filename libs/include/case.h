#ifndef DECODER_CASE_H
#define DECODER_CASE_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file case.h
 * @brief Unicode case conversion and case-folding APIs.
 *
 * All string APIs apply full Unicode mappings, including multi-codepoint
 * expansions such as `U+00DF LATIN SMALL LETTER SHARP S -> "SS"` in uppercase.
 *
 * Locale-sensitive tailoring is intentionally narrow:
 * - Turkish/Azerbaijani dotted and dotless I rules are supported in
 *   upper/lower/case-fold locale APIs.
 * - Greek final sigma is handled in lowercase and titlecase paths.
 * - Other locale tags currently fall back to the default Unicode mappings.
 */

/**
 * @brief Converts a Unicode code point to uppercase.
 *
 * @param cp The Unicode code point to convert.
 * @return The uppercase equivalent of the code point.
 */
uint32_t decoder_to_upper(uint32_t cp);

/**
 * @brief Converts a Unicode code point to lowercase.
 *
 * @param cp The Unicode code point to convert.
 * @return The lowercase equivalent of the code point.
 */
uint32_t decoder_to_lower(uint32_t cp);

/**
 * @brief Converts a Unicode code point to title case.
 *
 * @param cp The Unicode code point to convert.
 * @return The title case equivalent of the code point.
 */
uint32_t decoder_to_title(uint32_t cp);

/**
 * @brief Converts a Unicode code point to case-folded form.
 *
 * @param cp The Unicode code point to convert.
 * @return The case-folded equivalent of the code point.
 */
uint32_t decoder_case_fold(uint32_t cp);

/**
 * @brief Converts a Unicode code point to uppercase, handling full case mappings.
 *
 * @param cp The Unicode code point to convert.
 * @param out Output buffer for the uppercase code point(s).
 * @param capacity Maximum number of code points to write to out.
 * @return The number of code points written to out, or 0 if the mapping does
 *         not fit in `out`.
 */
size_t decoder_to_upper_full(uint32_t cp, uint32_t *out, size_t capacity);

/**
 * @brief Converts a Unicode code point to lowercase, handling full case mappings.
 *
 * @param cp The Unicode code point to convert.
 * @param out Output buffer for the lowercase code point(s).
 * @param capacity Maximum number of code points to write to out.
 * @return The number of code points written to out, or 0 if the mapping does
 *         not fit in `out`.
 */
size_t decoder_to_lower_full(uint32_t cp, uint32_t *out, size_t capacity);

/**
 * @brief Converts a Unicode code point to title case, handling full case mappings.
 *
 * @param cp The Unicode code point to convert.
 * @param out Output buffer for the title case code point(s).
 * @param capacity Maximum number of code points to write to out.
 * @return The number of code points written to out, or 0 if the mapping does
 *         not fit in `out`.
 */
size_t decoder_to_title_full(uint32_t cp, uint32_t *out, size_t capacity);

/**
 * @brief Converts a Unicode code point to case-folded form, handling full case mappings.
 *
 * @param cp The Unicode code point to convert.
 * @param out Output buffer for the case-folded code point(s).
 * @param capacity Maximum number of code points to write to out.
 * @return The number of code points written to out, or 0 if the mapping does
 *         not fit in `out`.
 */
size_t decoder_case_fold_full(uint32_t cp, uint32_t *out, size_t capacity);

/**
 * @brief Converts a sequence of Unicode code points to uppercase.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param dst Destination buffer for uppercase code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_to_upper(const uint32_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                            size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to lowercase.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param dst Destination buffer for lowercase code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_to_lower(const uint32_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                            size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to title case.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param dst Destination buffer for title case code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_to_title(const uint32_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                            size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to case-folded form.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param dst Destination buffer for case-folded code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_case_fold(const uint32_t *src, size_t src_len, uint32_t *dst,
                             size_t dst_capacity, size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to uppercase using locale-specific rules.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param locale Locale for case conversion.
 * @param dst Destination buffer for uppercase code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_to_upper_locale(const uint32_t *src, size_t src_len,
                                   const decoder_locale_t *locale, uint32_t *dst,
                                   size_t dst_capacity, size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to lowercase using locale-specific rules.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param locale Locale for case conversion.
 * @param dst Destination buffer for lowercase code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_to_lower_locale(const uint32_t *src, size_t src_len,
                                   const decoder_locale_t *locale, uint32_t *dst,
                                   size_t dst_capacity, size_t *dst_len);

/**
 * @brief Converts a sequence of Unicode code points to case-folded form using locale-specific
 * rules.
 *
 * @param src Source code points.
 * @param src_len Number of source code points.
 * @param locale Locale for case conversion.
 * @param dst Destination buffer for case-folded code points.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of code points written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_string_case_fold_locale(const uint32_t *src, size_t src_len,
                                    const decoder_locale_t *locale, uint32_t *dst,
                                    size_t dst_capacity, size_t *dst_len);

/**
 * @brief Converts a UTF-8 encoded string to uppercase.
 *
 * @param src Source UTF-8 bytes.
 * @param src_len Number of source bytes.
 * @param dst Destination buffer for uppercase UTF-8 bytes.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of bytes written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_utf8_to_upper(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_capacity,
                          size_t *dst_len);

/**
 * @brief Converts a UTF-8 encoded string to lowercase.
 *
 * @param src Source UTF-8 bytes.
 * @param src_len Number of source bytes.
 * @param dst Destination buffer for lowercase UTF-8 bytes.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of bytes written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_utf8_to_lower(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_capacity,
                          size_t *dst_len);

/**
 * @brief Converts a UTF-8 encoded string to case-folded form.
 *
 * @param src Source UTF-8 bytes.
 * @param src_len Number of source bytes.
 * @param dst Destination buffer for case-folded UTF-8 bytes.
 * @param dst_capacity Capacity of the destination buffer.
 * @param dst_len Output parameter for the number of bytes written to dst.
 * @return `DECODER_SUCCESS` on success, error code on failure.
 */
int decoder_utf8_case_fold(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_capacity,
                           size_t *dst_len);

/**
 * @brief Checks if a Unicode code point is case-ignorable.
 *
 * @param cp The Unicode code point to check.
 * @return true if the code point is case-ignorable, false otherwise.
 */
bool decoder_is_case_ignorable(uint32_t cp);

/**
 * @brief Checks if a Unicode code point is cased.
 *
 * @param cp The Unicode code point to check.
 * @return true if the code point is cased, false otherwise.
 */
bool decoder_is_cased(uint32_t cp);

/**
 * @brief Compares two sequences of Unicode code points for case-insensitive equality.
 *
 * @param s1 First sequence of code points.
 * @param len1 Length of the first sequence.
 * @param s2 Second sequence of code points.
 * @param len2 Length of the second sequence.
 * @return 0 if the sequences are case-insensitively equal, <0 if s1 < s2, >0 if s1 > s2.
 */
int decoder_case_compare(const uint32_t *s1, size_t len1, const uint32_t *s2, size_t len2);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Case Property Predicates
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Checks if a code point would change when uppercased.
 *
 * @param cp  Unicode code point.
 * @return true if to_upper(cp) ≠ cp.
 */
bool decoder_changes_when_uppercased(uint32_t cp);

/**
 * @brief Checks if a code point would change when lowercased.
 *
 * @param cp  Unicode code point.
 * @return true if to_lower(cp) ≠ cp.
 */
bool decoder_changes_when_lowercased(uint32_t cp);

/**
 * @brief Checks if a code point would change when titlecased.
 *
 * @param cp  Unicode code point.
 * @return true if to_title(cp) ≠ cp.
 */
bool decoder_changes_when_titlecased(uint32_t cp);

/**
 * @brief Checks if a code point would change when case-folded.
 *
 * @param cp  Unicode code point.
 * @return true if case_fold(cp) ≠ cp.
 */
bool decoder_changes_when_casefolded(uint32_t cp);

/**
 * @brief Checks two sequences for case-insensitive equality (boolean).
 *
 * @param s1    First code point sequence.
 * @param len1  Length of the first sequence.
 * @param s2    Second code point sequence.
 * @param len2  Length of the second sequence.
 * @return true if s1 and s2 are case-insensitively equal.
 */
bool decoder_case_equal(const uint32_t *s1, size_t len1, const uint32_t *s2, size_t len2);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_CASE_H */
