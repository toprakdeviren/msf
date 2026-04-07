/**
 * @file stubs.c
 * @brief Stub implementations for external dependencies not available in native test builds.
 *
 * libunicode (decoder) is a WASM-only library. For native unit tests we provide
 * no-op stubs that pass through without normalization. Module stubs are also
 * stubbed out since we don't link against the generated module_stubs.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── decoder stubs (libunicode) ───────────────────────────────────────────── */

void decoder_init(void) { /* no-op */ }
void decoder_cleanup(void) { /* no-op */ }

bool decoder_is_normalized_utf8(const uint8_t *str, size_t len, int form) {
  (void)str; (void)len; (void)form;
  return true; /* assume already NFC — skip normalization in tests */
}

int decoder_normalize_utf8(const uint8_t *src, size_t src_len, int form,
                           uint8_t *dst, size_t dst_capacity, size_t *dst_len) {
  (void)form;
  if (src_len > dst_capacity) return -2; /* DECODER_ERROR_BUFFER_TOO_SMALL */
  memcpy(dst, src, src_len);
  *dst_len = src_len;
  return 0; /* DECODER_SUCCESS */
}

/* ── module_stubs ─────────────────────────────────────────────────────────── */

typedef struct {
  const char *module;
  const void *types;
  uint32_t count;
} ModuleStub;

const ModuleStub *module_stub_find(const char *module_name) {
  (void)module_name;
  return NULL; /* no modules available in test environment */
}
