#ifndef DRASTIC_BACKGROUND_FILES_H
#define DRASTIC_BACKGROUND_FILES_H
#include <stdbool.h>
#include <stddef.h>
#include <SDL2/SDL.h>
#include "drastic_config.h"
#ifdef __cplusplus
extern "C" {
#endif
/* CPU-only image loading. No SDL initialization, window or renderer is used. */
SDL_Surface *drastic_background_image_load(const char *path, char *error, size_t size);
bool drastic_background_cache_valid(const char *path);
/* Fast boot check: metadata and a small cache header, no image reads/writes. */
bool drastic_background_find_prepared(const char *path, const char *cache_directory,
                                      char *cache_path, size_t cache_size);

/* Immutable, tightly packed ARGB8888. The shared LRU retains up to 128 MiB;
 * active images stay pinned until their last owner releases them. */
typedef struct DrasticBackgroundImage {
  const uint32_t *pixels;
  unsigned width, height;
} DrasticBackgroundImage;
const DrasticBackgroundImage *drastic_background_image_acquire(
    const char *path, const char *cache_path, char *error, size_t size);
const DrasticBackgroundImage *drastic_background_image_retain(const DrasticBackgroundImage *image);
void drastic_background_image_release(const DrasticBackgroundImage *image);
void drastic_background_cache_clear(void);

/* One worker, one pending request: rapid navigation replaces queued work.
 * poll transfers an image reference on success; cancel never waits for I/O.
 * A NULL cache directory prepares previews entirely in RAM. */
typedef struct DrasticBackgroundLoader DrasticBackgroundLoader;
DrasticBackgroundLoader *drastic_background_loader_create(const char *cache_directory);
void drastic_background_loader_request(DrasticBackgroundLoader *loader,
    const DrasticRuntimeConfig *config, const char *path);
int drastic_background_loader_poll(DrasticBackgroundLoader *loader,
    DrasticRuntimeConfig *config, const DrasticBackgroundImage **image, char *error, size_t size);
void drastic_background_loader_cancel(DrasticBackgroundLoader *loader);
void drastic_background_loader_destroy(DrasticBackgroundLoader *loader);

#ifdef DRASTIC_BACKGROUND_TESTING
typedef struct { size_t bytes; int decodes, reads, writes; } DrasticBackgroundCacheStats;
DrasticBackgroundCacheStats drastic_background_cache_stats(void);
#endif
typedef struct { char path[1024]; char name[160]; } DrasticBackgroundEntry;
size_t drastic_background_scan(const char *directory, DrasticBackgroundEntry *entries, size_t capacity);
/* Optional image.png.layout: NXBGLAYOUT2, rotation, two normalized x/y/w/h rows.
 * Version 1 (implicit rotation 0) remains supported. */
bool drastic_background_apply_layout(DrasticRuntimeConfig *config, const char *path);
bool drastic_background_prepare_image(const char *path, const char *cache_directory,
                                      char *cache_path, size_t cache_size,
                                      char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
