/* Flat, allocation-free preference store shared by the launcher and host. */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "prefs.h"

#define PREFS_MAX_ENTRIES 512
#define PREFS_KEY_LEN 128
#define PREFS_VAL_LEN 1056

typedef struct {
  char key[PREFS_KEY_LEN];
  char value[PREFS_VAL_LEN];
} PrefEntry;

static PrefEntry entries[PREFS_MAX_ENTRIES];
static int entry_count;
static char ini_path[1024];
static char pending_rom[PREFS_VAL_LEN];

static int find_entry(const char *key) {
  if (!key) return -1;
  for (int index = 0; index < entry_count; index++)
    if (!strcmp(entries[index].key, key)) return index;
  return -1;
}

static void put_entry(const char *key, const char *value) {
  if (!key || !*key || !value) return;
  int index = find_entry(key);
  if (index < 0) {
    if (entry_count >= PREFS_MAX_ENTRIES) return;
    index = entry_count++;
    snprintf(entries[index].key, sizeof(entries[index].key), "%s", key);
  }
  snprintf(entries[index].value, sizeof(entries[index].value), "%s", value);
}

static void seed(const char *key, const char *value) {
  if (find_entry(key) < 0) put_entry(key, value);
}

static char *trim(char *text) {
  while (*text && isspace((unsigned char)*text)) text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) --end;
  *end = 0;
  return text;
}

static void parse_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return;
  char line[2048];
  while (fgets(line, sizeof(line), file)) {
    char *key = trim(line);
    if (!*key || *key == '#' || *key == ';' || *key == '[') continue;
    char *equals = strchr(key, '=');
    if (!equals) continue;
    *equals++ = 0;
    key = trim(key);
    char *value = trim(equals);
    if (*key) put_entry(key, value);
  }
  fclose(file);
}

static void seed_critical_defaults(void) {
  seed("Wrapper/CoreSo", DATA_ROOT "/cores/" SO_NAME);
  seed("Drastic/RomPath", pending_rom[0] ? pending_rom : DEFAULT_ROM_PATH);
}

static void seed_defaults(void) {
  seed_critical_defaults();
  seed("Wrapper/Renderer", "deko3d");
  seed("Wrapper/Layout", "horizontal");
  seed("Wrapper/SwapScreens", "false");
  seed("Wrapper/Rotation", "0");
  seed("Wrapper/ScreenGap", "8");
  seed("Wrapper/IntegerScale", "false");
  seed("Wrapper/LowLatency", "true");
  seed("Wrapper/TextureFilter", "nearest");
  seed("Wrapper/VideoFilter", "nearest");
  seed("Wrapper/CustomShader", "");
  seed("Wrapper/LSFGEnabled", "false");
  seed("Wrapper/LSFGFlowScale", "0.25");
  seed("Wrapper/LSFGPerformance", "true");
  seed("Wrapper/Volume", "100");
  seed("Wrapper/MicrophoneSource", "noise");
  seed("Wrapper/Vibration", "true");
  seed("Wrapper/Motion", "true");
  seed("Wrapper/AnalogDpad", "true");
  seed("Wrapper/AnalogDeadzone", "35");
  seed("Wrapper/StylusMode", "stick");
  seed("Wrapper/MouseStylus", "true");
  seed("Wrapper/AnalogTouchButton", "StickR");
  seed("Wrapper/AnalogStylusSpeed", "8");
  seed("Wrapper/MotionStylusSensitivity", "10");
  seed("Wrapper/HotkeyMotionStylusRecenter", "L+R+StickR");
  seed("Wrapper/HotkeyFastForward", "ZR");
  seed("Wrapper/HotkeyMenu", "L+R+Plus");
  seed("Wrapper/HotkeySwapScreens", "ZL");
  seed("Wrapper/HotkeyMicrophone", "StickL");
  seed("Wrapper/HotkeySaveState", "L+R+Minus+Y");
  seed("Wrapper/HotkeyLoadState", "L+R+Minus+X");
  seed("Wrapper/HotkeyNextSlot", "L+R+Minus+Up");
  seed("Wrapper/HotkeyPreviousSlot", "L+R+Minus+Down");
  seed("Wrapper/HotkeyReset", "L+R+Minus+A");
  seed("Wrapper/HotkeyQuit", "None");
  seed("Wrapper/StateSlot", "0");
  seed("Wrapper/CustomTopX", "0.03");
  seed("Wrapper/CustomTopY", "0.20666667");
  seed("Wrapper/CustomTopW", "0.44");
  seed("Wrapper/CustomTopH", "0.58666667");
  seed("Wrapper/CustomBottomX", "0.53");
  seed("Wrapper/CustomBottomY", "0.20666667");
  seed("Wrapper/CustomBottomW", "0.44");
  seed("Wrapper/CustomBottomH", "0.58666667");
  seed("Wrapper/CustomAspectLock", "true");

  seed("Wrapper/Pad/A", "A");
  seed("Wrapper/Pad/B", "B");
  seed("Wrapper/Pad/X", "X");
  seed("Wrapper/Pad/Y", "Y");
  seed("Wrapper/Pad/L", "L");
  seed("Wrapper/Pad/R", "R");
  seed("Wrapper/Pad/Start", "Plus");
  seed("Wrapper/Pad/Select", "Minus");
  seed("Wrapper/Pad/Up", "Up");
  seed("Wrapper/Pad/Down", "Down");
  seed("Wrapper/Pad/Left", "Left");
  seed("Wrapper/Pad/Right", "Right");

  seed("Drastic/FrameskipValue", "0");
  seed("Drastic/FrameskipType", "0");
  seed("Drastic/FrameskipSafe", "false");
  seed("Drastic/AudioLatency", "2");
  seed("Drastic/FastForwardSpeed", "2");
  seed("Drastic/CpuThreads", "3");
  seed("Drastic/AutoFireSpeed", "2");
  seed("Drastic/MicLevel", "1");
  seed("Drastic/Slot2Type", "1");
  seed("Drastic/SoundEnabled", "true");
  seed("Drastic/ShowFPS", "false");
  seed("Drastic/Threaded3D", "true");
  seed("Drastic/CheatsEnabled", "true");
  seed("Drastic/MicEnabled", "true");
  seed("Drastic/BackupInSavestates", "true");
  seed("Drastic/IgnoreGamecardLimit", "false");
  seed("Drastic/Use16BitColor", "false");
  seed("Drastic/AutoTrim", "false");
  seed("Drastic/FixMainEngineScreen", "false");
  seed("Drastic/RtcSystemTime", "true");
  seed("Drastic/CustomClockEnable", "false");
  seed("Drastic/CustomClock", "0");
  seed("Drastic/DisableEdgeMarking", "false");
  seed("Drastic/Hires3D", "false");
  seed("Drastic/LuaEnabled", "true");
  seed("Drastic/PreloadRoms", "true");
  seed("Drastic/Blend", "false");
  seed("Drastic/RawSaveFormat", "false");
  seed("Drastic/AutosaveInterval", "300");
  seed("Drastic/FirmwareNickname", "Switch");
  seed("Drastic/FirmwareLanguage", "-1");
  seed("Drastic/FirmwareColor", "0");
  seed("Drastic/FirmwareBirthdayMonth", "6");
  seed("Drastic/FirmwareBirthdayDay", "6");
}

static void migrate_hotkey_defaults(void) {
  const int version_index = find_entry("Wrapper/HotkeyDefaultsVersion");
  const int version = version_index < 0 ? 0 : atoi(entries[version_index].value);
  if (version >= 3) return;

  const int menu_index = find_entry("Wrapper/HotkeyMenu");
  const int quit_index = find_entry("Wrapper/HotkeyQuit");
  if (version < 2 && menu_index >= 0 && quit_index >= 0 &&
      !strcasecmp(entries[menu_index].value, "L+R+Minus") &&
      !strcasecmp(entries[quit_index].value, "L+R+Plus")) {
    put_entry("Wrapper/HotkeyMenu", "L+R+Plus");
    put_entry("Wrapper/HotkeyQuit", "None");
  }
  struct HotkeyDefault {
    const char *key;
    const char *old_value;
    const char *new_value;
  };
  static const struct HotkeyDefault safer_defaults[] = {
    {"Wrapper/HotkeySaveState", "L+R+Y", "L+R+Minus+Y"},
    {"Wrapper/HotkeyLoadState", "L+R+X", "L+R+Minus+X"},
    {"Wrapper/HotkeyNextSlot", "L+R+Up", "L+R+Minus+Up"},
    {"Wrapper/HotkeyPreviousSlot", "L+R+Down", "L+R+Minus+Down"},
    {"Wrapper/HotkeyReset", "L+R+A", "L+R+Minus+A"},
  };
  for (unsigned index = 0;
       index < sizeof(safer_defaults) / sizeof(*safer_defaults); index++) {
    const int entry_index = find_entry(safer_defaults[index].key);
    if (entry_index >= 0 &&
        !strcasecmp(entries[entry_index].value,
                    safer_defaults[index].old_value))
      put_entry(safer_defaults[index].key, safer_defaults[index].new_value);
  }
  put_entry("Wrapper/HotkeyDefaultsVersion", "3");
}

static void migrate_fast_forward_speed(void) {
  const int version = prefs_get_int("Wrapper/LauncherSettingsVersion", 0);
  if (version >= 3) return;
  if (prefs_get_int("Drastic/FastForwardSpeed", 2) == 0)
    put_entry("Drastic/FastForwardSpeed", "5");
  put_entry("Wrapper/LauncherSettingsVersion", "3");
}

static void migrate_stylus_mode(void) {
  if (find_entry("Wrapper/StylusMode") >= 0) return;
  const int legacy = find_entry("Wrapper/AnalogStylus");
  if (legacy < 0) return;
  put_entry("Wrapper/StylusMode",
            prefs_get_bool("Wrapper/AnalogStylus", true) ? "stick" : "off");
}

void prefs_set_disc_path(const char *path) {
  snprintf(pending_rom, sizeof(pending_rom), "%s", path ? path : "");
  if (path && *path) put_entry("Drastic/RomPath", path);
}

void prefs_init(const char *path) {
  memset(entries, 0, sizeof(entries));
  entry_count = 0;
  snprintf(ini_path, sizeof(ini_path), "%s", path ? path : PREFS_PATH);
  seed_critical_defaults();
  parse_file(ini_path);
  prefs_remove("Wrapper/CpuBoost");
  prefs_remove("Wrapper/LSFGDllPath");
  const float lsfg_flow = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f);
  if (prefs_contains("Wrapper/LSFGFlowScale"))
    put_entry("Wrapper/LSFGFlowScale", lsfg_flow > 0.375f ? "0.5" : "0.25");
  migrate_stylus_mode();
  if (!prefs_contains("Wrapper/LowLatency")) {
    const bool vulkan = !strcmp(prefs_get_string("Wrapper/Renderer", "deko3d"), "vk");
    const char *legacy = vulkan ? "Wrapper/VulkanLowLatency" : "Wrapper/Deko3DLowLatency";
    if (prefs_contains(legacy))
      prefs_set_bool("Wrapper/LowLatency", prefs_get_bool(legacy, true));
  }
  prefs_remove("Wrapper/VulkanLowLatency");
  prefs_remove("Wrapper/Deko3DLowLatency");
  seed_defaults();
  migrate_hotkey_defaults();
  migrate_fast_forward_speed();
}

void prefs_save(void) {
  if (!ini_path[0]) return;
  char temporary[sizeof(ini_path) + 8];
  snprintf(temporary, sizeof(temporary), "%s.tmp", ini_path);
  FILE *file = fopen(temporary, "wb");
  if (!file) return;
  fputs("# DrasticDS_nx launch configuration\n", file);
  for (int index = 0; index < entry_count; index++)
    fprintf(file, "%s = %s\n", entries[index].key, entries[index].value);
  int failed = fflush(file) != 0;
  if (fclose(file) != 0) failed = 1;
  if (failed) {
    remove(temporary);
    return;
  }
  remove(ini_path);
  rename(temporary, ini_path);
}

void prefs_save_runtime_key(const char *key) {
  prefs_save();
  if (!key || !*key) return;
  const char *profile = prefs_get_string("Wrapper/GameConfigPath", "");
  static const char allowed_root[] = DATA_ROOT "/gamecfg/";
  if (strncmp(profile, allowed_root, sizeof(allowed_root) - 1) ||
      strstr(profile, "..")) return;
  const int entry = find_entry(key);
  if (entry < 0) return;

  char temporary[1200];
  snprintf(temporary, sizeof(temporary), "%s.tmp", profile);
  FILE *input = fopen(profile, "rb");
  FILE *output = fopen(temporary, "wb");
  if (!output) {
    if (input) fclose(input);
    return;
  }
  int replaced = 0;
  char line[2048];
  while (input && fgets(line, sizeof(line), input)) {
    char copy[sizeof(line)];
    snprintf(copy, sizeof(copy), "%s", line);
    char *candidate = trim(copy);
    char *equals = strchr(candidate, '=');
    if (equals) {
      *equals = '\0';
      candidate = trim(candidate);
    }
    if (equals && !strcmp(candidate, key)) {
      fprintf(output, "%s = %s\n", key, entries[entry].value);
      replaced = 1;
    } else {
      fputs(line, output);
    }
  }
  if (input) fclose(input);
  if (!replaced) fprintf(output, "%s = %s\n", key, entries[entry].value);
  int failed = fflush(output) != 0;
  if (fclose(output) != 0) failed = 1;
  if (failed) {
    remove(temporary);
    return;
  }
  remove(profile);
  rename(temporary, profile);
}

bool prefs_contains(const char *key) { return find_entry(key) >= 0; }

const char *prefs_get_string(const char *key, const char *fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : entries[index].value;
}

bool prefs_get_bool(const char *key, bool fallback) {
  const char *value = prefs_get_string(key, fallback ? "true" : "false");
  if (!strcasecmp(value, "true") || !strcmp(value, "1") || !strcasecmp(value, "on")) return true;
  if (!strcasecmp(value, "false") || !strcmp(value, "0") || !strcasecmp(value, "off")) return false;
  return fallback;
}

int prefs_get_int(const char *key, int fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : (int)strtol(entries[index].value, NULL, 0);
}

int64_t prefs_get_int64(const char *key, int64_t fallback) {
  const int index = find_entry(key);
  if (index < 0) return fallback;
  char *end = NULL;
  const long long value = strtoll(entries[index].value, &end, 10);
  return end && *end == '\0' ? (int64_t)value : fallback;
}

float prefs_get_float(const char *key, float fallback) {
  const int index = find_entry(key);
  return index < 0 ? fallback : strtof(entries[index].value, NULL);
}

void prefs_set_bool(const char *key, bool value) { put_entry(key, value ? "true" : "false"); }
void prefs_set_string(const char *key, const char *value) { put_entry(key, value ? value : ""); }

void prefs_set_int(const char *key, int value) {
  char text[32];
  snprintf(text, sizeof(text), "%d", value);
  put_entry(key, text);
}

void prefs_set_float(const char *key, float value) {
  char text[32];
  snprintf(text, sizeof(text), "%g", value);
  put_entry(key, text);
}

void prefs_remove(const char *key) {
  const int index = find_entry(key);
  if (index < 0) return;
  entries[index] = entries[--entry_count];
  memset(&entries[entry_count], 0, sizeof(entries[entry_count]));
}
