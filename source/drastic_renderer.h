#ifndef DRASTIC_NX_RENDERER_H
#define DRASTIC_NX_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "drastic_config.h"
#include "overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DrasticCoreRenderFrame)(void *env, void *clazz,
                                       int texture_top, int texture_bottom,
                                       unsigned char swap);

typedef enum DrasticRendererBackend {
  DRASTIC_RENDERER_BACKEND_GL,
  DRASTIC_RENDERER_BACKEND_VULKAN,
  DRASTIC_RENDERER_BACKEND_DEKO3D,
} DrasticRendererBackend;

/* Selects the backend before the Android core is loaded.  Both native NVC0
 * and Zink use the OpenGL renderer; Mesa's EGL driver is selected separately
 * through the environment before the first eglInitialize call. */
void drastic_renderer_select(const char *name);
DrasticRendererBackend drastic_renderer_backend(void);
bool drastic_renderer_is_vulkan(void);
bool drastic_renderer_is_deko3d(void);
const char *drastic_renderer_backend_name(void);

bool drastic_renderer_init(const DrasticRuntimeConfig *config);
void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame);
void drastic_renderer_suspend(void);
void drastic_renderer_resume(void);
void drastic_renderer_shutdown(void);
/* Release imported core allocations before JNI_OnUnload frees them. */
void drastic_renderer_detach_core(void);
unsigned drastic_renderer_frame_count(void);
bool drastic_renderer_lsfg_available(void);
bool drastic_renderer_lsfg_enabled(void);
bool drastic_renderer_lsfg_request_enabled(bool enabled);
bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size);
const char *drastic_renderer_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
