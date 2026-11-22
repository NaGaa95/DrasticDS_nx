#include <switch.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drastic_renderer.h"
#include "drastic_background.h"
#include "drastic_background_files.h"
#include "ingame_menu.h"
#include "jni_fake.h"
#include "opensles.h"
#include "overlay.h"
#include "prefs.h"

#define MENU_CHEAT_LIMIT 4096
#define MENU_FOLDER_LIMIT 128
#define MENU_STICK_ACTIVATION 18000
#define MENU_STICK_RELEASE 8000
#define MENU_STICK_REPEAT_DELAY_MS 360
#define MENU_STICK_REPEAT_RATE_MS 85

enum MenuPage {
  MENU_MAIN,
  MENU_STATES,
  MENU_CHEATS,
  MENU_DISPLAY,
  MENU_FILTER_PICKER,
  MENU_BACKGROUND_PICKER,
  MENU_EMULATION,
  MENU_AUDIO_INPUT,
  MENU_LAYOUT_EDITOR,
  MENU_PAGE_COUNT,
};

typedef struct {
  char name[144];
  char note[320];
  int index;
  int folder;
  int custom;
  int enabled;
} MenuCheat;

struct DrasticIngameMenu {
  DrasticRuntimeConfig *config;
  DrasticMenuCore core;
  int *state_slot;
  enum MenuPage page;
  int selection[MENU_PAGE_COUNT];
  int scroll[MENU_PAGE_COUNT];
  int open;
  bool lid_closed;
  int exit_requested;
  int redraw;
  int pending_snapshot;
  int confirm_delete_slot;
  int snapshot_valid;
  int32_t *snapshot_top;
  int32_t *snapshot_bottom;
  void *snapshot_top_array;
  void *snapshot_bottom_array;
  MenuCheat *cheats;
  int cheat_count;
  char folders[MENU_FOLDER_LIMIT][96];
  unsigned char folder_multi_select[MENU_FOLDER_LIMIT];
  int folder_count;
  int persisted_cheats_applied;
  char status[192];
  DrasticVideoFilter filter_backup;
  char filter_backup_shader[DRASTIC_CUSTOM_SHADER_PATH_MAX];
  DrasticCustomShaderEntry custom_shaders[128];
  int custom_shader_count;
  int filter_picker_index;
  int filter_picker_custom;
  int filter_picker_valid;
  DrasticBackgroundEntry *backgrounds;
  int background_count, background_index, background_valid;
  DrasticBackgroundLoader *background_loader;
  const DrasticBackgroundImage *background_backup_image;
  DrasticRuntimeConfig background_backup;
  u64 marquee_tick;
  u64 analog_nav_direction;
  u64 analog_nav_since;
  u64 analog_nav_last;
  OpenSLESMicrophoneStatus microphone_status;
  DrasticLayoutMode editor_old_layout;
  int editor_old_aspect_lock;
  DrasticScreenRect editor_backup[2];
  int editor_screen;
};

static const uint32_t COLOR_DIM = 0x79000000u;
static const uint32_t COLOR_PANEL = 0xff101a2bu;
static const uint32_t COLOR_PANEL_ALT = 0xff18253au;
static const uint32_t COLOR_FRAME = 0xff528ab1u;
static const uint32_t COLOR_ACCENT = 0xff63c7ffu;
static const uint32_t COLOR_SELECTED = 0xff234f70u;
static const uint32_t COLOR_TEXT = 0xfff2f7fbu;
static const uint32_t COLOR_MUTED = 0xffa9bfd0u;
static const uint32_t COLOR_VALUE = 0xffc4e2f6u;
static const uint32_t COLOR_DISABLED = 0xff91a3b5u;
static const uint32_t COLOR_GOOD = 0xff8de5b0u;
static const uint32_t COLOR_WARN = 0xffffca75u;

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

static void reset_analog_navigation(DrasticIngameMenu *menu) {
  menu->analog_nav_direction = 0;
  menu->analog_nav_since = 0;
  menu->analog_nav_last = 0;
}

static u64 analog_navigation_direction(const DrasticIngameMenu *menu,
                                       HidAnalogStickState stick) {
  const int x = abs(stick.x);
  const int y = abs(stick.y);
  if (x >= MENU_STICK_ACTIVATION || y >= MENU_STICK_ACTIVATION) {
    if (y >= x)
      return stick.y > 0 ? HidNpadButton_Up : HidNpadButton_Down;
    return stick.x > 0 ? HidNpadButton_Right : HidNpadButton_Left;
  }

  /* Keep the current direction latched until the stick returns close to its
   * centre.  This hysteresis prevents a slightly noisy stick from producing
   * extra menu steps around the activation threshold. */
  switch (menu->analog_nav_direction) {
    case HidNpadButton_Up:
      return stick.y > MENU_STICK_RELEASE ? HidNpadButton_Up : 0;
    case HidNpadButton_Down:
      return stick.y < -MENU_STICK_RELEASE ? HidNpadButton_Down : 0;
    case HidNpadButton_Left:
      return stick.x < -MENU_STICK_RELEASE ? HidNpadButton_Left : 0;
    case HidNpadButton_Right:
      return stick.x > MENU_STICK_RELEASE ? HidNpadButton_Right : 0;
    default: return 0;
  }
}

static u64 analog_navigation_pressed(DrasticIngameMenu *menu,
                                     HidAnalogStickState stick) {
  const u64 direction = analog_navigation_direction(menu, stick);
  const u64 now = armGetSystemTick();
  if (direction != menu->analog_nav_direction) {
    menu->analog_nav_direction = direction;
    menu->analog_nav_since = now;
    menu->analog_nav_last = now;
    return direction;
  }
  if (!direction) return 0;

  const u64 frequency = armGetSystemTickFreq();
  if (!frequency) return 0;
  const u64 delay = frequency * MENU_STICK_REPEAT_DELAY_MS / 1000u;
  const u64 rate = frequency * MENU_STICK_REPEAT_RATE_MS / 1000u;
  if (now - menu->analog_nav_since < delay ||
      now - menu->analog_nav_last < rate)
    return 0;
  menu->analog_nav_last = now;
  return direction;
}

static int ui_width(void) { return overlay_width(); }

static int ui_height(void) { return overlay_height(); }

static int ui_is_portrait(void) { return ui_height() > ui_width(); }

static void set_status(DrasticIngameMenu *menu, const char *status) {
  snprintf(menu->status, sizeof(menu->status), "%s", status ? status : "");
  menu->redraw = 1;
}

static void save_bool(const char *key, int value) {
  prefs_set_bool(key, value != 0);
  prefs_save_runtime_key(key);
}

static void save_int(const char *key, int value) {
  prefs_set_int(key, value);
  prefs_save_runtime_key(key);
}

static void save_string(const char *key, const char *value) {
  prefs_set_string(key, value);
  prefs_save_runtime_key(key);
}

static void apply_core_config(DrasticIngameMenu *menu) {
  menu->config->core_config = drastic_config_build_core_config();
  if (menu->core.apply_config)
    menu->core.apply_config(menu->core.env, menu->core.clazz,
                            (DrasticJLong)menu->config->core_config);
}

static void copy_java_bytes(void *array, char *destination, size_t size) {
  if (!destination || !size) return;
  destination[0] = '\0';
  const uint8_t *data = jni_byte_array_data(array);
  int length = jni_byte_array_length(array);
  if (data && length > 0) {
    while (length > 0 && !data[length - 1]) length--;
    if ((size_t)length >= size) length = (int)size - 1;
    memcpy(destination, data, (size_t)length);
    destination[length] = '\0';
  }
  jni_release_byte_array(array);
}

static void refresh_snapshot(DrasticIngameMenu *menu) {
  if (!menu->core.get_snapshots || !menu->snapshot_top ||
      !menu->snapshot_bottom) {
    menu->snapshot_valid = 0;
    return;
  }
  memset(menu->snapshot_top, 0, 256 * 192 * sizeof(*menu->snapshot_top));
  memset(menu->snapshot_bottom, 0, 256 * 192 * sizeof(*menu->snapshot_bottom));
  menu->core.get_snapshots(menu->core.env, menu->core.clazz,
                           *menu->state_slot, menu->snapshot_top_array,
                           menu->snapshot_bottom_array);
  menu->snapshot_valid = 0;
  for (int index = 0; index < 256 * 192; index++) {
    if (menu->snapshot_top[index] || menu->snapshot_bottom[index]) {
      menu->snapshot_valid = 1;
      break;
    }
  }
  menu->redraw = 1;
}

static int state_filename_matches_slot(const char *name, int slot) {
  if (!name) return 0;
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "_%d.dss", slot);
  const size_t name_length = strlen(name);
  const size_t suffix_length = strlen(suffix);
  return name_length > suffix_length &&
         !strcmp(name + name_length - suffix_length, suffix);
}

static int delete_matching_state(DrasticIngameMenu *menu, int slot) {
  if (!menu->snapshot_valid || !menu->core.get_snapshots_direct) return 0;
  void *top_array = jni_make_int_array(256 * 192);
  void *bottom_array = jni_make_int_array(256 * 192);
  int32_t *top = jni_int_array_data(top_array);
  int32_t *bottom = jni_int_array_data(bottom_array);
  if (!top || !bottom) {
    jni_release_int_array(top_array);
    jni_release_int_array(bottom_array);
    return 0;
  }
  int matches = 0;
  char matched_path[1200] = {0};
  DIR *directory = opendir(SAVESTATES_DIR);
  struct dirent *entry;
  while (directory && (entry = readdir(directory))) {
    if (!state_filename_matches_slot(entry->d_name, slot)) continue;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", SAVESTATES_DIR, entry->d_name);
    memset(top, 0, 256 * 192 * sizeof(*top));
    memset(bottom, 0, 256 * 192 * sizeof(*bottom));
    void *java_path = jni_make_string(path);
    menu->core.get_snapshots_direct(menu->core.env, menu->core.clazz,
                                    java_path, top_array, bottom_array);
    jni_release_string(java_path);
    if (memcmp(top, menu->snapshot_top, 256 * 192 * sizeof(*top)) ||
        memcmp(bottom, menu->snapshot_bottom, 256 * 192 * sizeof(*bottom)))
      continue;
    matches++;
    snprintf(matched_path, sizeof(matched_path), "%s", path);
  }
  if (directory) closedir(directory);
  const int deleted = matches == 1 && remove(matched_path) == 0;
  jni_release_int_array(top_array);
  jni_release_int_array(bottom_array);
  return deleted;
}

static void free_cheats(DrasticIngameMenu *menu) {
  free(menu->cheats);
  menu->cheats = NULL;
  menu->cheat_count = 0;
  menu->folder_count = 0;
}

static int hex_digit_value(int character) {
  if (character >= '0' && character <= '9') return character - '0';
  character = toupper((unsigned char)character);
  return character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
}

static int enabled_list_contains(const char *list, int wanted) {
  if (!list) return 0;
  if (!strncmp(list, "hex:", 4)) {
    const char *bitmap = list + 4;
    const size_t digit = (unsigned)wanted >> 2;
    if (digit >= strlen(bitmap)) return 0;
    const int value = hex_digit_value((unsigned char)bitmap[digit]);
    return value >= 0 && (value & (1 << (wanted & 3)));
  }
  const char *cursor = list;
  while (*cursor) {
    while (*cursor == ',' || isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) break;
    char *end = NULL;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor) {
      while (*cursor && *cursor != ',') cursor++;
      continue;
    }
    if (value == wanted) return 1;
    cursor = end;
    while (*cursor && *cursor != ',') cursor++;
  }
  return 0;
}

void drastic_menu_apply_persisted_cheats(DrasticIngameMenu *menu) {
  if (!menu || menu->persisted_cheats_applied) return;
  menu->persisted_cheats_applied = 1;
  if (!prefs_contains("Wrapper/EnabledDatabaseCheats") ||
      !menu->core.get_cheat_count || !menu->core.set_cheat_enabled) return;
  const char *enabled = prefs_get_string("Wrapper/EnabledDatabaseCheats", "");
  const int count = clamp_int(
      menu->core.get_cheat_count(menu->core.env, menu->core.clazz),
      0, MENU_CHEAT_LIMIT);
  for (int index = 0; index < count; index++)
    menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz, index,
                                 enabled_list_contains(enabled, index));
  if (menu->core.update_cheats)
    menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
}

static void persist_database_cheats(DrasticIngameMenu *menu) {
  static const char hexadecimal[] = "0123456789abcdef";
  unsigned char bitmap[MENU_CHEAT_LIMIT / 8] = {0};
  int highest = -1;
  for (int index = 0; index < menu->cheat_count; index++) {
    const MenuCheat *cheat = &menu->cheats[index];
    if (cheat->custom || !cheat->enabled) continue;
    if ((unsigned)cheat->index >= MENU_CHEAT_LIMIT) continue;
    bitmap[(unsigned)cheat->index >> 3] |=
        (unsigned char)(1u << (cheat->index & 7));
    if (cheat->index > highest) highest = cheat->index;
  }
  char enabled[5 + MENU_CHEAT_LIMIT / 4] = "hex:";
  const int digits = highest < 0 ? 0 : highest / 4 + 1;
  for (int digit = 0; digit < digits; digit++)
    enabled[4 + digit] = hexadecimal[
        (bitmap[digit >> 1] >> ((digit & 1) * 4)) & 0x0f];
  enabled[4 + digits] = '\0';
  save_string("Wrapper/EnabledDatabaseCheats", enabled);
}

static void refresh_cheats(DrasticIngameMenu *menu) {
  drastic_menu_apply_persisted_cheats(menu);
  free_cheats(menu);
  const int database_count = menu->core.get_cheat_count
      ? clamp_int(menu->core.get_cheat_count(menu->core.env, menu->core.clazz),
                  0, MENU_CHEAT_LIMIT)
      : 0;
  const int custom_count = menu->core.get_custom_cheat_count
      ? clamp_int(menu->core.get_custom_cheat_count(menu->core.env,
                                                    menu->core.clazz),
                  0, MENU_CHEAT_LIMIT)
      : 0;
  const int total = clamp_int(database_count + custom_count,
                              0, MENU_CHEAT_LIMIT);
  if (total) menu->cheats = calloc((size_t)total, sizeof(*menu->cheats));
  if (total && !menu->cheats) {
    set_status(menu, "Not enough memory to load the cheat list");
    return;
  }
  menu->folder_count = menu->core.get_cheat_folder_count
      ? clamp_int(menu->core.get_cheat_folder_count(menu->core.env,
                                                    menu->core.clazz),
                  0, MENU_FOLDER_LIMIT)
      : 0;
  for (int index = 0; index < menu->folder_count; index++)
    {
      menu->folder_multi_select[index] =
          !menu->core.get_cheat_folder_multi_select ||
          menu->core.get_cheat_folder_multi_select(
              menu->core.env, menu->core.clazz, index);
      if (menu->core.get_cheat_folder_name)
      copy_java_bytes(menu->core.get_cheat_folder_name(
                          menu->core.env, menu->core.clazz, index),
                      menu->folders[index], sizeof(menu->folders[index]));
    }

  int output = 0;
  for (int index = 0; index < database_count && output < total; index++) {
    MenuCheat *cheat = &menu->cheats[output++];
    cheat->index = index;
    cheat->folder = menu->core.get_cheat_folder_id
        ? menu->core.get_cheat_folder_id(menu->core.env, menu->core.clazz,
                                         index)
        : -1;
    cheat->enabled = menu->core.get_cheat_enabled &&
        menu->core.get_cheat_enabled(menu->core.env, menu->core.clazz, index);
    if (menu->core.get_cheat_name)
      copy_java_bytes(menu->core.get_cheat_name(menu->core.env,
                                                menu->core.clazz, index),
                      cheat->name, sizeof(cheat->name));
    if (menu->core.get_cheat_note)
      copy_java_bytes(menu->core.get_cheat_note(menu->core.env,
                                                menu->core.clazz, index),
                      cheat->note, sizeof(cheat->note));
    if (!cheat->name[0]) snprintf(cheat->name, sizeof(cheat->name),
                                  "Cheat %d", index + 1);
  }
  for (int index = 0; index < custom_count && output < total; index++) {
    MenuCheat *cheat = &menu->cheats[output++];
    cheat->index = index;
    cheat->folder = -1;
    cheat->custom = 1;
    cheat->enabled = menu->core.get_custom_cheat_enabled &&
        menu->core.get_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                            index);
    if (menu->core.get_custom_cheat_name)
      copy_java_bytes(menu->core.get_custom_cheat_name(
                          menu->core.env, menu->core.clazz, index),
                      cheat->name, sizeof(cheat->name));
    if (!cheat->name[0]) snprintf(cheat->name, sizeof(cheat->name),
                                  "Custom cheat %d", index + 1);
  }
  menu->cheat_count = output;
  const int maximum = menu->cheat_count; /* + Add entry at index zero. */
  menu->selection[MENU_CHEATS] =
      clamp_int(menu->selection[MENU_CHEATS], 0, maximum);
  menu->redraw = 1;
}

static int prompt_keyboard(const char *header, const char *guide,
                           const char *initial, char *output,
                           size_t output_size, int multiline) {
  if (!output || output_size < 2) return 0;
  SwkbdConfig keyboard;
  Result result = swkbdCreate(&keyboard, 0);
  if (R_FAILED(result)) return 0;
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard, header ? header : "Drastic DS");
  if (guide) swkbdConfigSetGuideText(&keyboard, guide);
  if (initial && *initial) swkbdConfigSetInitialText(&keyboard, initial);
  swkbdConfigSetStringLenMax(&keyboard, (u32)output_size - 1);
  swkbdConfigSetReturnButtonFlag(&keyboard, multiline ? 1 : 0);
  result = swkbdShow(&keyboard, output, output_size);
  swkbdClose(&keyboard);
  return R_SUCCEEDED(result) && output[0];
}

static int parse_cheat_words(const char *text, int32_t *words, int capacity) {
  int count = 0;
  const char *cursor = text;
  while (cursor && *cursor) {
    while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',' ||
                       *cursor == ';' || *cursor == ':')) cursor++;
    if (!*cursor) break;
    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X'))
      cursor += 2;
    const char *start = cursor;
    unsigned long value = 0;
    int digits = 0;
    while (isxdigit((unsigned char)*cursor) && digits < 8) {
      const int character = *cursor++;
      value <<= 4;
      value |= character >= '0' && character <= '9' ? character - '0'
          : (character & ~32) - 'A' + 10;
      digits++;
    }
    if (!digits || isxdigit((unsigned char)*cursor) || cursor == start ||
        count >= capacity) return -1;
    words[count++] = (int32_t)value;
  }
  return count >= 2 && !(count & 1) ? count : -1;
}

static void add_custom_cheat(DrasticIngameMenu *menu) {
  if (!menu->core.add_custom_cheat) {
    set_status(menu, "Custom cheats are unavailable in this core");
    return;
  }
  char name[96] = {0};
  char codes[4096] = {0};
  if (!prompt_keyboard("New custom cheat", "Enter a name", "", name,
                       sizeof(name), 0)) {
    menu->redraw = 1;
    return;
  }
  if (!prompt_keyboard("Action Replay code",
                       "Enter hexadecimal address/value pairs", "", codes,
                       sizeof(codes), 1)) {
    menu->redraw = 1;
    return;
  }
  int32_t words[512];
  const int count = parse_cheat_words(codes, words,
                                      (int)(sizeof(words) / sizeof(*words)));
  if (count < 0) {
    set_status(menu, "Invalid code: use 8-digit address/value pairs");
    return;
  }
  void *array = jni_make_int_array(count);
  int32_t *data = jni_int_array_data(array);
  if (!data) {
    set_status(menu, "Could not allocate the cheat code");
    return;
  }
  memcpy(data, words, (size_t)count * sizeof(*data));
  void *java_name = jni_make_string(name);
  const int result = menu->core.add_custom_cheat(
      menu->core.env, menu->core.clazz, java_name, array, count, 1);
  jni_release_string(java_name);
  jni_release_int_array(array);
  if (menu->core.update_cheats)
    menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
  refresh_cheats(menu);
  set_status(menu, result ? "Custom cheat added" :
                            "Drastic rejected the custom cheat");
}

enum {
  MENU_ROW_HEIGHT = 40,
  MENU_ROW_STEP = 44,
  MENU_HEADER_HEIGHT = 64,
  MENU_PADDING = 22,
};

typedef struct {
  int x, y, width, height;
} MenuPanel;

typedef enum {
  ROW_ACTION,
  ROW_LINK,
  ROW_ADJUST,
  ROW_PREVIEW,
  ROW_TOGGLE,
} MenuRowType;

typedef struct {
  const char *label;
  const char *value;
  MenuRowType type;
  bool on;
  bool unavailable;
} MenuRow;

static void draw_centered(int x, int y, int width, DrasticOverlayTextSize size,
                          uint32_t color, const char *text) {
  const int offset = (width - overlay_text_width(size, text)) / 2;
  overlay_draw_text_clipped(x + (offset > 0 ? offset : 0), y, width, size,
                            color, text);
}

static void draw_surface(MenuPanel panel) {
  overlay_fill_round_rect(panel.x, panel.y, panel.width, panel.height, 15,
                          COLOR_FRAME);
  overlay_fill_round_rect(panel.x + 1, panel.y + 1, panel.width - 2,
                          panel.height - 2, 14, COLOR_PANEL);
}

static MenuPanel draw_panel(int preferred_width, int content_height,
                            const char *title, const char *actions) {
  const int width = clamp_int(preferred_width, 1, ui_width() - 40);
  const int height = MENU_HEADER_HEIGHT + content_height + (actions ? 44 : 16);
  const MenuPanel panel = {
    .x = (ui_width() - width) / 2,
    .y = (ui_height() - height) / 2,
    .width = width,
    .height = height,
  };
  overlay_fill_rect(0, 0, ui_width(), ui_height(), COLOR_DIM);
  draw_surface(panel);
  draw_centered(panel.x + MENU_PADDING, panel.y + 12,
                width - 2 * MENU_PADDING, OVERLAY_TEXT_TITLE, COLOR_TEXT, title);
  overlay_fill_rect(panel.x + MENU_PADDING, panel.y + 54,
                    width - 2 * MENU_PADDING, 1, COLOR_PANEL_ALT);
  if (actions)
    draw_centered(panel.x + MENU_PADDING, panel.y + height - 33,
                  width - 2 * MENU_PADDING, OVERLAY_TEXT_SMALL, COLOR_MUTED,
                  actions);
  return panel;
}

static void draw_status(const DrasticIngameMenu *menu, MenuPanel panel) {
  if (!menu->status[0]) return;
  const int width = clamp_int(overlay_text_width(OVERLAY_TEXT_SMALL, menu->status)
                              + 34, 1, panel.width);
  const int x = (ui_width() - width) / 2;
  const int height = 34;
  const int y = clamp_int(panel.y + panel.height + 10, 8,
                          ui_height() - height - 6);
  overlay_fill_round_rect(x, y, width, height, 9, COLOR_PANEL_ALT);
  draw_centered(x + 14, y + 5, width - 28, OVERLAY_TEXT_SMALL, COLOR_WARN,
                menu->status);
}

static void draw_chevron(int x, int y, bool left, uint32_t color) {
  for (int row = -4; row <= 4; ++row) {
    const int offset = 4 - abs(row);
    overlay_fill_rect(x + (left ? -offset : offset), y + row, 2, 1, color);
  }
}

static void draw_toggle(int right, int center, bool on) {
  const int width = 76;
  const int left = right - width;
  overlay_fill_round_rect(left, center - 14, width, 28, 14,
                          on ? 0xff296b9bu : 0xff3a485au);
  const int knob = on ? right - 23 : left + 5;
  overlay_fill_round_rect(knob, center - 9, 18, 18, 9, COLOR_TEXT);
  draw_centered(on ? left + 7 : left + 27, center - 12, 42,
                OVERLAY_TEXT_SMALL, COLOR_TEXT, on ? "On" : "Off");
}

static void draw_row(int x, int y, int width, bool selected, MenuRow row) {
  const int center = y + MENU_ROW_HEIGHT / 2;
  const int label_x = x + 15;
  int right = x + width - 15;
  const int value_y = center - overlay_text_height(OVERLAY_TEXT_SMALL) / 2;
  const uint32_t value_color = row.unavailable ? COLOR_DISABLED :
      selected ? COLOR_ACCENT : COLOR_VALUE;
  if (selected)
    overlay_fill_round_rect(x, y, width, MENU_ROW_HEIGHT, 8, COLOR_SELECTED);

  if (row.type == ROW_TOGGLE) {
    draw_toggle(right, center, row.on);
    right -= 92;
  } else {
    if (row.type == ROW_LINK || row.type == ROW_ADJUST || row.type == ROW_PREVIEW) {
      draw_chevron(right - 6, center, false, value_color);
      right -= 17;
    }
    if (row.value && *row.value) {
      const int max_value = width / 2 - 18;
      const int text_width = overlay_text_width(OVERLAY_TEXT_SMALL, row.value);
      const int value_width = text_width < max_value ? text_width : max_value;
      const int value_x = right - value_width;
      if (selected && row.type == ROW_PREVIEW && text_width > max_value)
        overlay_draw_text_scrolling(value_x, value_y, value_width,
                                    OVERLAY_TEXT_SMALL, value_color, row.value);
      else
        overlay_draw_text_clipped(value_x, value_y, value_width,
                                  OVERLAY_TEXT_SMALL, value_color, row.value);
      right = value_x - 14;
      if (row.type == ROW_ADJUST) {
        draw_chevron(right + 6, center, true, value_color);
        right -= 8;
      }
    }
  }

  overlay_draw_text_clipped(label_x, center - overlay_text_height(OVERLAY_TEXT_BODY) / 2,
                            right - label_x, OVERLAY_TEXT_BODY,
                            row.unavailable ? COLOR_DISABLED : COLOR_TEXT,
                            row.label);
}

static void draw_rows(MenuPanel panel, int selected, const MenuRow *rows, int count) {
  for (int i = 0; i < count; ++i)
    draw_row(panel.x + MENU_PADDING, panel.y + MENU_HEADER_HEIGHT + i * MENU_ROW_STEP,
             panel.width - 2 * MENU_PADDING, i == selected, rows[i]);
}

enum MainMenuItem {
  MAIN_RESUME,
  MAIN_LID,
  MAIN_STATES,
  MAIN_CHEATS,
  MAIN_DISPLAY,
  MAIN_EMULATION,
  MAIN_AUDIO_INPUT,
#ifdef USE_VULKAN
  MAIN_LSFG,
#endif
  MAIN_RESET,
  MAIN_QUIT,
  MAIN_ITEM_COUNT,
};

static int main_item_available(int item) {
#ifdef USE_VULKAN
  if (item == MAIN_LSFG) return drastic_renderer_lsfg_available();
#else
  (void)item;
#endif
  return 1;
}

static void render_main(DrasticIngameMenu *menu) {
  const MenuRow rows[MAIN_ITEM_COUNT] = {
    [MAIN_RESUME] = {.label = "Resume Game"},
    [MAIN_LID] = {.label = menu->lid_closed ? "Open DS lid" : "Close DS lid"},
    [MAIN_STATES] = {.label = "Save states", .type = ROW_LINK},
    [MAIN_CHEATS] = {.label = "Cheats", .type = ROW_LINK},
    [MAIN_DISPLAY] = {.label = "Screen layout & filters", .type = ROW_LINK},
    [MAIN_EMULATION] = {.label = "Emulation options", .type = ROW_LINK},
    [MAIN_AUDIO_INPUT] = {.label = "Audio, input & motion", .type = ROW_LINK},
#ifdef USE_VULKAN
    [MAIN_LSFG] = main_item_available(MAIN_LSFG)
        ? (MenuRow){.label = "Frame generation", .type = ROW_TOGGLE,
                    .on = drastic_renderer_lsfg_enabled()}
        : (MenuRow){.label = "Frame generation", .value = "Unavailable",
                    .unavailable = true},
#endif
    [MAIN_RESET] = {.label = "Reset game"},
    [MAIN_QUIT] = {.label = "Quit to launcher"},
  };
  const MenuPanel panel = draw_panel(590, MAIN_ITEM_COUNT * MENU_ROW_STEP,
                                     "DraStic Quick Menu", NULL);
  draw_rows(panel, menu->selection[MENU_MAIN], rows, MAIN_ITEM_COUNT);
  draw_status(menu, panel);
}

static void render_states(DrasticIngameMenu *menu) {
  enum { SLOT_COUNT = 10, PREVIEW_HEIGHT = 280, COLUMN_GAP = 18 };
  const bool portrait = ui_is_portrait();
  const int list_height = SLOT_COUNT * MENU_ROW_STEP;
  const int content_height = portrait
      ? list_height + COLUMN_GAP + PREVIEW_HEIGHT : list_height;
  const MenuPanel panel = draw_panel(1016, content_height, "Save states",
      "A  Load      X  Save / overwrite      Y  Delete      B  Back");

  const int x = panel.x + MENU_PADDING;
  const int y = panel.y + MENU_HEADER_HEIGHT;
  const int width = panel.width - 2 * MENU_PADDING;
  const int list_width = portrait ? width : 258;
  const int selected = menu->selection[MENU_STATES];
  const bool saving = menu->core.is_saving &&
      menu->core.is_saving(menu->core.env, menu->core.clazz);
  const int saving_slot = saving && menu->core.get_saving_slot
      ? menu->core.get_saving_slot(menu->core.env, menu->core.clazz) : -1;
  for (int slot = 0; slot < SLOT_COUNT; ++slot) {
    char label[24];
    snprintf(label, sizeof(label), "Slot %d", slot);
    const char *value = slot == saving_slot ? "Saving..." :
        slot == *menu->state_slot ? "Active" : NULL;
    draw_row(x, y + slot * MENU_ROW_STEP, list_width, slot == selected,
             (MenuRow){.label = label, .value = value});
  }

  const int preview_x = portrait ? x : x + list_width + COLUMN_GAP;
  const int preview_y = portrait ? y + list_height + COLUMN_GAP :
      y + (list_height - PREVIEW_HEIGHT) / 2;
  const int preview_width = portrait ? width : width - list_width - COLUMN_GAP;
  overlay_fill_round_rect(preview_x, preview_y, preview_width, PREVIEW_HEIGHT,
                          12, COLOR_PANEL_ALT);
  char heading[48];
  snprintf(heading, sizeof(heading), "Slot %d preview", selected);
  draw_centered(preview_x, preview_y + 12, preview_width, OVERLAY_TEXT_BODY,
                COLOR_TEXT, heading);
  if (menu->snapshot_valid) {
    const int image_width = clamp_int((preview_width - 52) / 2, 1, 256);
    const int image_height = image_width * 3 / 4;
    const int image_x = preview_x + (preview_width - 2 * image_width - 14) / 2;
    const int image_y = preview_y + 54;
    const int touch_x = image_x + image_width + 14;
    overlay_blit_snapshot(image_x, image_y, image_width, image_height,
                          menu->snapshot_top, 256, 192);
    overlay_blit_snapshot(touch_x, image_y, image_width, image_height,
                          menu->snapshot_bottom, 256, 192);
    draw_centered(image_x, image_y + image_height + 6, image_width,
                  OVERLAY_TEXT_SMALL, COLOR_MUTED, "Top screen");
    draw_centered(touch_x, image_y + image_height + 6, image_width,
                  OVERLAY_TEXT_SMALL, COLOR_MUTED, "Touch screen");
  } else {
    draw_centered(preview_x + 14, preview_y + (PREVIEW_HEIGHT - 24) / 2,
                  preview_width - 28, OVERLAY_TEXT_SMALL, COLOR_MUTED,
                  "No preview available");
  }
  draw_status(menu, panel);
}

static void render_cheats(DrasticIngameMenu *menu) {
  enum { MAX_VISIBLE_CHEATS = 11, DETAILS_HEIGHT = 266, COLUMN_GAP = 16 };
  const bool portrait = ui_is_portrait();
  const int count = menu->cheat_count + 1;
  const int selected = clamp_int(menu->selection[MENU_CHEATS], 0, count - 1);
  const MenuCheat *cheat = selected ? &menu->cheats[selected - 1] : NULL;
  const int visible = count < MAX_VISIBLE_CHEATS ? count : MAX_VISIBLE_CHEATS;
  int scroll = menu->scroll[MENU_CHEATS];
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + visible) scroll = selected - visible + 1;
  scroll = clamp_int(scroll, 0, count - visible);
  menu->scroll[MENU_CHEATS] = scroll;

  const int list_height = visible * MENU_ROW_STEP;
  const int content_height = portrait ? list_height + COLUMN_GAP + DETAILS_HEIGHT :
      (list_height < DETAILS_HEIGHT ? DETAILS_HEIGHT : list_height);
  const char *actions = !cheat ? "A  Add code      B  Back" :
      cheat->custom ? "A  Toggle      X  Delete      B  Back" :
                      "A  Toggle      B  Back";
  const MenuPanel panel = draw_panel(1060, content_height, "Cheats", actions);
  const int x = panel.x + MENU_PADDING;
  const int y = panel.y + MENU_HEADER_HEIGHT;
  const int width = panel.width - 2 * MENU_PADDING;
  const int list_width = portrait ? width : 596;
  const int rows_width = list_width - (count > visible ? 10 : 0);
  for (int i = 0; i < visible; ++i) {
    const int index = scroll + i;
    const MenuRow row = index ? (MenuRow){
      .label = menu->cheats[index - 1].name,
      .type = ROW_TOGGLE,
      .on = menu->cheats[index - 1].enabled != 0,
    } : (MenuRow){.label = "Add custom Action Replay code", .type = ROW_LINK};
    draw_row(x, y + i * MENU_ROW_STEP, rows_width, index == selected, row);
  }
  if (count > visible) {
    const int track_height = list_height - 4;
    const int thumb = clamp_int(track_height * visible / count, 14, track_height);
    const int top = y + (track_height - thumb) * scroll / (count - visible);
    overlay_fill_round_rect(x + list_width - 4, y, 4, track_height, 2, COLOR_PANEL_ALT);
    overlay_fill_round_rect(x + list_width - 4, top, 4, thumb, 2, COLOR_FRAME);
  }

  const int detail_x = portrait ? x : x + list_width + COLUMN_GAP;
  const int detail_y = portrait ? y + list_height + COLUMN_GAP : y;
  const int detail_width = portrait ? width : width - list_width - COLUMN_GAP;
  const int detail_height = portrait ? DETAILS_HEIGHT : content_height - 4;
  overlay_fill_round_rect(detail_x, detail_y, detail_width, detail_height,
                          12, COLOR_PANEL_ALT);
  const int content_x = detail_x + 18;
  const int content_width = detail_width - 36;
  int content_y = detail_y + 17;
  if (!cheat) {
    overlay_draw_text(content_x, content_y, OVERLAY_TEXT_BODY, COLOR_TEXT,
                      "Action Replay");
    overlay_draw_wrapped(content_x, content_y + 40, content_width, 5,
                          OVERLAY_TEXT_SMALL, COLOR_MUTED,
        "Enter a name and hexadecimal address/value pairs using the on-screen keyboard.");
  } else {
    overlay_draw_wrapped(content_x, content_y, content_width, 2,
                          OVERLAY_TEXT_BODY, COLOR_TEXT, cheat->name);
    content_y += 63;
    overlay_draw_text(content_x, content_y, OVERLAY_TEXT_SMALL, COLOR_ACCENT,
                      cheat->custom ? "Custom cheat" : "Database cheat");
    content_y += 31;
    if (!cheat->custom && cheat->folder >= 0 &&
        cheat->folder < menu->folder_count && menu->folders[cheat->folder][0]) {
      overlay_draw_text_clipped(content_x, content_y, content_width,
                                OVERLAY_TEXT_SMALL, COLOR_MUTED,
                                menu->folders[cheat->folder]);
      content_y += 31;
    }
    if (cheat->note[0]) {
      const int room = detail_y + detail_height - 15 - content_y;
      overlay_draw_wrapped(content_x, content_y, content_width,
                            room / overlay_text_height(OVERLAY_TEXT_SMALL),
                            OVERLAY_TEXT_SMALL, COLOR_TEXT, cheat->note);
    }
  }
  draw_status(menu, panel);
}

static const char *layout_label(DrasticLayoutMode layout) {
  static const char *labels[] = {
    "Vertical", "Horizontal", "Top only", "Touch only", "Hybrid top",
    "Hybrid touch", "Custom"
  };
  return (unsigned)layout < sizeof(labels) / sizeof(*labels)
      ? labels[layout] : labels[0];
}

static const char *filter_label(DrasticVideoFilter filter) {
  static const char *labels[DRASTIC_FILTER_COUNT] = {
    "Nearest", "Linear", "Quilez", "Scanline", "Scale2x", "HQ2x", "FXAA",
    "FXAA HQ", "SMAA", "Custom"
  };
  return (unsigned)filter < DRASTIC_FILTER_COUNT ? labels[filter] : labels[0];
}

static const char *custom_shader_name(const DrasticIngameMenu *menu,
                                      const char *relative_path) {
  for (int index = 0; index < menu->custom_shader_count; index++)
    if (!strcmp(menu->custom_shaders[index].relative_path, relative_path))
      return menu->custom_shaders[index].name;
  const char *slash = relative_path ? strrchr(relative_path, '/') : NULL;
  return relative_path && relative_path[0] ? (slash ? slash + 1 : relative_path)
                                           : "Not selected";
}

static const char *filter_picker_label(const DrasticIngameMenu *menu) {
  if (!menu->filter_picker_custom)
    return menu->filter_picker_index >= 0 &&
           menu->filter_picker_index < DRASTIC_FILTER_CUSTOM
        ? filter_label((DrasticVideoFilter)menu->filter_picker_index)
        : "Nearest";
  return menu->filter_picker_index >= 0 &&
         menu->filter_picker_index < menu->custom_shader_count
      ? menu->custom_shaders[menu->filter_picker_index].name
      : "Not selected";
}

static void render_preview_picker(DrasticIngameMenu *menu, const char *kind,
                                  const char *label) {
  const char *cancel = "B  Cancel";
  const char *apply = "A  Apply";
  const bool show_status = menu->status[0] != '\0';
  const int width = ui_is_portrait() ? ui_width() - 40 : 804;
  const int height = show_status ? 89 : 54;
  const MenuPanel panel = {
    .x = (ui_width() - width) / 2,
    .y = ui_height() - height - 18,
    .width = width,
    .height = height,
  };
  draw_surface(panel);
  const int left = panel.x + 18;
  const int right = panel.x + width - 18;
  const int center_y = panel.y + 27;
  const int action_y = center_y - overlay_text_height(OVERLAY_TEXT_SMALL) / 2;
  overlay_draw_text(left, action_y, OVERLAY_TEXT_SMALL, COLOR_MUTED, cancel);
  overlay_draw_text_right(right, action_y, OVERLAY_TEXT_SMALL, COLOR_MUTED, apply);
  const int value_left = left + overlay_text_width(OVERLAY_TEXT_SMALL, cancel) + 36;
  const int value_right = right - overlay_text_width(OVERLAY_TEXT_SMALL, apply) - 36;
  draw_chevron(value_left - 9, center_y, true, COLOR_ACCENT);
  draw_chevron(value_right + 6, center_y, false, COLOR_ACCENT);

  char value[192];
  snprintf(value, sizeof(value), "%s  ·  %s", kind, label);
  const int available = value_right - value_left;
  const int value_y = center_y - overlay_text_height(OVERLAY_TEXT_BODY) / 2;
  if (overlay_text_width(OVERLAY_TEXT_BODY, value) > available)
    overlay_draw_text_scrolling(value_left, value_y, available,
                                OVERLAY_TEXT_BODY, COLOR_TEXT, value);
  else
    draw_centered(value_left, value_y, available, OVERLAY_TEXT_BODY,
                  COLOR_TEXT, value);
  if (show_status)
    draw_centered(left, panel.y + 54, right - left, OVERLAY_TEXT_SMALL,
                  COLOR_WARN, menu->status);
}

static void render_filter_picker(DrasticIngameMenu *menu) {
  render_preview_picker(menu,menu->filter_picker_custom ? "Custom shader" : "Filter",filter_picker_label(menu));
}

static const char *background_label(const char *path) {
  static const char *paths[]={"","builtin:white","builtin:pink","builtin:navy","builtin:black"};
  static const char *names[]={"None","PocketArch - White","PocketArch - Coral pink","PocketArch - Enamel navy","PocketArch - Black"};
  for (int i=0;i<5;i++) if (!strcmp(path,paths[i])) return names[i];
  const char *slash=strrchr(path,'/'); return slash ? slash+1 : path;
}

static const char *background_fit_label(int fit) {
  return fit==1 ? "Fill" : fit==2 ? "Stretch" : "Contain";
}

enum DisplayMenuItem {
  DISPLAY_LAYOUT,
  DISPLAY_SWAP,
  DISPLAY_ROTATION,
  DISPLAY_GAP,
  DISPLAY_INTEGER_SCALE,
  DISPLAY_FILTER,
  DISPLAY_SHADER,
  DISPLAY_EDITOR,
  DISPLAY_BACKGROUND,
  DISPLAY_BACKGROUND_FIT,
  DISPLAY_BACK,
  DISPLAY_ITEM_COUNT,
};

static void render_display(DrasticIngameMenu *menu) {
  const DrasticRuntimeConfig *config = menu->config;
  char rotation[24], gap[24];
  snprintf(rotation, sizeof(rotation), "%d degrees", (config->rotation & 3) * 90);
  snprintf(gap, sizeof(gap), "%d px", config->screen_gap);
  const MenuRow rows[DISPLAY_ITEM_COUNT] = {
    [DISPLAY_LAYOUT] = {.label = "Screen layout", .type = ROW_ADJUST,
                        .value = layout_label(config->layout)},
    [DISPLAY_SWAP] = {.label = "Swap DS screens", .type = ROW_TOGGLE,
                      .on = config->swap_screens},
    [DISPLAY_ROTATION] = {.label = "Rotation", .type = ROW_ADJUST,
                          .value = rotation},
    [DISPLAY_GAP] = {.label = "Screen gap", .type = ROW_ADJUST, .value = gap},
    [DISPLAY_INTEGER_SCALE] = {.label = "Integer scaling", .type = ROW_TOGGLE,
                               .on = config->integer_scale},
    [DISPLAY_FILTER] = {.label = "DraStic filter", .type = ROW_PREVIEW,
                        .value = filter_label(config->video_filter)},
    [DISPLAY_SHADER] = {.label = "Custom shader", .type = ROW_PREVIEW,
                        .value = custom_shader_name(menu, config->custom_shader)},
    [DISPLAY_EDITOR] = {.label = "Custom layout editor", .type = ROW_LINK},
    [DISPLAY_BACKGROUND] = {.label = "Background / bezel", .type = ROW_PREVIEW,
                            .value = background_label(config->background)},
    [DISPLAY_BACKGROUND_FIT] = {.label = "Background fit", .type = ROW_ADJUST,
                                .value = background_fit_label(config->background_fit)},
    [DISPLAY_BACK] = {.label = "Back"},
  };
  const MenuPanel panel = draw_panel(704, DISPLAY_ITEM_COUNT * MENU_ROW_STEP,
                                     "Screen layout & filters", NULL);
  draw_rows(panel, menu->selection[MENU_DISPLAY], rows, DISPLAY_ITEM_COUNT);
  draw_status(menu, panel);
}

enum EmulationMenuItem {
  EMULATION_FRAMESKIP,
  EMULATION_FRAMESKIP_TYPE,
  EMULATION_SAFE_SKIP,
  EMULATION_FAST_FORWARD,
  EMULATION_THREADED_3D,
  EMULATION_CHEATS,
  EMULATION_FPS,
  EMULATION_AUTOSAVE,
  EMULATION_BACK,
  EMULATION_ITEM_COUNT,
};

static void render_emulation(DrasticIngameMenu *menu) {
  static const char *methods[] = {"Automatic", "Fixed", "Aggressive", "Maximum"};
  static const char *ff_speeds[] = {"50%", "150%", "200%", "300%", "400%", "Unlimited"};
  const int method = clamp_int(prefs_get_int("Drastic/FrameskipType", 0), 0, 3);
  const int speed = clamp_int(prefs_get_int("Drastic/FastForwardSpeed", 2), 0, 5);
  char frameskip[24], autosave[24];
  snprintf(frameskip, sizeof(frameskip), "%d", prefs_get_int("Drastic/FrameskipValue", 0));
  if (menu->config->autosave_seconds)
    snprintf(autosave, sizeof(autosave), "%d min", menu->config->autosave_seconds / 60);
  else
    snprintf(autosave, sizeof(autosave), "Off");

  const MenuRow rows[EMULATION_ITEM_COUNT] = {
    [EMULATION_FRAMESKIP] = {.label = "Frames to skip", .type = ROW_ADJUST,
                             .value = frameskip},
    [EMULATION_FRAMESKIP_TYPE] = {.label = "Frame-skip method", .type = ROW_ADJUST,
                                  .value = methods[method]},
    [EMULATION_SAFE_SKIP] = {.label = "Safe frame skipping", .type = ROW_TOGGLE,
                             .on = prefs_get_bool("Drastic/FrameskipSafe", false)},
    [EMULATION_FAST_FORWARD] = {.label = "Fast-forward speed", .type = ROW_ADJUST,
                                 .value = ff_speeds[speed]},
    [EMULATION_THREADED_3D] = {.label = "Threaded 3D", .type = ROW_TOGGLE,
                               .on = prefs_get_bool("Drastic/Threaded3D", true)},
    [EMULATION_CHEATS] = {.label = "Enable cheats", .type = ROW_TOGGLE,
                          .on = prefs_get_bool("Drastic/CheatsEnabled", true)},
    [EMULATION_FPS] = {.label = "FPS counter", .type = ROW_TOGGLE,
                       .on = menu->config->show_fps},
    [EMULATION_AUTOSAVE] = {.label = "Autosave interval", .type = ROW_ADJUST,
                            .value = autosave},
    [EMULATION_BACK] = {.label = "Back"},
  };
  const MenuPanel panel = draw_panel(620, EMULATION_ITEM_COUNT * MENU_ROW_STEP,
                                     "Emulation options", NULL);
  draw_rows(panel, menu->selection[MENU_EMULATION], rows, EMULATION_ITEM_COUNT);
  draw_status(menu, panel);
}

enum AudioInputMenuItem {
  AUDIO_VOLUME,
  AUDIO_SOUND,
  AUDIO_MIC,
  AUDIO_MIC_SOURCE,
  AUDIO_MIC_LEVEL,
  AUDIO_VIBRATION,
  AUDIO_MOTION,
  AUDIO_STYLUS,
  AUDIO_MOUSE,
  AUDIO_BACK,
  AUDIO_ITEM_COUNT,
};

static const char *stylus_mode_label(DrasticStylusMode mode) {
  static const char *labels[] = {"Off", "Right stick", "Motion controls"};
  return (unsigned)mode < sizeof(labels) / sizeof(*labels)
      ? labels[mode] : labels[0];
}

static const char *microphone_source_label(const DrasticIngameMenu *menu) {
  if (menu->config->microphone_source != DRASTIC_MICROPHONE_EXTERNAL)
    return "Simulated noise";
  switch (menu->microphone_status) {
    case OPENSLES_MIC_STATUS_ACTIVE: return "External (active)";
    case OPENSLES_MIC_STATUS_CONNECTING: return "External (connecting)";
    case OPENSLES_MIC_STATUS_UNAVAILABLE: return "External (not detected)";
    default: return "External";
  }
}

static void render_audio_input(DrasticIngameMenu *menu) {
  const DrasticRuntimeConfig *config = menu->config;
  static const char *levels[] = {"Low", "Normal", "High", "Maximum"};
  char volume[24];
  snprintf(volume, sizeof(volume), "%d%%", config->volume);
  const MenuRow rows[AUDIO_ITEM_COUNT] = {
    [AUDIO_VOLUME] = {.label = "Volume", .type = ROW_ADJUST, .value = volume},
    [AUDIO_SOUND] = {.label = "Sound", .type = ROW_TOGGLE,
                     .on = prefs_get_bool("Drastic/SoundEnabled", true)},
    [AUDIO_MIC] = {.label = "Microphone", .type = ROW_TOGGLE,
                   .on = config->microphone_enabled},
    [AUDIO_MIC_SOURCE] = {.label = "Microphone source", .type = ROW_ADJUST,
                          .value = microphone_source_label(menu)},
    [AUDIO_MIC_LEVEL] = {.label = "Microphone level", .type = ROW_ADJUST,
                         .value = levels[clamp_int(prefs_get_int("Drastic/MicLevel", 1), 0, 3)]},
    [AUDIO_VIBRATION] = {.label = "Rumble Pak vibration", .type = ROW_TOGGLE,
                         .on = config->vibration},
    [AUDIO_MOTION] = {.label = "Gyro & accelerometer", .type = ROW_TOGGLE,
                      .on = config->motion},
    [AUDIO_STYLUS] = {.label = "Virtual stylus", .type = ROW_ADJUST,
                      .value = stylus_mode_label(config->stylus_mode)},
    [AUDIO_MOUSE] = {.label = "USB mouse stylus", .type = ROW_TOGGLE,
                     .on = config->mouse_stylus},
    [AUDIO_BACK] = {.label = "Back"},
  };
  const MenuPanel panel = draw_panel(668, AUDIO_ITEM_COUNT * MENU_ROW_STEP,
                                     "Audio, input & motion", NULL);
  draw_rows(panel, menu->selection[MENU_AUDIO_INPUT], rows, AUDIO_ITEM_COUNT);
  draw_status(menu, panel);
}

static void custom_rect_to_ui(const DrasticRuntimeConfig *config,
                              const DrasticScreenRect *rect,
                              DrasticScreenRect *ui) {
  *ui = *rect;
  switch (config->rotation & 3) {
    case 1:
      ui->x = rect->y;
      ui->y = 1.0f - rect->x - rect->width;
      ui->width = rect->height;
      ui->height = rect->width;
      break;
    case 2:
      ui->x = 1.0f - rect->x - rect->width;
      ui->y = 1.0f - rect->y - rect->height;
      break;
    case 3:
      ui->x = 1.0f - rect->y - rect->height;
      ui->y = rect->x;
      ui->width = rect->height;
      ui->height = rect->width;
      break;
    default:
      break;
  }
}

static void custom_rect_from_ui(const DrasticRuntimeConfig *config,
                                const DrasticScreenRect *ui,
                                DrasticScreenRect *rect) {
  const int screen = rect->screen;
  const int touch_target = rect->touch_target;
  switch (config->rotation & 3) {
    case 1:
      rect->x = 1.0f - ui->y - ui->height;
      rect->y = ui->x;
      rect->width = ui->height;
      rect->height = ui->width;
      break;
    case 2:
      rect->x = 1.0f - ui->x - ui->width;
      rect->y = 1.0f - ui->y - ui->height;
      rect->width = ui->width;
      rect->height = ui->height;
      break;
    case 3:
      rect->x = ui->y;
      rect->y = 1.0f - ui->x - ui->width;
      rect->width = ui->height;
      rect->height = ui->width;
      break;
    default:
      rect->x = ui->x;
      rect->y = ui->y;
      rect->width = ui->width;
      rect->height = ui->height;
      break;
  }
  rect->screen = screen;
  rect->touch_target = touch_target;
}

static void constrain_custom_rect(DrasticScreenRect *rect) {
  rect->width = clamp_float(rect->width, 0.08f, 1.0f);
  rect->height = clamp_float(rect->height, 0.08f, 1.0f);
  rect->x = clamp_float(rect->x, 0.0f, 1.0f - rect->width);
  rect->y = clamp_float(rect->y, 0.0f, 1.0f - rect->height);
}

/* driving_axis: 1 keeps width, 0 keeps height, -1 changes the closer side. */
static void set_custom_rect_aspect(DrasticScreenRect *rect,
                                   float physical_aspect,
                                   int canvas_width, int canvas_height,
                                   int driving_axis, int preserve_center) {
  if (!rect || canvas_width <= 0 || canvas_height <= 0) return;
  const float normalized_aspect =
      physical_aspect * (float)canvas_height / (float)canvas_width;
  if (normalized_aspect <= 0.0f) return;
  const float center_x = rect->x + rect->width * 0.5f;
  const float center_y = rect->y + rect->height * 0.5f;
  const float from_width = rect->width / normalized_aspect;
  if (driving_axis < 0) {
    const float width_cost = fabsf(from_width - rect->height) * canvas_height;
    const float height_cost =
        fabsf(rect->height * normalized_aspect - rect->width) * canvas_width;
    driving_axis = width_cost <= height_cost ? 1 : 0;
  }
  float height = driving_axis ? from_width : rect->height;
  const float minimum_height = fmaxf(0.08f, 0.08f / normalized_aspect);
  const float maximum_height = fminf(1.0f, 1.0f / normalized_aspect);
  height = clamp_float(height, minimum_height, maximum_height);
  rect->height = height;
  rect->width = height * normalized_aspect;
  if (preserve_center) {
    rect->x = center_x - rect->width * 0.5f;
    rect->y = center_y - rect->height * 0.5f;
  }
  rect->x = clamp_float(rect->x, 0.0f, 1.0f - rect->width);
  rect->y = clamp_float(rect->y, 0.0f, 1.0f - rect->height);
}

static float custom_native_aspect(const DrasticRuntimeConfig *config) {
  return (config->rotation & 1) ? 3.0f / 4.0f : 4.0f / 3.0f;
}

static void normalize_custom_layout(DrasticRuntimeConfig *config,
                                    int canvas_width, int canvas_height) {
  const float aspect = custom_native_aspect(config);
  for (int screen = 0; screen < 2; screen++)
    set_custom_rect_aspect(&config->custom_screens[screen], aspect,
                           canvas_width, canvas_height, -1, 1);
}

static void reset_custom_layout(DrasticRuntimeConfig *config) {
  const float aspect = custom_native_aspect(config);
  const float normalized_aspect =
      aspect * (float)panel_height / (float)panel_width;
  const float height = fminf(0.84f, 0.44f / normalized_aspect);
  const float width = height * normalized_aspect;
  config->custom_screens[0] = (DrasticScreenRect){
    .x = 0.25f - width * 0.5f, .y = 0.5f - height * 0.5f,
    .width = width, .height = height, .screen = 0,
  };
  config->custom_screens[1] = (DrasticScreenRect){
    .x = 0.75f - width * 0.5f, .y = 0.5f - height * 0.5f,
    .width = width, .height = height, .screen = 1, .touch_target = 1,
  };
}

static void custom_rect_for_ui(const DrasticRuntimeConfig *config,
                               const DrasticScreenRect *rect,
                               int *x, int *y, int *width, int *height) {
  DrasticScreenRect ui;
  custom_rect_to_ui(config, rect, &ui);
  *x = (int)(ui.x * overlay_width() + 0.5f);
  *y = (int)(ui.y * overlay_height() + 0.5f);
  *width = (int)(ui.width * overlay_width() + 0.5f);
  *height = (int)(ui.height * overlay_height() + 0.5f);
}

static void render_layout_editor(DrasticIngameMenu *menu) {
  const int canvas_width = ui_width();
  const int canvas_height = ui_height();
  overlay_fill_rect(0, 0, canvas_width, canvas_height, 0x33000000u);
  for (int screen = 0; screen < 2; screen++) {
    const DrasticScreenRect *rect = &menu->config->custom_screens[screen];
    int x, y, width, height;
    custom_rect_for_ui(menu->config, rect, &x, &y, &width, &height);
    const bool selected = screen == menu->editor_screen;
    const uint32_t color = selected ? COLOR_ACCENT : COLOR_TEXT;
    overlay_border_rect(x, y, width, height, selected ? 4 : 2, color);
    const char *name = screen ? "Touch screen" : "Top screen";
    const int badge_width = overlay_text_width(OVERLAY_TEXT_SMALL, name) + 20;
    const int badge_x = clamp_int(x + 8, 4, canvas_width - badge_width - 4);
    const int badge_y = clamp_int(y + 8, 4, canvas_height - 34);
    overlay_fill_round_rect(badge_x, badge_y, badge_width, 30, 8, COLOR_PANEL);
    overlay_draw_text(badge_x + 10, badge_y + 3, OVERLAY_TEXT_SMALL, color, name);
  }

  const int width = clamp_int(canvas_width - 38, 1, 660);
  const int x = (canvas_width - width) / 2;
  const MenuPanel header = {.x = x, .y = 16, .width = width, .height = 100};
  draw_surface(header);
  draw_centered(x + 18, header.y + 8, width - 36, OVERLAY_TEXT_TITLE,
                COLOR_TEXT, "Custom screen layout");
  const DrasticScreenRect *selected =
      &menu->config->custom_screens[menu->editor_screen];
  const float current_aspect = selected->height > 0.0f
      ? selected->width * panel_width / (selected->height * panel_height)
      : 0.0f;
  const float native_aspect = custom_native_aspect(menu->config);
  const int native = current_aspect > 0.0f &&
      fabsf(current_aspect / native_aspect - 1.0f) <= 0.005f;
  char aspect_text[48];
  if (native)
    snprintf(aspect_text, sizeof(aspect_text), "Native %s",
             (menu->config->rotation & 1) ? "3:4" : "4:3");
  else
    snprintf(aspect_text, sizeof(aspect_text), "Current %.2f:1",
             current_aspect);

  overlay_draw_text(x + 19, header.y + 61, OVERLAY_TEXT_SMALL,
                    native ? COLOR_GOOD : COLOR_WARN, aspect_text);
  const int right = x + width - 18;
  overlay_draw_text_right(right - 90, header.y + 61, OVERLAY_TEXT_SMALL,
                          COLOR_MUTED, "Aspect lock");
  draw_toggle(right, header.y + 73, menu->config->custom_aspect_lock);

  const MenuPanel controls = {.x = x, .y = canvas_height - 112,
                              .width = width, .height = 96};
  draw_surface(controls);
  draw_centered(x + 16, controls.y + 10, width - 32, OVERLAY_TEXT_SMALL, COLOR_TEXT,
                "Left stick / D-pad  Move     Right stick / ZL + D-pad  Resize");
  draw_centered(x + 16, controls.y + 36, width - 32, OVERLAY_TEXT_SMALL, COLOR_MUTED,
                "X  Switch screen     Y  Reset     R  Aspect lock");
  draw_centered(x + 16, controls.y + 62, width - 32, OVERLAY_TEXT_SMALL, COLOR_MUTED,
                "A  Save     B  Cancel");
}

static void render_menu(DrasticIngameMenu *menu) {
  if (!menu || !menu->open) return;
  overlay_begin();
  switch (menu->page) {
    case MENU_STATES: render_states(menu); break;
    case MENU_CHEATS: render_cheats(menu); break;
    case MENU_DISPLAY: render_display(menu); break;
    case MENU_FILTER_PICKER: render_filter_picker(menu); break;
    case MENU_BACKGROUND_PICKER:
      render_preview_picker(menu,"Background",menu->backgrounds[menu->background_index].name); break;
    case MENU_EMULATION: render_emulation(menu); break;
    case MENU_AUDIO_INPUT: render_audio_input(menu); break;
    case MENU_LAYOUT_EDITOR: render_layout_editor(menu); break;
    default: render_main(menu); break;
  }
  overlay_finish();
  menu->redraw = 0;
}

static void select_page(DrasticIngameMenu *menu, enum MenuPage page) {
  menu->page = page;
  menu->status[0] = '\0';
  menu->marquee_tick = 0;
  menu->redraw = 1;
  if (page == MENU_STATES) {
    menu->confirm_delete_slot = -1;
    menu->selection[page] = *menu->state_slot;
    refresh_snapshot(menu);
  } else if (page == MENU_CHEATS) {
    refresh_cheats(menu);
  }
}

static void navigate_list(DrasticIngameMenu *menu, int count, u64 pressed) {
  int *selection = &menu->selection[menu->page];
  if (pressed & HidNpadButton_Up)
    *selection = (*selection + count - 1) % count;
  if (pressed & HidNpadButton_Down)
    *selection = (*selection + 1) % count;
  if (pressed & HidNpadButton_L)
    *selection = clamp_int(*selection - 8, 0, count - 1);
  if (pressed & HidNpadButton_R)
    *selection = clamp_int(*selection + 8, 0, count - 1);
  if (pressed & (HidNpadButton_Up | HidNpadButton_Down |
                 HidNpadButton_L | HidNpadButton_R))
    menu->redraw = 1;
}

static void update_main(DrasticIngameMenu *menu, u64 pressed) {
  int *selection = &menu->selection[MENU_MAIN];
  int direction = 0;
  if (pressed & HidNpadButton_Up) direction = -1;
  if (pressed & HidNpadButton_Down) direction = 1;
  if (direction) {
    for (int count = 0; count < MAIN_ITEM_COUNT; count++) {
      *selection = (*selection + direction + MAIN_ITEM_COUNT) %
                   MAIN_ITEM_COUNT;
      if (main_item_available(*selection)) break;
    }
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_B) {
    drastic_menu_close(menu, true);
    return;
  }
  if (!(pressed & HidNpadButton_A)) return;
  switch (menu->selection[MENU_MAIN]) {
    case MAIN_RESUME: drastic_menu_close(menu, true); break;
    case MAIN_LID:
      drastic_menu_toggle_lid(menu);
      drastic_menu_close(menu, true);
      break;
    case MAIN_STATES: select_page(menu, MENU_STATES); break;
    case MAIN_CHEATS: select_page(menu, MENU_CHEATS); break;
    case MAIN_DISPLAY: select_page(menu, MENU_DISPLAY); break;
    case MAIN_EMULATION: select_page(menu, MENU_EMULATION); break;
    case MAIN_AUDIO_INPUT: select_page(menu, MENU_AUDIO_INPUT); break;
#ifdef USE_VULKAN
    case MAIN_LSFG: {
      if (!drastic_renderer_lsfg_available()) {
        set_status(menu, "Frame generation is disabled for this session");
        break;
      }
      const bool enabled = !drastic_renderer_lsfg_enabled();
      if (!drastic_renderer_lsfg_request_enabled(enabled)) {
        set_status(menu, "Frame generation could not be changed");
        break;
      }
      set_status(menu, "");
      break;
    }
#endif
    case MAIN_RESET:
      if (menu->core.reset_ds)
        menu->core.reset_ds(menu->core.env, menu->core.clazz);
      drastic_menu_close(menu, true);
      break;
    case MAIN_QUIT:
      menu->exit_requested = 1;
      /* Android leaves DraStic paused while its Activity is finishing.  Do
       * not resume the core for the small window between selecting Quit and
       * the native shutdown sequence. */
      drastic_menu_close(menu, false);
      break;
  }
}

static void update_states(DrasticIngameMenu *menu, u64 pressed) {
  const int old = menu->selection[MENU_STATES];
  navigate_list(menu, 10, pressed);
  if (old != menu->selection[MENU_STATES]) {
    menu->confirm_delete_slot = -1;
    *menu->state_slot = menu->selection[MENU_STATES];
    save_int("Wrapper/StateSlot", *menu->state_slot);
    refresh_snapshot(menu);
  }
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int slot = menu->selection[MENU_STATES];
  if ((pressed & HidNpadButton_A) && menu->core.load_state) {
    const int result = menu->core.load_state(menu->core.env, menu->core.clazz,
                                             slot);
    if (result) drastic_menu_close(menu, true);
    else set_status(menu, "This slot could not be loaded");
  }
  if ((pressed & HidNpadButton_X) && menu->core.save_state) {
    const int result = menu->core.save_state(menu->core.env, menu->core.clazz,
                                             slot, 1);
    menu->pending_snapshot = result != 0;
    set_status(menu, result ? "Save-state requested" :
                              "This slot could not be saved");
  }
  if (pressed & HidNpadButton_Y) {
    if (!menu->snapshot_valid) {
      set_status(menu, "There is no state to delete in this slot");
      menu->confirm_delete_slot = -1;
    } else if (menu->confirm_delete_slot != slot) {
      menu->confirm_delete_slot = slot;
      set_status(menu, "Press Y again to permanently delete this state");
    } else {
      const int deleted = delete_matching_state(menu, slot);
      menu->confirm_delete_slot = -1;
      refresh_snapshot(menu);
      set_status(menu, deleted ? "Save-state deleted" :
                                 "Could not safely identify the state file");
    }
  } else if (pressed & (HidNpadButton_A | HidNpadButton_B |
                        HidNpadButton_X)) {
    menu->confirm_delete_slot = -1;
  }
}

static void update_cheats(DrasticIngameMenu *menu, u64 pressed) {
  const int count = menu->cheat_count + 1;
  navigate_list(menu, count, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selected = menu->selection[MENU_CHEATS];
  if ((pressed & HidNpadButton_A) && !selected) {
    add_custom_cheat(menu);
    return;
  }
  if (selected <= 0 || selected > menu->cheat_count) return;
  MenuCheat *cheat = &menu->cheats[selected - 1];
  if (pressed & HidNpadButton_A) {
    cheat->enabled ^= 1;
    if (!cheat->custom && cheat->enabled && cheat->folder >= 0 &&
        cheat->folder < menu->folder_count &&
        !menu->folder_multi_select[cheat->folder]) {
      for (int index = 0; index < menu->cheat_count; index++) {
        MenuCheat *other = &menu->cheats[index];
        if (other == cheat || other->custom ||
            other->folder != cheat->folder || !other->enabled) continue;
        other->enabled = 0;
        if (menu->core.set_cheat_enabled)
          menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz,
                                       other->index, 0);
      }
    }
    if (cheat->custom && menu->core.set_custom_cheat_enabled)
      menu->core.set_custom_cheat_enabled(menu->core.env, menu->core.clazz,
                                          cheat->index, cheat->enabled);
    else if (!cheat->custom && menu->core.set_cheat_enabled)
      menu->core.set_cheat_enabled(menu->core.env, menu->core.clazz,
                                   cheat->index, cheat->enabled);
    if (menu->core.update_cheats)
      menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
    if (!cheat->custom) persist_database_cheats(menu);
    set_status(menu, "");
  }
  if ((pressed & HidNpadButton_X) && cheat->custom &&
      menu->core.remove_custom_cheat) {
    menu->core.remove_custom_cheat(menu->core.env, menu->core.clazz,
                                   cheat->index);
    if (menu->core.update_cheats)
      menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
    refresh_cheats(menu);
    set_status(menu, "Custom cheat deleted");
  }
}

static const char *layout_value(DrasticLayoutMode layout) {
  static const char *values[] = {
    "vertical", "horizontal", "top", "bottom", "hybrid_top",
    "hybrid_bottom", "custom"
  };
  return (unsigned)layout < sizeof(values) / sizeof(*values)
      ? values[layout] : values[0];
}

static int change_direction(u64 pressed) {
  if (pressed & HidNpadButton_Left) return -1;
  if (pressed & (HidNpadButton_Right | HidNpadButton_A)) return 1;
  return 0;
}

static void refresh_custom_shaders(DrasticIngameMenu *menu) {
  menu->custom_shader_count = (int)drastic_custom_shader_scan(
      menu->custom_shaders,
      sizeof(menu->custom_shaders) / sizeof(menu->custom_shaders[0]));
}

static int configured_custom_shader_index(const DrasticIngameMenu *menu) {
  for (int index = 0; index < menu->custom_shader_count; index++) {
    if (!strcmp(menu->custom_shaders[index].relative_path,
                menu->config->custom_shader))
      return index;
  }
  return -1;
}

static int select_builtin_filter_candidate(DrasticIngameMenu *menu,
                                           int candidate) {
  if (candidate < 0 || candidate >= DRASTIC_FILTER_CUSTOM) return 0;
  menu->config->video_filter = (DrasticVideoFilter)candidate;
  menu->filter_picker_index = candidate;
  menu->filter_picker_valid = 1;
  menu->status[0] = '\0';
  menu->redraw = 1;
  return 1;
}

static int select_custom_shader_candidate(DrasticIngameMenu *menu,
                                          int candidate) {
  if (candidate < 0 || candidate >= menu->custom_shader_count) return 0;
  menu->filter_picker_index = candidate;
  menu->filter_picker_valid = 0;
  char error[192];
  if (!drastic_renderer_set_custom_shader(
          menu->custom_shaders[candidate].relative_path,
          error, sizeof(error))) {
    set_status(menu, error);
    return 0;
  }
  menu->config->video_filter = DRASTIC_FILTER_CUSTOM;
  snprintf(menu->config->custom_shader, sizeof(menu->config->custom_shader),
           "%s", menu->custom_shaders[candidate].relative_path);
  menu->filter_picker_valid = 1;
  menu->status[0] = '\0';
  menu->redraw = 1;
  return 1;
}

static void preview_filter(DrasticIngameMenu *menu, int direction) {
  const int count = menu->filter_picker_custom
      ? menu->custom_shader_count : DRASTIC_FILTER_CUSTOM;
  if (count <= 0) return;
  const int candidate = (menu->filter_picker_index + direction + count) % count;
  if (menu->filter_picker_custom)
    select_custom_shader_candidate(menu, candidate);
  else
    select_builtin_filter_candidate(menu, candidate);
}

static int restore_filter_backup(DrasticIngameMenu *menu) {
  if (menu->filter_backup == DRASTIC_FILTER_CUSTOM) {
    char error[192];
    if (!drastic_renderer_set_custom_shader(
            menu->filter_backup_shader, error, sizeof(error))) {
      set_status(menu, error);
      return 0;
    }
  }
  menu->config->video_filter = menu->filter_backup;
  snprintf(menu->config->custom_shader, sizeof(menu->config->custom_shader),
           "%s", menu->filter_backup_shader);
  menu->redraw = 1;
  return 1;
}

static void begin_filter_picker(DrasticIngameMenu *menu, int custom,
                                int direction) {
  if (custom) {
    refresh_custom_shaders(menu);
    if (!menu->custom_shader_count) {
      set_status(menu, "No custom shaders were found");
      return;
    }
  }

  menu->filter_backup = menu->config->video_filter;
  snprintf(menu->filter_backup_shader, sizeof(menu->filter_backup_shader),
           "%s", menu->config->custom_shader);
  menu->filter_picker_custom = custom;
  menu->filter_picker_valid = 0;
  menu->status[0] = '\0';

  const int count = custom ? menu->custom_shader_count
                           : DRASTIC_FILTER_CUSTOM;
  int current = custom ? configured_custom_shader_index(menu)
                       : (menu->config->video_filter < DRASTIC_FILTER_CUSTOM
                           ? (int)menu->config->video_filter : -1);
  int candidate;
  if (current < 0)
    candidate = direction < 0 ? count - 1 : 0;
  else
    candidate = (current + direction + count) % count;

  select_page(menu, MENU_FILTER_PICKER);
  if (custom)
    select_custom_shader_candidate(menu, candidate);
  else
    select_builtin_filter_candidate(menu, candidate);
}

static void update_filter_picker(DrasticIngameMenu *menu, u64 pressed) {
  if (pressed & HidNpadButton_B) {
    if (!restore_filter_backup(menu)) return;
    menu->status[0] = '\0';
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_A) {
    if (!menu->filter_picker_valid) {
      set_status(menu, menu->filter_picker_custom
          ? "This custom shader could not be loaded"
          : "This filter could not be selected");
      return;
    }
    save_string("Wrapper/VideoFilter",
                drastic_config_filter_name(menu->config->video_filter));
    if (menu->filter_picker_custom)
      save_string("Wrapper/CustomShader", menu->config->custom_shader);
    menu->status[0] = '\0';
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_Left)
    preview_filter(menu, -1);
  else if (pressed & HidNpadButton_Right)
    preview_filter(menu, 1);
}

static const char *sd_path(const char *path) {
  return !strncmp(path,"sdmc:",5) ? path+5 : path;
}

static void save_custom_layout(DrasticIngameMenu *menu);
static void set_custom_layout_preferences(DrasticIngameMenu *menu);
static const char *const background_keys[]={
  "Wrapper/Background", "Wrapper/Rotation", "Wrapper/BackgroundFit",
  "Wrapper/CustomTopX", "Wrapper/CustomTopY", "Wrapper/CustomTopW", "Wrapper/CustomTopH",
  "Wrapper/CustomBottomX", "Wrapper/CustomBottomY", "Wrapper/CustomBottomW", "Wrapper/CustomBottomH",
  "Wrapper/CustomAspectLock", "Wrapper/Layout"
};

static void preview_background(DrasticIngameMenu *menu) {
  if (!menu->background_loader)
    menu->background_loader=drastic_background_loader_create(CACHE_DIR "/backgrounds");
  if (!menu->background_loader) {
    menu->background_valid=0; set_status(menu,"Could not prepare the background image."); return;
  }
  menu->background_valid=0;
  drastic_background_loader_request(menu->background_loader,&menu->background_backup,
      menu->backgrounds[menu->background_index].path);
  set_status(menu,"Preparing background");
}

static void poll_background(DrasticIngameMenu *menu) {
  if (!menu->background_loader) return;
  DrasticRuntimeConfig candidate;
  const DrasticBackgroundImage *image=NULL;
  char error[192]={0};
  const int status=drastic_background_loader_poll(menu->background_loader,&candidate,&image,error,sizeof(error));
  if (!status) return;
  menu->background_valid=status>0 && drastic_background_set_cached_image(&candidate,image);
  drastic_background_image_release(image);
  if (menu->background_valid) {
    *menu->config=candidate;
    overlay_set_rotation(menu->config->rotation);
    drastic_config_calculate_layout(menu->config,panel_width,panel_height);
  }
  set_status(menu,menu->background_valid ? "" : *error?error:"Could not prepare the background image.");
}

static void begin_background_picker(DrasticIngameMenu *menu,int direction) {
  free(menu->backgrounds);
  menu->backgrounds=calloc(513,sizeof(*menu->backgrounds));
  if (!menu->backgrounds) { set_status(menu,"Could not prepare the background image."); return; }
  static const char *styles[]={"","builtin:white","builtin:pink","builtin:navy","builtin:black"};
  for (int i=0;i<5;i++) {
    snprintf(menu->backgrounds[i].path,sizeof(menu->backgrounds[i].path),"%s",styles[i]);
    snprintf(menu->backgrounds[i].name,sizeof(menu->backgrounds[i].name),"%s",background_label(styles[i]));
  }
  menu->background_count=5+(int)drastic_background_scan(DATA_ROOT "/backgrounds",menu->backgrounds+5,507);
  menu->background_index=-1;
  for (int i=0;i<menu->background_count;i++)
    if (!strcmp(sd_path(menu->backgrounds[i].path),sd_path(menu->config->background))) menu->background_index=i;
  if (menu->background_index<0) {
    const int i=menu->background_count++;
    snprintf(menu->backgrounds[i].path,sizeof(menu->backgrounds[i].path),"%s",menu->config->background);
    snprintf(menu->backgrounds[i].name,sizeof(menu->backgrounds[i].name),"%s",background_label(menu->config->background));
    menu->background_index=i;
  }
  menu->background_backup=*menu->config;
  drastic_background_image_release(menu->background_backup_image);
  menu->background_backup_image=drastic_background_acquire_current_image();
  menu->background_valid=1;
  select_page(menu,MENU_BACKGROUND_PICKER);
  if (direction) {
    menu->background_index=(menu->background_index+direction+menu->background_count)%menu->background_count;
    preview_background(menu);
  }
}

static int restore_background(DrasticIngameMenu *menu) {
  DrasticRuntimeConfig backup=menu->background_backup;
  drastic_background_loader_cancel(menu->background_loader);
  if (!drastic_background_set_cached_image(&backup,menu->background_backup_image)) {
    set_status(menu,"Could not restore the previous background."); return 0;
  }
  *menu->config=backup;
  overlay_set_rotation(menu->config->rotation);
  drastic_config_calculate_layout(menu->config,panel_width,panel_height);
  return 1;
}

static void finish_background_picker(DrasticIngameMenu *menu) {
  drastic_background_loader_cancel(menu->background_loader);
  drastic_background_image_release(menu->background_backup_image); menu->background_backup_image=NULL;
  free(menu->backgrounds); menu->backgrounds=NULL; menu->background_count=0;
  select_page(menu,MENU_DISPLAY);
}

static void update_background_picker(DrasticIngameMenu *menu,u64 pressed) {
  poll_background(menu);
  if (pressed & HidNpadButton_B) {
    if (restore_background(menu)) finish_background_picker(menu);
  } else if (pressed & HidNpadButton_A) {
    if (!menu->background_valid) { set_status(menu,"This background could not be loaded"); return; }
    prefs_set_string("Wrapper/BackgroundCache",menu->config->background_cache);
    prefs_set_string("Wrapper/Background",menu->config->background);
    DrasticRuntimeConfig preset={0};
    if (drastic_background_apply_layout(&preset,menu->config->background)) {
      set_custom_layout_preferences(menu);
      prefs_set_int("Wrapper/Rotation",menu->config->rotation);
      prefs_set_string("Wrapper/BackgroundFit",menu->config->background_fit==2?"stretch":
                  menu->config->background_fit==1?"fill":"contain");
      prefs_save_runtime_keys(background_keys,sizeof(background_keys)/sizeof(*background_keys));
    } else prefs_save_runtime_key("Wrapper/Background");
    finish_background_picker(menu);
  } else if (pressed & (HidNpadButton_Left|HidNpadButton_Right)) {
    const int direction=pressed&HidNpadButton_Left ? -1 : 1;
    menu->background_index=(menu->background_index+direction+menu->background_count)%menu->background_count;
    preview_background(menu);
  }
}

static void update_display(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, DISPLAY_ITEM_COUNT, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_DISPLAY];
  if (selection == DISPLAY_BACK && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
    return;
  }
  if (selection == DISPLAY_BACKGROUND && (pressed & (HidNpadButton_A|HidNpadButton_Left|HidNpadButton_Right))) {
    begin_background_picker(menu,pressed&HidNpadButton_Left ? -1 : pressed&HidNpadButton_Right ? 1 : 0);
    return;
  }
  if (selection == DISPLAY_EDITOR && (pressed & HidNpadButton_A)) {
    menu->editor_old_layout = menu->config->layout;
    menu->editor_old_aspect_lock = menu->config->custom_aspect_lock;
    memcpy(menu->editor_backup, menu->config->custom_screens,
           sizeof(menu->editor_backup));
    menu->editor_screen = 0;
    menu->config->layout = DRASTIC_LAYOUT_CUSTOM;
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_LAYOUT_EDITOR);
    return;
  }
  if ((selection == DISPLAY_FILTER || selection == DISPLAY_SHADER) &&
      (pressed & (HidNpadButton_Left | HidNpadButton_Right |
                  HidNpadButton_A))) {
    const int direction = (pressed & HidNpadButton_Left) ? -1 :
                          (pressed & HidNpadButton_Right) ? 1 : 0;
    begin_filter_picker(menu, selection == DISPLAY_SHADER, direction);
    return;
  }
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case DISPLAY_LAYOUT:
      menu->config->layout = (DrasticLayoutMode)(
          ((int)menu->config->layout + direction + 7) % 7);
      save_string("Wrapper/Layout", layout_value(menu->config->layout));
      break;
    case DISPLAY_SWAP:
      menu->config->swap_screens ^= 1;
      save_bool("Wrapper/SwapScreens", menu->config->swap_screens);
      break;
    case DISPLAY_ROTATION:
      menu->config->rotation = (menu->config->rotation + direction + 4) % 4;
      save_int("Wrapper/Rotation", menu->config->rotation);
      overlay_set_rotation(menu->config->rotation);
      break;
    case DISPLAY_GAP:
      menu->config->screen_gap = clamp_int(
          menu->config->screen_gap + direction * 2, 0, 128);
      save_int("Wrapper/ScreenGap", menu->config->screen_gap);
      break;
    case DISPLAY_INTEGER_SCALE:
      menu->config->integer_scale ^= 1;
      save_bool("Wrapper/IntegerScale", menu->config->integer_scale);
      break;
    case DISPLAY_BACKGROUND_FIT: {
      menu->config->background_fit=(menu->config->background_fit+direction+3)%3;
      static const char *fits[]={"contain","fill","stretch"};
      save_string("Wrapper/BackgroundFit",fits[menu->config->background_fit]);
      break;
    }
    default: return;
  }
  drastic_config_calculate_layout(menu->config, panel_width, panel_height);
  menu->redraw = 1;
}

static void update_emulation(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, EMULATION_ITEM_COUNT, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_EMULATION];
  if (selection == EMULATION_BACK && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case EMULATION_FRAMESKIP:
      save_int("Drastic/FrameskipValue", clamp_int(
          prefs_get_int("Drastic/FrameskipValue", 0) + direction, 0, 9));
      break;
    case EMULATION_FRAMESKIP_TYPE:
      save_int("Drastic/FrameskipType",
               (prefs_get_int("Drastic/FrameskipType", 0) + direction + 4) % 4);
      break;
    case EMULATION_SAFE_SKIP: save_bool("Drastic/FrameskipSafe",
                      !prefs_get_bool("Drastic/FrameskipSafe", false)); break;
    case EMULATION_FAST_FORWARD: {
      static const int speeds[] = {0, 1, 2, 3, 4, 5};
      const int current = prefs_get_int("Drastic/FastForwardSpeed", 2);
      int index = 0;
      for (int i = 0; i < 6; i++) if (speeds[i] == current) index = i;
      index = (index + direction + 6) % 6;
      save_int("Drastic/FastForwardSpeed", speeds[index]);
      break;
    }
    case EMULATION_THREADED_3D: save_bool("Drastic/Threaded3D",
                      !prefs_get_bool("Drastic/Threaded3D", true)); break;
    case EMULATION_CHEATS:
      save_bool("Drastic/CheatsEnabled",
                !prefs_get_bool("Drastic/CheatsEnabled", true));
      if (menu->core.update_cheats)
        menu->core.update_cheats(menu->core.env, menu->core.clazz, 1);
      break;
    case EMULATION_FPS:
      menu->config->show_fps ^= 1;
      save_bool("Drastic/ShowFPS", menu->config->show_fps);
      break;
    case EMULATION_AUTOSAVE: {
      static const int intervals[] = {0, 60, 300, 600, 1800};
      int index = 0;
      for (int i = 0; i < 5; i++)
        if (intervals[i] == menu->config->autosave_seconds) index = i;
      index = (index + direction + 5) % 5;
      menu->config->autosave_seconds = intervals[index];
      save_int("Drastic/AutosaveInterval", menu->config->autosave_seconds);
      if (menu->core.set_autosave_interval)
        menu->core.set_autosave_interval(menu->core.env, menu->core.clazz,
                                         menu->config->autosave_seconds);
      break;
    }
    default: return;
  }
  apply_core_config(menu);
  menu->redraw = 1;
}

static void update_audio_input(DrasticIngameMenu *menu, u64 pressed) {
  navigate_list(menu, AUDIO_ITEM_COUNT, pressed);
  if (pressed & HidNpadButton_B) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int selection = menu->selection[MENU_AUDIO_INPUT];
  if (selection == AUDIO_BACK && (pressed & HidNpadButton_A)) {
    select_page(menu, MENU_MAIN);
    return;
  }
  const int direction = change_direction(pressed);
  if (!direction) return;
  switch (selection) {
    case AUDIO_VOLUME:
      menu->config->volume = clamp_int(menu->config->volume + direction * 5,
                                       0, 100);
      save_int("Wrapper/Volume", menu->config->volume);
      if (menu->core.set_audio_volume)
        menu->core.set_audio_volume(menu->core.env, menu->core.clazz,
                                    menu->config->volume);
      opensles_set_master_volume((unsigned)menu->config->volume);
      break;
    case AUDIO_SOUND: save_bool("Drastic/SoundEnabled",
                      !prefs_get_bool("Drastic/SoundEnabled", true)); break;
    case AUDIO_MIC:
      menu->config->microphone_enabled ^= 1;
      save_bool("Drastic/MicEnabled", menu->config->microphone_enabled);
      opensles_set_microphone_enabled(menu->config->microphone_enabled != 0);
      break;
    case AUDIO_MIC_SOURCE:
      menu->config->microphone_source =
          menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL
              ? DRASTIC_MICROPHONE_SIMULATED
              : DRASTIC_MICROPHONE_EXTERNAL;
      save_string("Wrapper/MicrophoneSource",
                  menu->config->microphone_source ==
                          DRASTIC_MICROPHONE_EXTERNAL
                      ? "external" : "noise");
      opensles_set_microphone_source(
          menu->config->microphone_source == DRASTIC_MICROPHONE_EXTERNAL
              ? OPENSLES_MIC_SOURCE_EXTERNAL
              : OPENSLES_MIC_SOURCE_SIMULATED);
      set_status(menu,
                 menu->config->microphone_source ==
                         DRASTIC_MICROPHONE_EXTERNAL
                     ? "External input selected; connect a headset or USB microphone"
                     : "Simulated-noise microphone selected");
      break;
    case AUDIO_MIC_LEVEL:
      save_int("Drastic/MicLevel",
               (prefs_get_int("Drastic/MicLevel", 1) + direction + 4) % 4);
      break;
    case AUDIO_VIBRATION:
      menu->config->vibration ^= 1;
      save_bool("Wrapper/Vibration", menu->config->vibration);
      break;
    case AUDIO_MOTION:
      menu->config->motion ^= 1;
      save_bool("Wrapper/Motion", menu->config->motion);
      break;
    case AUDIO_STYLUS:
      menu->config->stylus_mode = (DrasticStylusMode)(
          ((int)menu->config->stylus_mode + direction + 3) % 3);
      save_string("Wrapper/StylusMode",
                  menu->config->stylus_mode == DRASTIC_STYLUS_OFF ? "off" :
                  menu->config->stylus_mode == DRASTIC_STYLUS_MOTION
                      ? "motion" : "stick");
      if (menu->config->stylus_mode == DRASTIC_STYLUS_MOTION)
        set_status(menu, "Motion stylus will center on the current controller angle");
      break;
    case AUDIO_MOUSE:
      menu->config->mouse_stylus ^= 1;
      save_bool("Wrapper/MouseStylus", menu->config->mouse_stylus);
      break;
    default: return;
  }
  if (selection == AUDIO_SOUND || selection == AUDIO_MIC || selection == AUDIO_MIC_LEVEL)
    apply_core_config(menu);
  menu->redraw = 1;
}

static void set_custom_layout_preferences(DrasticIngameMenu *menu) {
  static const char *keys[2][4] = {
    {"Wrapper/CustomTopX", "Wrapper/CustomTopY",
     "Wrapper/CustomTopW", "Wrapper/CustomTopH"},
    {"Wrapper/CustomBottomX", "Wrapper/CustomBottomY",
     "Wrapper/CustomBottomW", "Wrapper/CustomBottomH"},
  };
  for (int screen = 0; screen < 2; screen++) {
    const DrasticScreenRect *rect = &menu->config->custom_screens[screen];
    prefs_set_float(keys[screen][0], rect->x);
    prefs_set_float(keys[screen][1], rect->y);
    prefs_set_float(keys[screen][2], rect->width);
    prefs_set_float(keys[screen][3], rect->height);
  }
  prefs_set_bool("Wrapper/CustomAspectLock",
            menu->config->custom_aspect_lock);
  prefs_set_string("Wrapper/Layout", "custom");
}

static void save_custom_layout(DrasticIngameMenu *menu) {
  set_custom_layout_preferences(menu);
  prefs_save_runtime_keys(background_keys+3,sizeof(background_keys)/sizeof(*background_keys)-3);
}

static float normalized_axis(int value) {
  if (abs(value) < 6000) return 0.0f;
  return clamp_float((float)value / 32767.0f, -1.0f, 1.0f);
}

static void update_layout_editor(DrasticIngameMenu *menu, u64 held,
                                 u64 pressed, HidAnalogStickState left,
                                 HidAnalogStickState right) {
  if (pressed & HidNpadButton_B) {
    menu->config->layout = menu->editor_old_layout;
    menu->config->custom_aspect_lock = menu->editor_old_aspect_lock;
    memcpy(menu->config->custom_screens, menu->editor_backup,
           sizeof(menu->editor_backup));
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_DISPLAY);
    return;
  }
  if (pressed & HidNpadButton_A) {
    menu->config->layout = DRASTIC_LAYOUT_CUSTOM;
    save_custom_layout(menu);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    select_page(menu, MENU_DISPLAY);
    set_status(menu, "Custom layout saved for this title");
    return;
  }
  if (pressed & HidNpadButton_X) {
    menu->editor_screen ^= 1;
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_Y) {
    reset_custom_layout(menu->config);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
  }
  if (pressed & HidNpadButton_R) {
    menu->config->custom_aspect_lock ^= 1;
    if (menu->config->custom_aspect_lock)
      normalize_custom_layout(menu->config, panel_width, panel_height);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
  }
  DrasticScreenRect *rect = &menu->config->custom_screens[menu->editor_screen];
  float move_x = normalized_axis(left.x) * 0.004f;
  float move_y = -normalized_axis(left.y) * 0.004f;
  float size_x = normalized_axis(right.x) * 0.004f;
  float size_y = -normalized_axis(right.y) * 0.004f;
  const float step = (held & HidNpadButton_ZR) ? 0.02f : 0.006f;
  if (held & HidNpadButton_ZL) {
    if (pressed & HidNpadButton_Left) size_x -= step;
    if (pressed & HidNpadButton_Right) size_x += step;
    if (pressed & HidNpadButton_Up) size_y -= step;
    if (pressed & HidNpadButton_Down) size_y += step;
  } else {
    if (pressed & HidNpadButton_Left) move_x -= step;
    if (pressed & HidNpadButton_Right) move_x += step;
    if (pressed & HidNpadButton_Up) move_y -= step;
    if (pressed & HidNpadButton_Down) move_y += step;
  }
  if (move_x || move_y || size_x || size_y) {
    /* Edit the rectangle in the same rotated coordinate space shown by the
     * overlay.  Otherwise at 90 degrees a rightward resize changes height,
     * and at 270 degrees the axes appear inverted. */
    DrasticScreenRect ui;
    custom_rect_to_ui(menu->config, rect, &ui);
    ui.x += move_x;
    ui.y += move_y;
    ui.width += size_x;
    ui.height += size_y;
    if (menu->config->custom_aspect_lock && (size_x || size_y)) {
      const int width_drives =
          fabsf(size_x * overlay_width()) >=
          fabsf(size_y * overlay_height());
      set_custom_rect_aspect(&ui, 4.0f / 3.0f,
                             overlay_width(), overlay_height(),
                             width_drives, 0);
    } else {
      constrain_custom_rect(&ui);
    }
    custom_rect_from_ui(menu->config, &ui, rect);
    constrain_custom_rect(rect);
    drastic_config_calculate_layout(menu->config, panel_width, panel_height);
    menu->redraw = 1;
  }
}

DrasticIngameMenu *drastic_menu_create(DrasticRuntimeConfig *config,
                                       const DrasticMenuCore *core,
                                       int *state_slot) {
  if (!config || !core || !core->set_hinge_status || !state_slot) return NULL;
  DrasticIngameMenu *menu = calloc(1, sizeof(*menu));
  if (!menu) return NULL;
  menu->config = config;
  menu->core = *core;
  menu->state_slot = state_slot;
  menu->confirm_delete_slot = -1;
  menu->snapshot_top_array = jni_make_int_array(256 * 192);
  menu->snapshot_bottom_array = jni_make_int_array(256 * 192);
  menu->snapshot_top = jni_int_array_data(menu->snapshot_top_array);
  menu->snapshot_bottom = jni_int_array_data(menu->snapshot_bottom_array);
  menu->microphone_status = opensles_get_microphone_status();
  refresh_custom_shaders(menu);
  return menu;
}

void drastic_menu_destroy(DrasticIngameMenu *menu) {
  if (!menu) return;
  free_cheats(menu);
  free(menu->backgrounds);
  drastic_background_loader_destroy(menu->background_loader);
  drastic_background_image_release(menu->background_backup_image);
  jni_release_int_array(menu->snapshot_top_array);
  jni_release_int_array(menu->snapshot_bottom_array);
  free(menu);
}

void drastic_menu_open(DrasticIngameMenu *menu) {
  if (!menu || menu->open) return;
  menu->open = 1;
  menu->page = MENU_MAIN;
  menu->status[0] = '\0';
  reset_analog_navigation(menu);
  if (menu->core.pause_system)
    menu->core.pause_system(menu->core.env, menu->core.clazz, 1);
  menu->redraw = 1;
  render_menu(menu);
}

void drastic_menu_close(DrasticIngameMenu *menu, bool resume_core) {
  if (!menu || !menu->open) return;
  if (menu->page==MENU_BACKGROUND_PICKER) {
    if (!restore_background(menu)) return;
    finish_background_picker(menu);
  }
  menu->open = 0;
  overlay_hide();
  if (resume_core && menu->core.pause_system)
    menu->core.pause_system(menu->core.env, menu->core.clazz, 0);
}

bool drastic_menu_is_open(const DrasticIngameMenu *menu) {
  return menu && menu->open;
}

void drastic_menu_toggle_lid(DrasticIngameMenu *menu) {
  if (!menu) return;
  menu->lid_closed = !menu->lid_closed;
  menu->core.set_hinge_status(menu->core.env, menu->core.clazz, menu->lid_closed);
}

static void update_marquee(DrasticIngameMenu *menu) {
  const int selection = menu->selection[MENU_DISPLAY];
  const bool active = (menu->page == MENU_DISPLAY &&
      (selection == DISPLAY_SHADER || selection == DISPLAY_BACKGROUND)) ||
      menu->page == MENU_BACKGROUND_PICKER ||
      (menu->page == MENU_FILTER_PICKER && menu->filter_picker_custom);
  if (!active) {
    menu->marquee_tick = 0;
    return;
  }
  const u64 frequency = armGetSystemTickFreq();
  const u64 now = armGetSystemTick();
  const u64 interval = frequency / 30u;
  if (!menu->marquee_tick || now - menu->marquee_tick >= interval) {
    menu->marquee_tick = now;
    menu->redraw = 1;
  }
}

void drastic_menu_update(DrasticIngameMenu *menu, u64 held, u64 pressed,
                         HidAnalogStickState left,
                         HidAnalogStickState right) {
  if (!menu || !menu->open) return;
  if (menu->pending_snapshot && (!menu->core.is_saving ||
      !menu->core.is_saving(menu->core.env, menu->core.clazz))) {
    menu->pending_snapshot = 0;
    refresh_snapshot(menu);
    set_status(menu, "Save-state complete");
  }
  if (menu->page == MENU_AUDIO_INPUT) {
    const OpenSLESMicrophoneStatus microphone_status =
        opensles_get_microphone_status();
    if (microphone_status != menu->microphone_status) {
      menu->microphone_status = microphone_status;
      menu->redraw = 1;
    }
  }
  if (menu->page == MENU_LAYOUT_EDITOR)
    reset_analog_navigation(menu);
  else
    pressed |= analog_navigation_pressed(menu, left);
  switch (menu->page) {
    case MENU_STATES: update_states(menu, pressed); break;
    case MENU_CHEATS: update_cheats(menu, pressed); break;
    case MENU_DISPLAY: update_display(menu, pressed); break;
    case MENU_FILTER_PICKER: update_filter_picker(menu, pressed); break;
    case MENU_BACKGROUND_PICKER: update_background_picker(menu, pressed); break;
    case MENU_EMULATION: update_emulation(menu, pressed); break;
    case MENU_AUDIO_INPUT: update_audio_input(menu, pressed); break;
    case MENU_LAYOUT_EDITOR:
      update_layout_editor(menu, held, pressed, left, right); break;
    default: update_main(menu, pressed); break;
  }
  update_marquee(menu);
  if (menu->open && menu->redraw) render_menu(menu);
}

bool drastic_menu_take_exit_request(DrasticIngameMenu *menu) {
  if (!menu || !menu->exit_requested) return false;
  menu->exit_requested = 0;
  return true;
}
