#include <switch.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drastic_config.h"
#include "drastic_rotation.h"
#include "prefs.h"

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static unsigned system_firmware_language(void) {
  unsigned firmware_language = 1; /* English fallback. */
  if (R_FAILED(setInitialize())) return firmware_language;

  u64 language_code = 0;
  SetLanguage language = SetLanguage_ENUS;
  const Result get_result = setGetSystemLanguage(&language_code);
  const Result convert_result = R_SUCCEEDED(get_result)
      ? setMakeLanguage(language_code, &language) : get_result;
  setExit();
  if (R_FAILED(get_result) || R_FAILED(convert_result))
    return firmware_language;

  switch (language) {
    case SetLanguage_JA: firmware_language = 0; break;
    case SetLanguage_ENUS:
    case SetLanguage_ENGB: firmware_language = 1; break;
    case SetLanguage_FR:
    case SetLanguage_FRCA: firmware_language = 2; break;
    case SetLanguage_DE: firmware_language = 3; break;
    case SetLanguage_IT: firmware_language = 4; break;
    case SetLanguage_ES:
    case SetLanguage_ES419: firmware_language = 5; break;
    case SetLanguage_KO: firmware_language = 6; break;
    default: break;
  }
  return firmware_language;
}

static uint64_t flag_if(bool enabled, unsigned bit) {
  return enabled ? (UINT64_C(1) << bit) : 0;
}

uint64_t drastic_config_build_core_config(void) {
  const int frameskip = clamp_int(prefs_get_int("Drastic/FrameskipValue", 0), 0, 9);
  const int frameskip_type = clamp_int(prefs_get_int("Drastic/FrameskipType", 0), 0, 3);
  const int audio_latency = clamp_int(prefs_get_int("Drastic/AudioLatency", 2), 0, 3);
  const int fast_forward = clamp_int(prefs_get_int("Drastic/FastForwardSpeed", 2), 0, 5);
  const int cpu_threads = clamp_int(prefs_get_int("Drastic/CpuThreads", 3), 1, 3);
  const int autofire = clamp_int(prefs_get_int("Drastic/AutoFireSpeed", 2), 0, 7);
  const int mic_level = clamp_int(prefs_get_int("Drastic/MicLevel", 1), 0, 3);
  const int slot2 = clamp_int(prefs_get_int("Drastic/Slot2Type", 1), 0, 5);

  uint64_t value = (uint64_t)frameskip;
  value |= (uint64_t)frameskip_type << 5;
  value |= (uint64_t)audio_latency << 8;
  value |= (uint64_t)fast_forward << 12;
  value |= (uint64_t)cpu_threads << 16;
  value |= (uint64_t)autofire << 32;
  value |= (uint64_t)mic_level << 37;
  value |= (uint64_t)slot2 << 43;

  value |= flag_if(prefs_get_bool("Drastic/SoundEnabled", true), 31);
  value |= flag_if(prefs_get_bool("Drastic/ShowFPS", false), 30);
  value |= flag_if(prefs_get_bool("Drastic/Threaded3D", true), 28);
  value |= flag_if(prefs_get_bool("Drastic/CheatsEnabled", true), 27);
  value |= flag_if(prefs_get_bool("Drastic/MicEnabled", true), 26);
  value |= flag_if(prefs_get_bool("Drastic/BackupInSavestates", true), 25);
  value |= flag_if(prefs_get_bool("Drastic/IgnoreGamecardLimit", false), 24);
  value |= flag_if(prefs_get_bool("Drastic/Use16BitColor", false), 23);
  value |= flag_if(prefs_get_bool("Drastic/AutoTrim", false), 36);
  value |= flag_if(prefs_get_bool("Drastic/FixMainEngineScreen", false), 35);
  value |= flag_if(prefs_get_bool("Drastic/RtcSystemTime", true), 39);
  value |= flag_if(prefs_get_bool("Drastic/DisableEdgeMarking", false), 40);
  value |= flag_if(prefs_get_bool("Drastic/Hires3D", false), 41);
  value |= flag_if(prefs_get_bool("Drastic/LuaEnabled", true), 42);
  value |= flag_if(prefs_get_bool("Drastic/FrameskipSafe", false), 47);
  value |= flag_if(prefs_get_bool("Drastic/PreloadRoms", true), 48);
  value |= flag_if(prefs_get_bool("Drastic/Blend", false), 49);
  value |= flag_if(prefs_get_bool("Drastic/RawSaveFormat", false), 50);
  return value;
}

static DrasticLayoutMode read_layout(void) {
  const char *layout = prefs_get_string("Wrapper/Layout", "horizontal");
  if (!strcmp(layout, "vertical")) return DRASTIC_LAYOUT_VERTICAL;
  if (!strcmp(layout, "horizontal")) return DRASTIC_LAYOUT_HORIZONTAL;
  if (!strcmp(layout, "top")) return DRASTIC_LAYOUT_TOP_ONLY;
  if (!strcmp(layout, "bottom")) return DRASTIC_LAYOUT_BOTTOM_ONLY;
  if (!strcmp(layout, "hybrid_top")) return DRASTIC_LAYOUT_HYBRID_TOP;
  if (!strcmp(layout, "hybrid_bottom")) return DRASTIC_LAYOUT_HYBRID_BOTTOM;
  if (!strcmp(layout, "custom")) return DRASTIC_LAYOUT_CUSTOM;
  return DRASTIC_LAYOUT_HORIZONTAL;
}

static DrasticVideoFilter read_filter(void) {
  const char *filter = prefs_get_string(
      "Wrapper/VideoFilter",
      prefs_get_string("Wrapper/TextureFilter", "nearest"));
  if (!strcmp(filter, "linear")) return DRASTIC_FILTER_LINEAR;
  if (!strcmp(filter, "quilez")) return DRASTIC_FILTER_QUILEZ;
  if (!strcmp(filter, "scanline")) return DRASTIC_FILTER_SCANLINE;
  if (!strcmp(filter, "scale2x")) return DRASTIC_FILTER_SCALE2X;
  if (!strcmp(filter, "hq2x")) return DRASTIC_FILTER_HQ2X;
  if (!strcmp(filter, "fxaa")) return DRASTIC_FILTER_FXAA;
  if (!strcmp(filter, "fxaa_hq")) return DRASTIC_FILTER_FXAA_HQ;
  if (!strcmp(filter, "smaa")) return DRASTIC_FILTER_SMAA;
  if (!strcmp(filter, "custom")) return DRASTIC_FILTER_CUSTOM;
  return DRASTIC_FILTER_NEAREST;
}

static DrasticMicrophoneSource read_microphone_source(void) {
  const char *source = prefs_get_string("Wrapper/MicrophoneSource", "noise");
  return !strcmp(source, "external") ? DRASTIC_MICROPHONE_EXTERNAL
                                     : DRASTIC_MICROPHONE_SIMULATED;
}

static DrasticStylusMode read_stylus_mode(void) {
  const char *mode = prefs_get_string("Wrapper/StylusMode", "stick");
  if (!strcmp(mode, "off")) return DRASTIC_STYLUS_OFF;
  if (!strcmp(mode, "motion")) return DRASTIC_STYLUS_MOTION;
  return DRASTIC_STYLUS_STICK;
}

const char *drastic_config_filter_name(DrasticVideoFilter filter) {
  static const char *names[DRASTIC_FILTER_COUNT] = {
    "nearest", "linear", "quilez", "scanline", "scale2x", "hq2x", "fxaa",
    "fxaa_hq", "smaa", "custom"
  };
  if ((unsigned)filter >= DRASTIC_FILTER_COUNT) return names[0];
  return names[filter];
}

static float clamp_float(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

void drastic_config_load(DrasticRuntimeConfig *config) {
  memset(config, 0, sizeof(*config));
  snprintf(config->rom_path, sizeof(config->rom_path), "%s",
           prefs_get_string("Drastic/RomPath", DEFAULT_ROM_PATH));
  snprintf(config->core_path, sizeof(config->core_path), "%s",
           prefs_get_string("Wrapper/CoreSo", SO_NAME));
  snprintf(config->launcher_path, sizeof(config->launcher_path), "%s",
           prefs_get_string("Wrapper/LauncherPath", ""));
  snprintf(config->firmware_nickname, sizeof(config->firmware_nickname), "%s",
           prefs_get_string("Drastic/FirmwareNickname", "Switch"));
  const int configured_language =
      prefs_get_int("Drastic/FirmwareLanguage", -1);
  const unsigned firmware_language = configured_language >= 0 &&
      configured_language <= 6 ? (unsigned)configured_language
                               : system_firmware_language();
  const unsigned firmware_color = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareColor", 0), 0, 15);
  const unsigned firmware_month = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareBirthdayMonth", 6), 1, 12);
  const unsigned firmware_day = (unsigned)clamp_int(
      prefs_get_int("Drastic/FirmwareBirthdayDay", 6), 1, 31);
  config->firmware_userdata = firmware_language | (firmware_color << 8) |
                              (firmware_month << 16) | (firmware_day << 24);
  config->layout = read_layout();
  config->swap_screens = prefs_get_bool("Wrapper/SwapScreens", false);
  config->rotation = clamp_int(prefs_get_int("Wrapper/Rotation", 0), 0, 3);
  config->screen_gap = clamp_int(prefs_get_int("Wrapper/ScreenGap", 8), 0, 128);
  config->integer_scale = prefs_get_bool("Wrapper/IntegerScale", false);
  config->custom_aspect_lock =
      prefs_get_bool("Wrapper/CustomAspectLock", true);
  config->low_latency = prefs_get_bool("Wrapper/LowLatency", true);
  config->video_filter = read_filter();
  snprintf(config->custom_shader, sizeof(config->custom_shader), "%s",
           prefs_get_string("Wrapper/CustomShader", ""));
  config->show_fps = prefs_get_bool("Drastic/ShowFPS", false);
  config->volume = clamp_int(prefs_get_int("Wrapper/Volume", 100), 0, 100);
  config->microphone_enabled =
      prefs_get_bool("Drastic/MicEnabled", true);
  config->microphone_source = read_microphone_source();
  config->autosave_seconds = clamp_int(
      prefs_get_int("Drastic/AutosaveInterval", 300), 0, 3600);
  config->vibration = prefs_get_bool("Wrapper/Vibration", true);
  config->motion = prefs_get_bool("Wrapper/Motion", true);
  config->lua_enabled = prefs_get_bool("Drastic/LuaEnabled", true);
  config->stylus_mode = read_stylus_mode();
  config->mouse_stylus = prefs_get_bool("Wrapper/MouseStylus", true);
  config->motion_stylus_sensitivity = clamp_int(
      prefs_get_int("Wrapper/MotionStylusSensitivity", 10), 1, 20);
  config->stylus_x = 128;
  config->stylus_y = 96;
  config->core_config = drastic_config_build_core_config();
  static const float defaults[2][4] = {
    {0.03f, 0.20666667f, 0.44f, 0.58666667f},
    {0.53f, 0.20666667f, 0.44f, 0.58666667f},
  };
  static const char *keys[2][4] = {
    {"Wrapper/CustomTopX", "Wrapper/CustomTopY",
     "Wrapper/CustomTopW", "Wrapper/CustomTopH"},
    {"Wrapper/CustomBottomX", "Wrapper/CustomBottomY",
     "Wrapper/CustomBottomW", "Wrapper/CustomBottomH"},
  };
  for (int screen = 0; screen < 2; screen++) {
    config->custom_screens[screen].x = clamp_float(
        prefs_get_float(keys[screen][0], defaults[screen][0]), 0.0f, 0.95f);
    config->custom_screens[screen].y = clamp_float(
        prefs_get_float(keys[screen][1], defaults[screen][1]), 0.0f, 0.95f);
    config->custom_screens[screen].width = clamp_float(
        prefs_get_float(keys[screen][2], defaults[screen][2]), 0.05f, 1.0f);
    config->custom_screens[screen].height = clamp_float(
        prefs_get_float(keys[screen][3], defaults[screen][3]), 0.05f, 1.0f);
    if (config->custom_screens[screen].x +
        config->custom_screens[screen].width > 1.0f)
      config->custom_screens[screen].x =
          1.0f - config->custom_screens[screen].width;
    if (config->custom_screens[screen].y +
        config->custom_screens[screen].height > 1.0f)
      config->custom_screens[screen].y =
          1.0f - config->custom_screens[screen].height;
    config->custom_screens[screen].screen = screen;
    config->custom_screens[screen].touch_target = screen == 1;
  }
}

static void fit_size(float available_width, float available_height,
                     int integer_scale, int rotation,
                     float *width, float *height) {
  const float native_width = (rotation & 1) ? 192.0f : 256.0f;
  const float native_height = (rotation & 1) ? 256.0f : 192.0f;
  float scale = fminf(available_width / native_width,
                      available_height / native_height);
  if (integer_scale && scale >= 1.0f) scale = floorf(scale);
  if (scale <= 0.0f) scale = 1.0f;
  *width = native_width * scale;
  *height = native_height * scale;
}

static int remap_screen(const DrasticRuntimeConfig *config, int screen) {
  return config->swap_screens ? 1 - screen : screen;
}

static void enforce_custom_native_aspect(DrasticScreenRect *rect,
                                         int rotation,
                                         int canvas_width,
                                         int canvas_height) {
  if (!rect || canvas_width <= 0 || canvas_height <= 0) return;
  const float physical_aspect = (rotation & 1) ? 3.0f / 4.0f : 4.0f / 3.0f;
  const float normalized_aspect =
      physical_aspect * (float)canvas_height / (float)canvas_width;
  if (normalized_aspect <= 0.0f) return;

  const float center_x = rect->x + rect->width * 0.5f;
  const float center_y = rect->y + rect->height * 0.5f;
  const float height_from_width = rect->width / normalized_aspect;
  const float width_from_height = rect->height * normalized_aspect;
  const float width_cost = fabsf(height_from_width - rect->height) * canvas_height;
  const float height_cost = fabsf(width_from_height - rect->width) * canvas_width;
  float height = width_cost <= height_cost ? height_from_width : rect->height;
  const float minimum_height = fmaxf(0.08f, 0.08f / normalized_aspect);
  const float maximum_height = fminf(1.0f, 1.0f / normalized_aspect);
  height = clamp_float(height, minimum_height, maximum_height);
  rect->height = height;
  rect->width = height * normalized_aspect;
  rect->x = clamp_float(center_x - rect->width * 0.5f,
                        0.0f, 1.0f - rect->width);
  rect->y = clamp_float(center_y - rect->height * 0.5f,
                        0.0f, 1.0f - rect->height);
}

static void set_rect(DrasticRuntimeConfig *config, int index, int screen,
                     float x, float y, float width, float height) {
  config->screens[index].x = x;
  config->screens[index].y = y;
  config->screens[index].width = width;
  config->screens[index].height = height;
  config->screens[index].screen = remap_screen(config, screen);
  config->screens[index].touch_target =
      config->screens[index].screen == 1;
}

void drastic_config_calculate_layout(DrasticRuntimeConfig *config,
                                     int width, int height) {
  const float gap = (float)config->screen_gap;
  config->screen_count = 0;

  if (config->layout == DRASTIC_LAYOUT_CUSTOM) {
    for (int screen = 0; screen < 2; screen++) {
      DrasticScreenRect *custom = &config->custom_screens[screen];
      if (config->custom_aspect_lock)
        enforce_custom_native_aspect(custom, config->rotation, width, height);
      set_rect(config, screen, screen,
               custom->x * width, custom->y * height,
               custom->width * width, custom->height * height);
    }
    config->screen_count = 2;
    return;
  }

  if (config->layout == DRASTIC_LAYOUT_VERTICAL) {
    float w, h;
    fit_size((float)width, ((float)height - gap) * 0.5f,
             config->integer_scale, config->rotation, &w, &h);
    const float x = ((float)width - w) * 0.5f;
    const float y = ((float)height - (h * 2.0f + gap)) * 0.5f;
    set_rect(config, 0, 0, x, y, w, h);
    set_rect(config, 1, 1, x, y + h + gap, w, h);
    config->screen_count = 2;
  } else if (config->layout == DRASTIC_LAYOUT_HORIZONTAL) {
    float w, h;
    fit_size(((float)width - gap) * 0.5f, (float)height,
             config->integer_scale, config->rotation, &w, &h);
    const float x = ((float)width - (w * 2.0f + gap)) * 0.5f;
    const float y = ((float)height - h) * 0.5f;
    set_rect(config, 0, 0, x, y, w, h);
    set_rect(config, 1, 1, x + w + gap, y, w, h);
    config->screen_count = 2;
  } else if (config->layout == DRASTIC_LAYOUT_TOP_ONLY ||
             config->layout == DRASTIC_LAYOUT_BOTTOM_ONLY) {
    float w, h;
    fit_size((float)width, (float)height, config->integer_scale,
             config->rotation, &w, &h);
    const int screen = config->layout == DRASTIC_LAYOUT_TOP_ONLY ? 0 : 1;
    set_rect(config, 0, screen, ((float)width - w) * 0.5f,
             ((float)height - h) * 0.5f, w, h);
    config->screen_count = 1;
  } else {
    const int primary = config->layout == DRASTIC_LAYOUT_HYBRID_TOP ? 0 : 1;
    const float side_width = fminf((float)width * 0.27f, 384.0f);
    float small_w, small_h;
    fit_size(side_width, ((float)height - gap) * 0.5f,
             config->integer_scale, config->rotation, &small_w, &small_h);
    float large_w, large_h;
    fit_size((float)width - small_w - gap, (float)height,
             config->integer_scale, config->rotation, &large_w, &large_h);
    const float total_w = large_w + gap + small_w;
    const float x = ((float)width - total_w) * 0.5f;
    set_rect(config, 0, primary, x, ((float)height - large_h) * 0.5f,
             large_w, large_h);
    const float small_y = ((float)height - (small_h * 2.0f + gap)) * 0.5f;
    set_rect(config, 1, 0, x + large_w + gap, small_y, small_w, small_h);
    set_rect(config, 2, 1, x + large_w + gap, small_y + small_h + gap,
             small_w, small_h);
    config->screen_count = 3;
  }
}

bool drastic_config_map_touch_rects(const DrasticScreenRect *screens,
                                    int screen_count, int rotation,
                                    float panel_x, float panel_y,
                                    int *ds_x, int *ds_y) {
  if (!screens || screen_count <= 0) return false;
  /* Prefer the largest bottom-screen rectangle in hybrid modes. */
  const DrasticScreenRect *target = NULL;
  for (int index = 0; index < screen_count; index++) {
    const DrasticScreenRect *rect = &screens[index];
    if (!rect->touch_target || rect->width <= 0.0f ||
        rect->height <= 0.0f || panel_x < rect->x || panel_y < rect->y ||
        panel_x >= rect->x + rect->width ||
        panel_y >= rect->y + rect->height)
      continue;
    if (!target || rect->width * rect->height > target->width * target->height)
      target = rect;
  }
  if (!target) return false;
  const float display_u = (panel_x - target->x) / target->width;
  const float display_v = (panel_y - target->y) / target->height;
  float source_u, source_v;
  drastic_rotation_display_to_source(rotation, display_u, display_v,
                                     &source_u, &source_v);
  if (ds_x) *ds_x = clamp_int((int)(source_u * 256.0f), 0, 255);
  if (ds_y) *ds_y = clamp_int((int)(source_v * 192.0f), 0, 191);
  return true;
}

bool drastic_config_map_touch(const DrasticRuntimeConfig *config,
                              float panel_x, float panel_y,
                              int *ds_x, int *ds_y) {
  if (!config) return false;
  return drastic_config_map_touch_rects(
      config->screens, config->screen_count, config->rotation,
      panel_x, panel_y, ds_x, ds_y);
}

bool drastic_config_map_stylus(const DrasticRuntimeConfig *config,
                               int ds_x, int ds_y,
                               float *panel_x, float *panel_y) {
  const DrasticScreenRect *target = NULL;
  for (int index = 0; index < config->screen_count; index++) {
    const DrasticScreenRect *rect = &config->screens[index];
    if (!rect->touch_target) continue;
    if (!target || rect->width * rect->height > target->width * target->height)
      target = rect;
  }
  if (!target) return false;
  float source_u = (float)clamp_int(ds_x, 0, 255) / 255.0f;
  float source_v = (float)clamp_int(ds_y, 0, 191) / 191.0f;
  float display_u, display_v;
  drastic_rotation_source_to_display(config->rotation, source_u, source_v,
                                     &display_u, &display_v);
  if (panel_x) *panel_x = target->x + display_u * target->width;
  if (panel_y) *panel_y = target->y + display_v * target->height;
  return true;
}
