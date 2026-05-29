/**
 * @file stubs.c
 * @brief Stub implementations for external dependencies not available in native test builds.
 *
 * The Unicode decoder (NFC normalization) is now vendored into the library
 * itself (libs/decoder/), so it is NO LONGER stubbed here — the real
 * normalization runs in tests and benchmarks.  Only the SDK module table is
 * stubbed, since the real module_stubs.c lives in the miniswift backend.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
