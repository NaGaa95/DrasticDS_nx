#ifndef DRASTIC_DEKO_RENDERER_H
#define DRASTIC_DEKO_RENDERER_H
#include "drastic_renderer.h"
#ifdef __cplusplus
extern "C" {
#endif
bool drastic_deko_renderer_init(const DrasticRuntimeConfig *config);
void drastic_deko_renderer_present(const DrasticRuntimeConfig *config,
    DrasticCoreRenderFrame core_render, void *env, void *clazz,
    const DrasticOverlayFrame *overlay, bool consume_core_frame);
void drastic_deko_renderer_suspend(void);
void drastic_deko_renderer_resume(void);
void drastic_deko_renderer_detach_core(void);
void drastic_deko_renderer_shutdown(void);
unsigned drastic_deko_renderer_frame_count(void);
bool drastic_deko_renderer_lsfg_available(void);
bool drastic_deko_renderer_lsfg_enabled(void);
bool drastic_deko_renderer_lsfg_request_enabled(bool enabled);
bool drastic_deko_renderer_set_custom_shader(const char *path,
                                            char *error, size_t error_size);
const char *drastic_deko_renderer_last_error(void);
#ifdef __cplusplus
}
#endif
#endif
