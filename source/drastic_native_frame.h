#ifndef DRASTIC_NATIVE_FRAME_H
#define DRASTIC_NATIVE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r2.6.0.4a / 109 software display ABI. Each bank reserves two maximum-size
 * 512x384 RGBA screens, including when a screen is currently 256x192/RGB565. */
#define DRASTIC_NATIVE_SCREEN_BYTES 0xc0000u
#define DRASTIC_NATIVE_BANK_BYTES   0x180000u
#define DRASTIC_NATIVE_POOL_BYTES   0x300000u

typedef struct {
  const void *allocation;
  const void *pixels[2];
  unsigned width[2], height[2], bytes_per_pixel;
  unsigned bank, dirty;
} DrasticNativeFrame;

typedef void (*DrasticNativeFrameWait)(void *lease);

/* Install before so_finalize. Validates every touched instruction first. */
bool drastic_native_frame_install(so_module *module);
/* A successful lock must be paired with unlock, including on render failure.
 * The mutex prevents the producer from recycling the bank until its DMA fence
 * has been published. The producer waits only if it actually reuses that bank. */
bool drastic_native_frame_lock(DrasticNativeFrame *frame);
void drastic_native_frame_unlock(void *lease, DrasticNativeFrameWait wait,
                                 unsigned uploaded);
/* Call after draining GPU work and before the core releases its allocation. */
void drastic_native_frame_detach(void);

#ifdef __cplusplus
}
#endif
#endif
