#ifndef DRASTIC_NX_RUNTIME_CONFIG_H
#define DRASTIC_NX_RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "drastic_custom_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DRASTIC_LAYOUT_VERTICAL,
  DRASTIC_LAYOUT_HORIZONTAL,
  DRASTIC_LAYOUT_TOP_ONLY,
  DRASTIC_LAYOUT_BOTTOM_ONLY,
  DRASTIC_LAYOUT_HYBRID_TOP,
  DRASTIC_LAYOUT_HYBRID_BOTTOM,
  DRASTIC_LAYOUT_CUSTOM,
} DrasticLayoutMode;

typedef enum {
  DRASTIC_FILTER_NEAREST,
  DRASTIC_FILTER_LINEAR,
  DRASTIC_FILTER_QUILEZ,
  DRASTIC_FILTER_SCANLINE,
  DRASTIC_FILTER_SCALE2X,
  DRASTIC_FILTER_HQ2X,
  DRASTIC_FILTER_FXAA,
  DRASTIC_FILTER_FXAA_HQ,
  DRASTIC_FILTER_SMAA,
  DRASTIC_FILTER_CUSTOM,
  DRASTIC_FILTER_COUNT,
} DrasticVideoFilter;

typedef enum {
  DRASTIC_MICROPHONE_SIMULATED,
  DRASTIC_MICROPHONE_EXTERNAL,
} DrasticMicrophoneSource;

typedef enum {
  DRASTIC_STYLUS_OFF,
  DRASTIC_STYLUS_STICK,
  DRASTIC_STYLUS_MOTION,
} DrasticStylusMode;

typedef struct {
  float x;
  float y;
  float width;
  float height;
  int screen;       /* 0 = DS top, 1 = DS bottom */
  int touch_target; /* accepts touchscreen input */
} DrasticScreenRect;

typedef struct {
  char rom_path[1024];
  char core_path[256];
  char launcher_path[1024];
  char firmware_nickname[32];
  DrasticLayoutMode layout;
  int swap_screens;
  int rotation;
  int screen_gap;
  int integer_scale;
  int custom_aspect_lock;
  int low_latency;
  DrasticVideoFilter video_filter;
  char custom_shader[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  int show_fps;
  int volume;
  int microphone_enabled;
  DrasticMicrophoneSource microphone_source;
  int autosave_seconds;
  int vibration;
  int motion;
  int lua_enabled;
  DrasticStylusMode stylus_mode;
  int mouse_stylus;
  int motion_stylus_sensitivity;
  int stylus_x;
  int stylus_y;
  int stylus_visible;
  int screen_count;
  uint32_t firmware_userdata;
  uint64_t core_config;
  DrasticScreenRect screens[3];
  /* Normalized custom rectangles. Index 0 is the DS top screen. */
  DrasticScreenRect custom_screens[2];
} DrasticRuntimeConfig;

void drastic_config_load(DrasticRuntimeConfig *config);
uint64_t drastic_config_build_core_config(void);
const char *drastic_config_filter_name(DrasticVideoFilter filter);
void drastic_config_calculate_layout(DrasticRuntimeConfig *config,
                                     int width, int height);
bool drastic_config_map_touch(const DrasticRuntimeConfig *config,
                              float panel_x, float panel_y,
                              int *ds_x, int *ds_y);
bool drastic_config_map_touch_rects(const DrasticScreenRect *screens,
                                    int screen_count, int rotation,
                                    float panel_x, float panel_y,
                                    int *ds_x, int *ds_y);
bool drastic_config_map_stylus(const DrasticRuntimeConfig *config,
                               int ds_x, int ds_y,
                               float *panel_x, float *panel_y);

#ifdef __cplusplus
}
#endif

#endif
