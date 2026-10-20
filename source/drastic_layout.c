#include "drastic_config.h"
#include "drastic_rotation.h"
#include <math.h>
#include <stddef.h>

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float clamp_float(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
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
