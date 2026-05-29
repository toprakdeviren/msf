/**
 * @file decoder.h
 * @brief Public API of msf's Unicode decoder.
 *
 * msf uses this for exactly one job: normalizing Swift identifiers to NFC before
 * interning them.  Only the status enum, the normalization-form enum, and three
 * entry points are exposed; the rest of the original vendored decoder API
 * (validation, quick-check, comparison, combining-class / general-category /
 * script / segmentation / emoji queries, UTF-16) was unused and removed.
 */
#ifndef DECODER_H
#define DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return status codes (0 = success, negative = error). */
typedef enum {
    DECODER_SUCCESS = 0,
    DECODER_ERROR_INVALID_INPUT = -1,
    DECODER_ERROR_BUFFER_TOO_SMALL = -2,
    DECODER_ERROR_INVALID_UTF8 = -3,
    DECODER_ERROR_INVALID_UTF16 = -4,
    DECODER_ERROR_INVALID_CODEPOINT = -5,
    DECODER_ERROR_OUT_OF_MEMORY = -6,
    DECODER_ERROR_NOT_IMPLEMENTED = -7,
    DECODER_ERROR_IO = -8,
    DECODER_ERROR_INVALID_ARGUMENT = -9,
    DECODER_ERROR_OVERFLOW = -10,
    DECODER_ERROR_NOT_FOUND = -11
} decoder_status_t;

/** Canonical Unicode Normalization Forms (UAX #15). NFKC/NFKD not supported. */
typedef enum {
    DECODER_NFC = 0, /**< Canonical Decomposition, followed by Canonical Composition. */
    DECODER_NFD = 1  /**< Canonical Decomposition. */
} decoder_normalization_form_t;

/**
 * @brief Initializes the decoder. No-op (no global state); kept for API stability.
 */
void decoder_init(void);

/**
 * @brief Normalizes a UTF-8 string into the given form.
 * @return DECODER_SUCCESS, or a negative decoder_status_t on error.
 */
int decoder_normalize_utf8(const uint8_t *src, size_t src_len, decoder_normalization_form_t form,
                           uint8_t *dst, size_t dst_capacity, size_t *dst_len);

/**
 * @brief Reports whether a UTF-8 string is already in the given normalization form.
 */
bool decoder_is_normalized_utf8(const uint8_t *str, size_t len, decoder_normalization_form_t form);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_H */
