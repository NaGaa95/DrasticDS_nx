#include "drastic_renderer.h"
#include "drastic_deko_renderer.h"

#include <string.h>

/* The implementations intentionally keep independent state.  This small
 * dispatcher is the only public renderer ABI used by the rest of the host. */
bool drastic_gl_renderer_init(const DrasticRuntimeConfig *config);
void drastic_gl_renderer_present(const DrasticRuntimeConfig *config,
                                 DrasticCoreRenderFrame core_render,
                                 void *env, void *clazz,
                                 const DrasticOverlayFrame *overlay,
                                 bool consume_core_frame);
void drastic_gl_renderer_suspend(void);
void drastic_gl_renderer_resume(void);
void drastic_gl_renderer_shutdown(void);
unsigned drastic_gl_renderer_frame_count(void);
bool drastic_gl_renderer_lsfg_available(void);
bool drastic_gl_renderer_lsfg_enabled(void);
bool drastic_gl_renderer_lsfg_request_enabled(bool enabled);
bool drastic_gl_renderer_set_custom_shader(const char *relative_path,
                                           char *error, size_t error_size);
const char *drastic_gl_renderer_last_error(void);

bool drastic_vk_renderer_init(const DrasticRuntimeConfig *config);
void drastic_vk_renderer_present(const DrasticRuntimeConfig *config,
                                 DrasticCoreRenderFrame core_render,
                                 void *env, void *clazz,
                                 const DrasticOverlayFrame *overlay,
                                 bool consume_core_frame);
void drastic_vk_renderer_suspend(void);
void drastic_vk_renderer_resume(void);
void drastic_vk_renderer_shutdown(void);
unsigned drastic_vk_renderer_frame_count(void);
bool drastic_vk_renderer_lsfg_available(void);
bool drastic_vk_renderer_lsfg_enabled(void);
bool drastic_vk_renderer_lsfg_request_enabled(bool enabled);
bool drastic_vk_renderer_set_custom_shader(const char *relative_path,
                                           char *error, size_t error_size);
const char *drastic_vk_renderer_last_error(void);

static DrasticRendererBackend g_backend = DRASTIC_RENDERER_BACKEND_DEKO3D;

void drastic_renderer_select(const char *name) {
  if (name && !strcmp(name, "vk")) {
    g_backend = DRASTIC_RENDERER_BACKEND_VULKAN;
    return;
  }
  g_backend = name && (!strcmp(name, "gl") || !strcmp(name, "zink"))
                  ? DRASTIC_RENDERER_BACKEND_GL
                  : DRASTIC_RENDERER_BACKEND_DEKO3D;
}

DrasticRendererBackend drastic_renderer_backend(void) { return g_backend; }

bool drastic_renderer_is_vulkan(void) {
  return g_backend == DRASTIC_RENDERER_BACKEND_VULKAN;
}

bool drastic_renderer_is_deko3d(void) {
  return g_backend == DRASTIC_RENDERER_BACKEND_DEKO3D;
}

const char *drastic_renderer_backend_name(void) {
  if (drastic_renderer_is_deko3d()) return "Deko3D";
  return drastic_renderer_is_vulkan() ? "Vulkan" : "OpenGL";
}

bool drastic_renderer_init(const DrasticRuntimeConfig *config) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_init(config);
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_init(config)
                                      : drastic_gl_renderer_init(config);
}

void drastic_renderer_present(const DrasticRuntimeConfig *config,
                              DrasticCoreRenderFrame core_render,
                              void *env, void *clazz,
                              const DrasticOverlayFrame *overlay,
                              bool consume_core_frame) {
  if (drastic_renderer_is_deko3d())
    drastic_deko_renderer_present(config, core_render, env, clazz, overlay,
                                  consume_core_frame);
  else if (drastic_renderer_is_vulkan())
    drastic_vk_renderer_present(config, core_render, env, clazz, overlay,
                                consume_core_frame);
  else
    drastic_gl_renderer_present(config, core_render, env, clazz, overlay,
                                consume_core_frame);
}

void drastic_renderer_suspend(void) {
  if (drastic_renderer_is_deko3d()) { drastic_deko_renderer_suspend(); return; }
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_suspend();
  else drastic_gl_renderer_suspend();
}

void drastic_renderer_resume(void) {
  if (drastic_renderer_is_deko3d()) { drastic_deko_renderer_resume(); return; }
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_resume();
  else drastic_gl_renderer_resume();
}

void drastic_renderer_shutdown(void) {
  if (drastic_renderer_is_deko3d()) { drastic_deko_renderer_shutdown(); return; }
  if (drastic_renderer_is_vulkan()) drastic_vk_renderer_shutdown();
  else drastic_gl_renderer_shutdown();
}

void drastic_renderer_detach_core(void) {
  if (drastic_renderer_is_deko3d()) drastic_deko_renderer_detach_core();
}

unsigned drastic_renderer_frame_count(void) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_frame_count();
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_frame_count()
                                      : drastic_gl_renderer_frame_count();
}

bool drastic_renderer_lsfg_available(void) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_lsfg_available();
  return drastic_renderer_is_vulkan() &&
         drastic_vk_renderer_lsfg_available();
}

bool drastic_renderer_lsfg_enabled(void) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_lsfg_enabled();
  return drastic_renderer_is_vulkan() && drastic_vk_renderer_lsfg_enabled();
}

bool drastic_renderer_lsfg_request_enabled(bool enabled) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_lsfg_request_enabled(enabled);
  return drastic_renderer_is_vulkan()
             ? drastic_vk_renderer_lsfg_request_enabled(enabled)
             : drastic_gl_renderer_lsfg_request_enabled(enabled);
}

bool drastic_renderer_set_custom_shader(const char *relative_path,
                                        char *error, size_t error_size) {
  if (drastic_renderer_is_deko3d())
    return drastic_deko_renderer_set_custom_shader(relative_path, error, error_size);
  return drastic_renderer_is_vulkan()
             ? drastic_vk_renderer_set_custom_shader(relative_path, error,
                                                     error_size)
             : drastic_gl_renderer_set_custom_shader(relative_path, error,
                                                     error_size);
}

const char *drastic_renderer_last_error(void) {
  if (drastic_renderer_is_deko3d()) return drastic_deko_renderer_last_error();
  return drastic_renderer_is_vulkan() ? drastic_vk_renderer_last_error()
                                      : drastic_gl_renderer_last_error();
}
