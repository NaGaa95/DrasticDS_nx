#ifndef DRASTIC_BACKGROUND_H
#define DRASTIC_BACKGROUND_H

#include "drastic_config.h"
#include "overlay.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Launcher-generated, little-endian ARGB8888 cache. Bounded before allocation. */
#define DRASTIC_BACKGROUND_MAGIC "NXBGv1\0"
#define DRASTIC_BACKGROUND_MAX_EDGE 1920u
#define DRASTIC_BACKGROUND_MAX_PIXELS (1920u * 1920u)
typedef struct {
  char magic[8];
  uint32_t width, height;
} DrasticBackgroundHeader;

void drastic_background_init(const DrasticRuntimeConfig *config);
struct DrasticBackgroundImage;
/* Activates worker-loaded pixels without copying or accessing storage. */
bool drastic_background_set_cached_image(const DrasticRuntimeConfig *config,
                                         const struct DrasticBackgroundImage *image);
/* Caller releases the reference; used to pin the picker Cancel target. */
const struct DrasticBackgroundImage *drastic_background_acquire_current_image(void);
/* Launcher preview: decoded ARGB pixels, copied once; no cache file writes. */
bool drastic_background_set_image(const DrasticRuntimeConfig *config,
                                  const uint32_t *pixels, unsigned width, unsigned height);
bool drastic_background_select(DrasticRuntimeConfig *config, const char *path, const char *cache_directory,
                                char *error, size_t error_size);
void drastic_background_shutdown(void);
/* Reuses the existing GPU overlay. Rebuilds only when layout or UI changes. */
const DrasticOverlayFrame *drastic_background_compose(
    const DrasticRuntimeConfig *config, int panel_width, int panel_height,
    const DrasticOverlayFrame *ui);

#ifdef __cplusplus
}
#endif

#endif
