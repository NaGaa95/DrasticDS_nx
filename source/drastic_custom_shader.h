#ifndef DRASTIC_NX_CUSTOM_SHADER_H
#define DRASTIC_NX_CUSTOM_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRASTIC_CUSTOM_SHADER_MAX_TEXTURES 16
#define DRASTIC_CUSTOM_SHADER_MAX_PASSES 16
#define DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS 16
#define DRASTIC_CUSTOM_SHADER_PATH_MAX 768
#define DRASTIC_CUSTOM_SHADER_NAME_MAX 96
#define DRASTIC_CUSTOM_SHADER_SAMPLER_MAX 64

enum DrasticCustomShaderLoadFlags {
  DRASTIC_CUSTOM_SHADER_LOAD_SOURCES = 1u << 0,
  DRASTIC_CUSTOM_SHADER_LOAD_PIXELS = 1u << 1,
  DRASTIC_CUSTOM_SHADER_LOAD_SPIRV = 1u << 2,
  DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D = 1u << 3,
};

typedef enum {
  DRASTIC_CUSTOM_TEXTURE_FRAMEBUFFER,
  DRASTIC_CUSTOM_TEXTURE_TARGET,
  DRASTIC_CUSTOM_TEXTURE_RAW,
} DrasticCustomTextureKind;

typedef enum {
  DRASTIC_CUSTOM_FORMAT_ALPHA,
  DRASTIC_CUSTOM_FORMAT_LUMINANCE,
  DRASTIC_CUSTOM_FORMAT_LUMINANCE_ALPHA,
  DRASTIC_CUSTOM_FORMAT_RGB,
  DRASTIC_CUSTOM_FORMAT_RGBA,
  DRASTIC_CUSTOM_FORMAT_RED,
  DRASTIC_CUSTOM_FORMAT_RG,
} DrasticCustomPixelFormat;

typedef struct {
  DrasticCustomTextureKind kind;
  DrasticCustomPixelFormat format;
  int width;
  int height;
  int channels;
  int min_linear;
  int mag_linear;
  int output_scale;
  char source_path[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  uint8_t *pixels;
  size_t pixels_size;
} DrasticCustomTexture;

typedef struct {
  char shader_path[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  int sampler_count;
  char sampler_names[DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS]
                    [DRASTIC_CUSTOM_SHADER_SAMPLER_MAX];
  uint8_t sampler_textures[DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS];
  int output_texture;
  int output_scale;
  char *vertex_source;
  char *fragment_source;
  uint8_t *vertex_spirv;
  size_t vertex_spirv_size;
  uint8_t *fragment_spirv;
  size_t fragment_spirv_size;
  uint8_t *vertex_deko3d;
  size_t vertex_deko3d_size;
  uint8_t *fragment_deko3d;
  size_t fragment_deko3d_size;
} DrasticCustomPass;

typedef struct {
  char name[DRASTIC_CUSTOM_SHADER_NAME_MAX];
  char relative_path[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  int texture_count;
  int pass_count;
  DrasticCustomTexture textures[DRASTIC_CUSTOM_SHADER_MAX_TEXTURES];
  DrasticCustomPass passes[DRASTIC_CUSTOM_SHADER_MAX_PASSES];
} DrasticCustomShader;

typedef struct {
  char name[DRASTIC_CUSTOM_SHADER_NAME_MAX];
  char relative_path[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  int vulkan_ready;
  int deko3d_ready;
} DrasticCustomShaderEntry;

bool drastic_custom_shader_load(const char *relative_path, unsigned flags,
                                DrasticCustomShader *shader,
                                char *error, size_t error_size);
void drastic_custom_shader_destroy(DrasticCustomShader *shader);

size_t drastic_custom_shader_scan(DrasticCustomShaderEntry *entries,
                                  size_t capacity);
bool drastic_custom_shader_vulkan_ready(const char *relative_path,
                                        char *error, size_t error_size);
bool drastic_custom_shader_deko3d_ready(const char *relative_path,
                                       char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
