/**
 * @file encoding.h
 * @brief UTF-8 / UTF-16 / UTF-32 validation and conversion.
 *
 * All conversion functions follow a uniform contract:
 *  - Return DECODER_SUCCESS (0) on success, or a negative decoder_status_t on failure.
 *  - Write the number of output units through the final pointer parameter.
 *  - Never write beyond dst_capacity.
 *  - Stop cleanly at the first invalid sequence; partial output is still usable.
 */
#ifndef DECODER_ENCODING_H
#define DECODER_ENCODING_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  UTF-8
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Validates a UTF-8 byte sequence.
 *
 * Uses SIMD-accelerated validation when available (SSE2/NEON/WASM SIMD128).
 *
 * @param str   Pointer to the UTF-8 byte sequence.
 * @param len   Number of bytes to validate.
 * @return true if the entire sequence is well-formed UTF-8, false otherwise.
 */
bool decoder_is_valid_utf8(const uint8_t *str, size_t len);

/**
 * @brief Returns the number of Unicode code points in a UTF-8 string.
 *
 * Counts code points by skipping continuation bytes (0x80–0xBF).
 * Does not validate; behavior on invalid input is undefined.
 *
 * @param str       Pointer to the UTF-8 byte sequence.
 * @param byte_len  Number of bytes in the string.
 * @return The number of Unicode code points.
 */
size_t decoder_utf8_length(const uint8_t *str, size_t byte_len);

/**
 * @brief Alias for decoder_utf8_length(). Returns the code point count.
 *
 * @param str       Pointer to the UTF-8 byte sequence.
 * @param byte_len  Number of bytes in the string.
 * @return The number of Unicode code points.
 */
size_t decoder_utf8_char_count(const uint8_t *str, size_t byte_len);

/**
 * @brief Converts a UTF-8 byte sequence to UTF-32 code points.
 *
 * @param src            Source UTF-8 bytes.
 * @param src_len        Number of source bytes.
 * @param dst            Destination buffer for UTF-32 code points.
 * @param dst_capacity   Maximum number of uint32_t values to write.
 * @param chars_written  Output: number of code points written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf8_to_utf32(const uint8_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                          size_t *chars_written);

/**
 * @brief Converts UTF-32 code points to a UTF-8 byte sequence.
 *
 * @param src            Source UTF-32 code points.
 * @param src_len        Number of source code points.
 * @param dst            Destination buffer for UTF-8 bytes.
 * @param dst_capacity   Maximum number of bytes to write.
 * @param bytes_written  Output: number of bytes written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf32_to_utf8(const uint32_t *src, size_t src_len, uint8_t *dst, size_t dst_capacity,
                          size_t *bytes_written);

/**
 * @brief Converts a UTF-8 byte sequence to UTF-16 code units.
 *
 * Supplementary characters (U+10000..U+10FFFF) are encoded as surrogate pairs.
 *
 * @param src            Source UTF-8 bytes.
 * @param src_len        Number of source bytes.
 * @param dst            Destination buffer for UTF-16 code units.
 * @param dst_capacity   Maximum number of uint16_t values to write.
 * @param units_written  Output: number of UTF-16 code units written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf8_to_utf16(const uint8_t *src, size_t src_len, uint16_t *dst, size_t dst_capacity,
                          size_t *units_written);

/**
 * @brief Converts UTF-16 code units to a UTF-8 byte sequence.
 *
 * Properly handles surrogate pairs for supplementary characters.
 *
 * @param src            Source UTF-16 code units.
 * @param src_len        Number of source code units.
 * @param dst            Destination buffer for UTF-8 bytes.
 * @param dst_capacity   Maximum number of bytes to write.
 * @param bytes_written  Output: number of bytes written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf16_to_utf8(const uint16_t *src, size_t src_len, uint8_t *dst, size_t dst_capacity,
                          size_t *bytes_written);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UTF-16
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Validates a UTF-16 code unit sequence.
 *
 * Checks for properly paired surrogates and rejects lone surrogates.
 *
 * @param str  Pointer to the UTF-16 code unit sequence.
 * @param len  Number of code units.
 * @return true if the sequence is well-formed UTF-16, false otherwise.
 */
bool decoder_is_valid_utf16(const uint16_t *str, size_t len);

/**
 * @brief Converts UTF-16 code units to UTF-32 code points.
 *
 * @param src            Source UTF-16 code units.
 * @param src_len        Number of source code units.
 * @param dst            Destination buffer for UTF-32 code points.
 * @param dst_capacity   Maximum number of uint32_t values to write.
 * @param chars_written  Output: number of code points written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf16_to_utf32(const uint16_t *src, size_t src_len, uint32_t *dst, size_t dst_capacity,
                           size_t *chars_written);

/**
 * @brief Converts UTF-32 code points to UTF-16 code units.
 *
 * @param src            Source UTF-32 code points.
 * @param src_len        Number of source code points.
 * @param dst            Destination buffer for UTF-16 code units.
 * @param dst_capacity   Maximum number of uint16_t values to write.
 * @param units_written  Output: number of code units written to dst.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_utf32_to_utf16(const uint32_t *src, size_t src_len, uint16_t *dst, size_t dst_capacity,
                           size_t *units_written);

/* ═══════════════════════════════════════════════════════════════════════════
 *  UTF-32
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Validates a UTF-32 code point sequence.
 *
 * Rejects surrogates (U+D800..U+DFFF) and values above U+10FFFF.
 *
 * @param str  Pointer to the UTF-32 code point sequence.
 * @param len  Number of code points.
 * @return true if every code point is a valid Unicode scalar value, false otherwise.
 */
bool decoder_is_valid_utf32(const uint32_t *str, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_ENCODING_H */
