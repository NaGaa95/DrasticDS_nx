#ifndef DRASTIC_DKSH_H
#define DRASTIC_DKSH_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
  uint32_t magic, header_size, control_size, code_size, programs_offset, count;
} DrasticDkshHeader;

/* DKSH program IDs differ from DkStage. Only graphics vertex/fragment modules
 * are accepted here; bound all offsets before the driver reads their control. */
static inline bool drastic_dksh_valid(const void *blob, size_t size,
                                      unsigned stage) {
  if (!blob || size < 24 || size > 8u * 1024 * 1024 ||
      (stage != 0 && stage != 4)) return false;
  DrasticDkshHeader h;
  memcpy(&h, blob, sizeof(h));
  if (h.magic != 0x48534b44u || h.header_size != 24 || h.count != 1 ||
      !h.code_size || (h.control_size & 255) || (h.code_size & 255) ||
      h.programs_offset < 24 ||
      (uint64_t)h.programs_offset + 64 > h.control_size ||
      (uint64_t)h.control_size + h.code_size != size) return false;
  uint32_t p[16];
  memcpy(p, (const uint8_t *)blob + h.programs_offset, sizeof(p));
  return p[0] == (stage == 0 ? 0u : 1u) && p[1] < h.code_size &&
         p[2] <= 255 && (uint64_t)p[3] + p[4] <= h.code_size && p[5] <= 65536;
}
#endif
