/**
 * @file parallel.h
 * @brief Multi-threaded Unicode operations.
 *
 * Provides thread-pool-based parallel implementations of common operations
 * (case conversion, normalization, skeleton computation). The thread count
 * is configurable; by default, uses the number of available CPU cores.
 *
 * All parallel functions split the input into chunks, process each chunk
 * independently, and merge the results. The caller is responsible for
 * allocating output buffers of sufficient size.
 *
 * @note These functions are not available in the WASM build (single-threaded).
 */
#ifndef DECODER_PARALLEL_H
#define DECODER_PARALLEL_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sets the number of worker threads used by parallel operations.
 *
 * @param num_threads  Number of threads. Use 0 to auto-detect (number of CPU cores).
 *                     Use 1 to disable parallelism.
 */
void decoder_parallel_set_threads(int num_threads);

/**
 * @brief Returns the current number of worker threads.
 *
 * @return The thread count set by decoder_parallel_set_threads(), or
 *         the auto-detected count if never explicitly set.
 */
int decoder_parallel_get_threads(void);

/**
 * @brief Converts code points to uppercase in parallel.
 *
 * Each code point is converted independently (simple case mapping).
 * The output buffer must be at least len elements.
 *
 * @param input   Source code points.
 * @param len     Number of code points.
 * @param output  Destination buffer (must be at least len uint32_t).
 */
void decoder_parallel_to_upper(const uint32_t *input, size_t len, uint32_t *output);

/**
 * @brief Converts code points to lowercase in parallel.
 *
 * Each code point is converted independently (simple case mapping).
 * The output buffer must be at least len elements.
 *
 * @param input   Source code points.
 * @param len     Number of code points.
 * @param output  Destination buffer (must be at least len uint32_t).
 */
void decoder_parallel_to_lower(const uint32_t *input, size_t len, uint32_t *output);

/**
 * @brief Normalizes code points to NFC in parallel.
 *
 * Splits the input at safe boundaries (starters with CCC=0) and
 * normalizes each chunk independently.
 *
 * @param input       Source code points.
 * @param len         Number of source code points.
 * @param output      Destination buffer for NFC-normalized code points.
 * @param cap         Capacity of the output buffer.
 * @param output_len  Output: number of code points written.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_parallel_normalize_nfc(const uint32_t *input, size_t len, uint32_t *output, size_t cap,
                                   size_t *output_len);

/**
 * @brief Normalizes code points to NFD in parallel.
 *
 * Splits the input at safe boundaries and decomposes each chunk independently.
 *
 * @param input       Source code points.
 * @param len         Number of source code points.
 * @param output      Destination buffer for NFD-normalized code points.
 * @param cap         Capacity of the output buffer.
 * @param output_len  Output: number of code points written.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_parallel_normalize_nfd(const uint32_t *input, size_t len, uint32_t *output, size_t cap,
                                   size_t *output_len);

/**
 * @brief Computes the confusable skeleton of a string in parallel.
 *
 * Splits the input at safe points and computes the skeleton (UTS #39, §4)
 * for each segment independently.
 *
 * @param input       Source code points.
 * @param len         Number of source code points.
 * @param output      Destination buffer for skeleton code points.
 * @param cap         Capacity of the output buffer.
 * @param output_len  Output: number of code points written.
 * @return DECODER_SUCCESS on success, or a negative decoder_status_t on error.
 */
int decoder_parallel_get_skeleton(const uint32_t *input, size_t len, uint32_t *output, size_t cap,
                                  size_t *output_len);

/**
 * @brief Low-level parallel for-each primitive.
 *
 * Divides the range [0, count) into chunks and calls the worker function
 * for each chunk on a separate thread. Blocks until all workers complete.
 *
 * @param count   Total number of items.
 * @param worker  Callback invoked with (start, end, ctx) for each chunk.
 * @param ctx     User context pointer passed to each worker invocation.
 */
void decoder_parallel_for(size_t count, void (*worker)(size_t start, size_t end, void *ctx),
                          void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* DECODER_PARALLEL_H */
