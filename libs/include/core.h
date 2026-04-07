/**
 * @file core.h
 * @brief Core lifecycle and diagnostics for the Decoder library.
 *
 * Provides initialization, cleanup, version querying, and error reporting.
 * Must be called before any other decoder_* function is used.
 */
#ifndef DECODER_CORE_H
#define DECODER_CORE_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the Decoder library.
 *
 * Loads internal lookup tables, initializes the SIMD dispatch layer, and
 * prepares all subsystems (normalization, segmentation, security, etc.).
 * This function is thread-safe and idempotent — calling it more than once
 * has no additional effect.
 *
 * @note Must be called before any other decoder_* function.
 */
void decoder_init(void);

/**
 * @brief Cleans up all resources held by the Decoder library.
 *
 * Releases internal lookup tables, SIMD state, and any heap-allocated
 * structures. After calling this function, decoder_init() must be called
 * again before using any other decoder_* function.
 */
void decoder_cleanup(void);

/**
 * @brief Returns the Unicode Standard version supported by this build.
 *
 * @return A null-terminated string such as "17.0.0". The returned pointer
 *         is valid for the lifetime of the program (static storage).
 */
const char *decoder_get_unicode_version(void);

/**
 * @brief Returns a human-readable error message for a status code.
 *
 * @param status A decoder_status_t value returned by any decoder function.
 * @return A null-terminated string describing the error. The returned pointer
 *         is valid for the lifetime of the program (static storage).
 *         Returns "Unknown error" for unrecognized status codes.
 */
const char *decoder_error_message(decoder_status_t status);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_CORE_H */
