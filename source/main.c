/* Drastic Android core host for Nintendo Switch. */

#include <switch.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"
#include "archive_loader.h"
#include "drastic_compat.h"
#include "drastic_config.h"
#include "drastic_jit.h"
#include "drastic_renderer.h"
#include "drastic_native_frame.h"
#include "drastic_rotation.h"
#include "error.h"
#include "imports.h"
#include "ingame_menu.h"
#include "input_sampler.h"
#include "jni_fake.h"
#include "libc_shim.h"
#include "opensles.h"
#include "overlay.h"
#include "prefs.h"
#include "pthr.h"
#include "so_util.h"
#include "switch/SwitchStorageBridge.h"
#include "util.h"

static void *heap_so_base;
static size_t heap_so_limit;
so_module emu_mod;

/* r2.6.0.4a stores its ARM7/ARM9 native code cache at BSS + 0x92000.
 * The three adjacent areas it marks RWX are 16 MiB, 1 MiB and 2 MiB. */
#define DRASTIC_JIT_OFFSET 0x001de000u
#define DRASTIC_JIT_SIZE   0x01300000u

static int configure_core_jit(so_module *mod) {
  /* Fingerprint the cache setup routine before relying on fixed offsets.
   * These are `mov w1,#0x1000000`, `mov w2,#7` at 0x961ac. */
  static const uint32_t expected[] = {0x52a02001u, 0x528000e2u};
  const size_t fingerprint = 0x961acu;
  if (!mod || fingerprint + sizeof(expected) > mod->load_size ||
      memcmp((const char *)mod->load_base + fingerprint,
             expected, sizeof(expected)) != 0)
    return 0;
  if (!so_add_jit_range(mod, DRASTIC_JIT_OFFSET, DRASTIC_JIT_SIZE))
    return 0;
  return drastic_jit_install(mod);
}

u32 __nx_nv_transfermem_size = 16 * 1024 * 1024;

void __libnx_initheap(void) {
  void *address;
  size_t size = 0;
  size_t available = 0, used = 0;
  if (envHasHeapOverride()) {
    address = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (available > used + 0x200000)
      size = (available - used - 0x200000) & ~(size_t)0x1fffff;
    const size_t gpu_reserve = (size_t)GPU_RESERVE_MB * 1024 * 1024;
    if (size > gpu_reserve + 384 * 1024 * 1024)
      size = (size - gpu_reserve) & ~(size_t)0x1fffff;
    if (!size) size = 512 * 1024 * 1024;
    Result result = svcSetHeapSize(&address, size);
    if (R_FAILED(result))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  size_t heap_size = size > so_reserve + 64 * 1024 * 1024
                         ? size - so_reserve : size / 2;
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)address;
  fake_heap_end = (char *)address + heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)fake_heap_end, 0x1000);
  heap_so_limit = (char *)address + size - (char *)heap_so_base;
}

typedef unsigned char jboolean;
typedef long long jlong;

static struct {
  int (*JNI_OnLoad)(void *vm, void *reserved);
  void (*JNI_OnUnload)(void *vm, void *reserved);
  void (*onInit)(void *env, void *clazz, void *activity,
                 int version_code, int sdk_int);
  int (*getRomType)(void *env, void *clazz, void *path);
  /* Java declaration: startGame(String path, int loadSlot, long config,
   *                             int startupMode, boolean zipped, long clock).
   * loadSlot is -1 for a normal boot; passing a ROM type here makes the core
   * try to restore that save-state slot during startup. */
  jboolean (*startGame)(void *env, void *clazz, void *path, int load_slot,
                        jlong config, int startup_mode, jboolean archive,
                        jlong clock);
  void (*applyConfig)(void *env, void *clazz, jlong config);
  void (*setFirmwareUserdata)(void *env, void *clazz, void *nickname,
                              int packed);
  void (*setAutosaveInterval)(void *env, void *clazz, int seconds);
  void (*setAudioVolume)(void *env, void *clazz, int volume);
  void (*updateInput)(void *env, void *clazz, int buttons,
                      int touch_position, int autofire);
  void (*waitScreen)(void *env, void *clazz);
  void (*signalScreen)(void *env, void *clazz);
  DrasticCoreRenderFrame renderFrame;
  int (*getFrameInfo)(void *env, void *clazz);
  jboolean (*getRumbleState)(void *env, void *clazz);
  jboolean (*saveState)(void *env, void *clazz, int slot, jboolean user);
  jboolean (*loadState)(void *env, void *clazz, int slot);
  jboolean (*isSaving)(void *env, void *clazz);
  int (*getSavingSlot)(void *env, void *clazz);
  void (*getSnapshots16)(void *env, void *clazz, int slot,
                         void *top, void *bottom);
  void (*getSnapshots16Direct)(void *env, void *clazz, void *path,
                               void *top, void *bottom);
  void (*resetDS)(void *env, void *clazz);
  void (*pauseSystem)(void *env, void *clazz, int pause);
  void (*quitSystem)(void *env, void *clazz);
  void (*releaseSystem)(void *env, void *clazz);
  void (*setHingeStatus)(void *env, void *clazz, jboolean closed);
  void (*setWhitenoiseFeed)(void *env, void *clazz, jboolean enabled);
  void (*luaUpdateAxisValues)(void *env, void *clazz, float lx, float ly,
                              float rx, float ry);
  void (*luaUpdateRotation)(void *env, void *clazz, int degrees);
  void (*updateAccelerometer)(void *env, void *clazz,
                              float x, float y, float z);
  void (*updateGyroscope)(void *env, void *clazz, float z);
  int (*getCheatCount)(void *env, void *clazz);
  jboolean (*getCheatEnabled)(void *env, void *clazz, int index);
  void *(*getCheatName)(void *env, void *clazz, int index);
  void *(*getCheatNote)(void *env, void *clazz, int index);
  int (*getCheatFolderId)(void *env, void *clazz, int index);
  int (*getCheatFolderCount)(void *env, void *clazz);
  jboolean (*getCheatFolderMultiSelect)(void *env, void *clazz, int index);
  void *(*getCheatFolderName)(void *env, void *clazz, int index);
  void (*setCheatEnabled)(void *env, void *clazz, int index,
                          jboolean enabled);
  int (*getCustomCheatCount)(void *env, void *clazz);
  jboolean (*getCustomCheatEnabled)(void *env, void *clazz, int index);
  void *(*getCustomCheatName)(void *env, void *clazz, int index);
  void (*setCustomCheatEnabled)(void *env, void *clazz, int index,
                                jboolean enabled);
  int (*addCustomCheat)(void *env, void *clazz, void *name, void *codes,
                        int count, jboolean enabled);
  void (*removeCustomCheat)(void *env, void *clazz, int index);
  void (*updateCheats)(void *env, void *clazz, jboolean reload);
} core;

#define DRASTIC_SYMBOL(name) "Java_com_dsemu_drastic_DraSticJNI_" name
#define RESOLVE_REQUIRED(field, name) \
  core.field = (void *)so_find_addr_rx(&emu_mod, DRASTIC_SYMBOL(name))
#define RESOLVE_OPTIONAL(field, name) \
  core.field = (void *)so_try_find_addr_rx(&emu_mod, DRASTIC_SYMBOL(name))

static void resolve_core(void) {
  core.JNI_OnLoad = (void *)so_find_addr_rx(&emu_mod, "JNI_OnLoad");
  core.JNI_OnUnload = (void *)so_try_find_addr_rx(&emu_mod, "JNI_OnUnload");
  RESOLVE_REQUIRED(onInit, "onInit");
  RESOLVE_REQUIRED(getRomType, "getRomType");
  RESOLVE_REQUIRED(startGame, "startGame");
  RESOLVE_REQUIRED(applyConfig, "applyConfig");
  RESOLVE_REQUIRED(setFirmwareUserdata, "setFirmwareUserdata");
  RESOLVE_REQUIRED(setAutosaveInterval, "setAutosaveInterval");
  RESOLVE_REQUIRED(setAudioVolume, "setAudioVolume");
  RESOLVE_REQUIRED(updateInput, "updateInput");
  RESOLVE_REQUIRED(waitScreen, "waitScreen");
  RESOLVE_REQUIRED(signalScreen, "signalScreen");
  RESOLVE_REQUIRED(renderFrame, "renderFrame");
  RESOLVE_OPTIONAL(getFrameInfo, "getFrameInfo");
  RESOLVE_OPTIONAL(getRumbleState, "getRumbleState");
  RESOLVE_REQUIRED(saveState, "saveState");
  RESOLVE_REQUIRED(loadState, "loadState");
  RESOLVE_OPTIONAL(isSaving, "isSaving");
  RESOLVE_OPTIONAL(getSavingSlot, "getSavingSlot");
  RESOLVE_OPTIONAL(getSnapshots16, "getSnapshots16");
  RESOLVE_OPTIONAL(getSnapshots16Direct, "getSnapshots16Direct");
  RESOLVE_REQUIRED(resetDS, "resetDS");
  RESOLVE_REQUIRED(pauseSystem, "pauseSystem");
  RESOLVE_REQUIRED(quitSystem, "quitSystem");
  RESOLVE_REQUIRED(releaseSystem, "releaseSystem");
  RESOLVE_REQUIRED(setHingeStatus, "setHingeStatus");
  RESOLVE_REQUIRED(setWhitenoiseFeed, "setWhitenoiseFeed");
  RESOLVE_OPTIONAL(luaUpdateAxisValues, "luaUpdateAxisValues");
  RESOLVE_OPTIONAL(luaUpdateRotation, "luaUpdateRotation");
  RESOLVE_REQUIRED(updateAccelerometer, "updateAccelerometer");
  RESOLVE_REQUIRED(updateGyroscope, "updateGyroscope");
  RESOLVE_REQUIRED(getCheatCount, "getCheatCount");
  RESOLVE_REQUIRED(getCheatEnabled, "getCheatEnabled");
  RESOLVE_REQUIRED(getCheatName, "getCheatName");
  RESOLVE_REQUIRED(getCheatNote, "getCheatNote");
  RESOLVE_REQUIRED(getCheatFolderId, "getCheatFolderId");
  RESOLVE_REQUIRED(getCheatFolderCount, "getCheatFolderCount");
  RESOLVE_REQUIRED(getCheatFolderMultiSelect, "getCheatFolderMultiSelect");
  RESOLVE_REQUIRED(getCheatFolderName, "getCheatFolderName");
  RESOLVE_REQUIRED(setCheatEnabled, "setCheatEnabled");
  RESOLVE_REQUIRED(getCustomCheatCount, "getCustomCheatCount");
  RESOLVE_REQUIRED(getCustomCheatEnabled, "getCustomCheatEnabled");
  RESOLVE_REQUIRED(getCustomCheatName, "getCustomCheatName");
  RESOLVE_REQUIRED(setCustomCheatEnabled, "setCustomCheatEnabled");
  RESOLVE_REQUIRED(addCustomCheat, "addCustomCheat");
  RESOLVE_REQUIRED(removeCustomCheat, "removeCustomCheat");
  RESOLVE_REQUIRED(updateCheats, "updateCheats");
}

typedef struct {
  pthread_t thread;
  void *clazz;
  void *rom;
  int load_slot;
  jlong config;
  jlong clock;
  int startup_mode;
  jboolean archive;
  volatile int finished;
  int result;
} CoreGameThread;

static void *core_game_thread_main(void *opaque) {
  CoreGameThread *game = (CoreGameThread *)opaque;
  pthr_install_fake_tls();
  pthr_pin_emulation_core();
  const int result = core.startGame(
      fake_env, game->clazz, game->rom, game->load_slot, game->config,
      game->startup_mode, game->archive, game->clock);
  game->result = result;
  __atomic_store_n(&game->finished, 1, __ATOMIC_RELEASE);
  /* Wake a presentation thread parked in waitScreen() so it can observe the
   * terminal result instead of remaining asleep after an early failure. */
  core.signalScreen(fake_env, game->clazz);
  return (void *)(uintptr_t)(unsigned)result;
}

static int core_game_thread_start(CoreGameThread *game) {
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);
  const int result = pthread_create(&game->thread, &attributes,
                                    core_game_thread_main, game);
  pthread_attr_destroy(&attributes);
  return result;
}

static int make_directory(const char *path) {
  if (mkdir(path, 0777) == 0 || errno == EEXIST) return 1;
  return 0;
}

static void setup_directories(void) {
  const char *directories[] = {
    "/switch", DATA_ROOT, SYSTEM_DIR, USER_DIR, CACHE_DIR, UNZIP_CACHE_DIR,
    GAMES_DIR, CHEATS_DIR, SCRIPTS_DIR, SHADERS_DIR, SLOT2_DIR, MICROPHONE_DIR,
    SAVESTATES_DIR,
    BACKUPS_DIR,
  };
  for (unsigned index = 0; index < sizeof(directories) / sizeof(*directories);
       index++)
    if (!make_directory(directories[index]))
      fatal_error("Could not create %s.", directories[index]);
}

static int regular_file(const char *path) {
  struct stat status;
  return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static void validate_inputs(const DrasticRuntimeConfig *config) {
  if (!regular_file(config->core_path))
    fatal_error("Missing Drastic core:\n%s", config->core_path);
  if (!regular_file(config->rom_path))
    fatal_error("Nintendo DS ROM not found:\n%s", config->rom_path);
  if ((!regular_file(SYSTEM_DIR "/nds_bios_arm7.bin") &&
       !regular_file(SYSTEM_DIR "/drastic_bios_arm7.bin")) ||
      (!regular_file(SYSTEM_DIR "/nds_bios_arm9.bin") &&
       !regular_file(SYSTEM_DIR "/drastic_bios_arm9.bin")))
    fatal_error("Nintendo DS BIOS files are missing from\n%s.\n\n"
                "Copy nds_bios_arm7.bin and nds_bios_arm9.bin there.",
                SYSTEM_DIR);
  if (!regular_file(SYSTEM_DIR "/nds_firmware_modified.bin") &&
      !regular_file(SYSTEM_DIR "/nds_firmware.bin"))
    fatal_error("Nintendo DS firmware is missing from\n%s.\n\n"
                "Copy nds_firmware.bin there.", SYSTEM_DIR);
  if (!regular_file(SYSTEM_DIR "/game_database.xml"))
    fatal_error("Drastic game_database.xml is missing from\n%s.", SYSTEM_DIR);
}

static void check_jit_services(void) {
  if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78) ||
      !envIsSyscallHinted(0x73) ||
      envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("The required JIT syscalls are unavailable.\n\n"
                "Launch hbmenu over an installed game, then start Drastic.");
}

static void select_panel_size(void) {
  if (appletGetOperationMode() == AppletOperationMode_Console) {
    panel_width = screen_width = 1920;
    panel_height = screen_height = 1080;
  } else {
    panel_width = screen_width = 1280;
    panel_height = screen_height = 720;
  }

  /* libnx creates the default NWindow at 1280x720 even in console mode.
   * Configure it before EGL/Vulkan registers any buffers; changing only the
   * wrapper globals leaves a 720p layer centred inside the 1080p TV output. */
  NWindow *window = nwindowGetDefault();
  if (!window || R_FAILED(nwindowSetDimensions(
          window, (u32)panel_width, (u32)panel_height)) ||
      R_FAILED(nwindowSetCrop(window, 0, 0, panel_width, panel_height)))
    fatal_error("Could not configure the %dx%d display surface.",
                panel_width, panel_height);
}

enum {
  DS_UP = 1,
  DS_DOWN = 2,
  DS_LEFT = 4,
  DS_RIGHT = 8,
  DS_A = 16,
  DS_B = 32,
  DS_X = 64,
  DS_Y = 128,
  DS_L = 256,
  DS_R = 512,
  DS_START = 1024,
  DS_SELECT = 2048,
};

typedef struct { const char *name; u64 button; } SwitchButton;
static const SwitchButton switch_buttons[] = {
  {"A",HidNpadButton_A},{"B",HidNpadButton_B},{"X",HidNpadButton_X},
  {"Y",HidNpadButton_Y},{"L",HidNpadButton_L},{"R",HidNpadButton_R},
  {"ZL",HidNpadButton_ZL},{"ZR",HidNpadButton_ZR},
  {"Plus",HidNpadButton_Plus},{"Minus",HidNpadButton_Minus},
  {"StickL",HidNpadButton_StickL},{"StickR",HidNpadButton_StickR},
  {"Up",HidNpadButton_Up},{"Down",HidNpadButton_Down},
  {"Left",HidNpadButton_Left},{"Right",HidNpadButton_Right},
};

typedef struct {
  const char *key;
  const char *fallback;
  int ds_mask;
  u64 switch_mask;
} DsBinding;

static DsBinding bindings[] = {
  {"Wrapper/Pad/A","A",DS_A,0},{"Wrapper/Pad/B","B",DS_B,0},
  {"Wrapper/Pad/X","X",DS_X,0},{"Wrapper/Pad/Y","Y",DS_Y,0},
  {"Wrapper/Pad/L","L",DS_L,0},{"Wrapper/Pad/R","R",DS_R,0},
  {"Wrapper/Pad/Start","Plus",DS_START,0},
  {"Wrapper/Pad/Select","Minus",DS_SELECT,0},
  {"Wrapper/Pad/Up","Up",DS_UP,0},{"Wrapper/Pad/Down","Down",DS_DOWN,0},
  {"Wrapper/Pad/Left","Left",DS_LEFT,0},
  {"Wrapper/Pad/Right","Right",DS_RIGHT,0},
};

static u64 button_for_token(const char *token) {
  if (!token || !*token || !strcasecmp(token, "None")) return 0;
  for (unsigned index = 0; index < sizeof(switch_buttons) / sizeof(*switch_buttons);
       index++)
    if (!strcasecmp(token, switch_buttons[index].name))
      return switch_buttons[index].button;
  return 0;
}

static u64 buttons_for_combo(const char *combo) {
  if (!combo || !*combo || !strcasecmp(combo, "None")) return 0;
  char copy[192];
  if (strlen(combo) >= sizeof(copy)) return 0;
  strcpy(copy, combo);
  u64 result = 0;
  char *save = NULL;
  for (char *token = strtok_r(copy, "+", &save); token;
       token = strtok_r(NULL, "+", &save)) {
    while (*token && isspace((unsigned char)*token)) token++;
    char *end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) *--end = '\0';
    const u64 button = button_for_token(token);
    if (!button) return 0;
    result |= button;
  }
  return result;
}

static int analog_dpad_enabled;
static int analog_dpad_deadzone;

static void load_bindings(void) {
  for (unsigned index = 0; index < sizeof(bindings) / sizeof(*bindings); index++)
    bindings[index].switch_mask = button_for_token(
        prefs_get_string(bindings[index].key, bindings[index].fallback));
  analog_dpad_enabled = prefs_get_bool("Wrapper/AnalogDpad", true);
  int percent = prefs_get_int("Wrapper/AnalogDeadzone", 35);
  if (percent < 5) percent = 5;
  if (percent > 80) percent = 80;
  analog_dpad_deadzone = percent * 32767 / 100;
}

static HidVibrationDeviceHandle vibration_player[2];
static HidVibrationDeviceHandle vibration_handheld[2];
static int vibration_player_ready;
static int vibration_handheld_ready;
static int rumble_active;

static void update_motion(const DrasticRuntimeConfig *config, void *clazz,
                          const DrasticInputSnapshot *input,
                          u64 *last_sample, int *last_source) {
  if (!config->motion || !core.updateAccelerometer || !core.updateGyroscope)
    return;
  if (!input || !input->motion_sample || input->motion_source < 0 ||
      (input->motion_sample == *last_sample &&
       input->motion_source == *last_source))
    return;
  *last_sample = input->motion_sample;
  *last_source = input->motion_source;

  /* Switch HID reports acceleration in g. Map the controller axes to the
   * Android device axes Drastic expects, then apply the configured view
   * rotation exactly as the Android frontend did. */
  const float gravity = 9.80665f;
  float x = -input->motion_acceleration.y * gravity;
  float y = input->motion_acceleration.z * gravity;
  const float z = -input->motion_acceleration.x * gravity;
  if (input->motion_right_joycon) { x = -x; y = -y; }
  float rotated_x, rotated_y;
  drastic_rotation_display_delta_to_source(
      config->rotation, x, y, &rotated_x, &rotated_y);
  core.updateAccelerometer(fake_env, clazz, rotated_x, rotated_y, z);
  core.updateGyroscope(fake_env, clazz, -input->motion_angular_velocity.x);
}

static void send_rumble(const HidVibrationValue values[2]) {
  if (vibration_player_ready)
    hidSendVibrationValues(vibration_player, values, 2);
  if (vibration_handheld_ready)
    hidSendVibrationValues(vibration_handheld, values, 2);
}

static void update_rumble(const DrasticRuntimeConfig *config, void *clazz) {
  if (!config->vibration) {
    if (rumble_active) {
      HidVibrationValue stopped[2] = {0};
      send_rumble(stopped);
      rumble_active = 0;
    }
    return;
  }
  if (!core.getRumbleState ||
      (!vibration_player_ready && !vibration_handheld_ready)) return;
  const int active = core.getRumbleState(fake_env, clazz) != 0;
  if (active == rumble_active) return;
  rumble_active = active;
  HidVibrationValue values[2] = {0};
  for (int index = 0; index < 2; index++) {
    values[index].amp_low = active ? 0.35f : 0.0f;
    values[index].freq_low = 160.0f;
    values[index].amp_high = active ? 0.22f : 0.0f;
    values[index].freq_high = 320.0f;
  }
  send_rumble(values);
}

typedef struct {
  u64 menu;
  u64 fast_forward;
  u64 swap_screens;
  u64 microphone;
  u64 motion_stylus_recenter;
  u64 autofire;
  u64 lid;
  u64 save_state;
  u64 load_state;
  u64 next_slot;
  u64 previous_slot;
  u64 reset;
  u64 quit;
} RuntimeHotkeys;

typedef struct {
  RuntimeHotkeys hotkeys;
  int fast_forward;
  int fast_forward_latched;
  int fast_forward_toggle;
  int microphone_feed;
  int hinge_closed;
  int exit_requested;
  int state_slot;
  int stylus_speed;
  int lua_rotation_sent;
  u64 motion_sample_sent;
  int motion_source_sent;
  u64 analog_touch_button;
} RuntimeControls;

typedef struct {
  u64 window_start;
  unsigned window_frames;
  float fps;
} RuntimeHud;

static void remove_duplicate_hotkeys(RuntimeHotkeys *hotkeys) {
  u64 *ordered[] = {
    &hotkeys->menu,
    &hotkeys->fast_forward,
    &hotkeys->swap_screens,
    &hotkeys->microphone,
    &hotkeys->motion_stylus_recenter,
    &hotkeys->autofire,
    &hotkeys->lid,
    &hotkeys->save_state,
    &hotkeys->load_state,
    &hotkeys->next_slot,
    &hotkeys->previous_slot,
    &hotkeys->reset,
    &hotkeys->quit,
  };
  for (unsigned index = 0;
       index < sizeof(ordered) / sizeof(*ordered); index++) {
    if (!*ordered[index]) continue;
    for (unsigned earlier = 0; earlier < index; earlier++) {
      if (*ordered[index] == *ordered[earlier]) {
        *ordered[index] = 0;
        break;
      }
    }
  }
}

static void reset_runtime_fps_window(RuntimeHud *hud) {
  hud->window_start = 0;
  hud->window_frames = 0;
}

static void update_runtime_hud(RuntimeHud *hud,
                               const DrasticRuntimeConfig *config,
                               const RuntimeControls *controls,
                               int consumed_core_frame) {
  if (!consumed_core_frame) {
    reset_runtime_fps_window(hud);
  } else {
    const u64 now = armGetSystemTick();
    const u64 frequency = armGetSystemTickFreq();
    if (!hud->window_start)
      hud->window_start = now;
    else
      hud->window_frames++;
    const u64 elapsed = now - hud->window_start;
    if (frequency && elapsed >= frequency) {
      hud->fps = (float)((double)hud->window_frames * (double)frequency /
                         (double)elapsed);
      hud->window_start = now;
      hud->window_frames = 0;
    }
  }
  overlay_draw_hud(config->show_fps, hud->fps, controls->fast_forward);
}

static void load_runtime_controls(RuntimeControls *controls) {
  controls->hotkeys.menu = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyMenu", "L+R+Plus"));
  controls->hotkeys.fast_forward = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyFastForward", "ZR"));
  controls->hotkeys.swap_screens = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeySwapScreens", "ZL"));
  controls->hotkeys.microphone = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyMicrophone", "StickL"));
  controls->hotkeys.motion_stylus_recenter = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyMotionStylusRecenter",
                       "L+R+StickR"));
  controls->hotkeys.autofire = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyAutoFire", "None"));
  controls->hotkeys.lid = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyLid", "None"));
  controls->hotkeys.save_state = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeySaveState", "L+R+Minus+Y"));
  controls->hotkeys.load_state = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyLoadState", "L+R+Minus+X"));
  controls->hotkeys.next_slot = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyNextSlot", "L+R+Minus+Up"));
  controls->hotkeys.previous_slot = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyPreviousSlot", "L+R+Minus+Down"));
  controls->hotkeys.reset = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyReset", "L+R+Minus+A"));
  controls->hotkeys.quit = buttons_for_combo(
      prefs_get_string("Wrapper/HotkeyQuit", "None"));
  /* A saved duplicate must never execute two emulator actions. The menu is
   * reserved first, then fast-forward, followed by the remaining hotkeys. */
  remove_duplicate_hotkeys(&controls->hotkeys);
  controls->fast_forward_toggle = !strcasecmp(
      prefs_get_string("Wrapper/FastForwardMode", "hold"), "toggle");
  controls->analog_touch_button = button_for_token(
      prefs_get_string("Wrapper/AnalogTouchButton", "StickR"));
  controls->stylus_speed = prefs_get_int("Wrapper/AnalogStylusSpeed", 8);
  if (controls->stylus_speed < 1) controls->stylus_speed = 1;
  if (controls->stylus_speed > 20) controls->stylus_speed = 20;
  controls->lua_rotation_sent = 360;
  controls->motion_sample_sent = 0;
  controls->motion_source_sent = -1;
}

static int combo_held(u64 held, u64 combo) {
  return combo && (held & combo) == combo;
}

static float normalized_stick_axis(int value) {
  float result = (float)value / 32767.0f;
  if (result < -1.0f) result = -1.0f;
  if (result > 1.0f) result = 1.0f;
  return result;
}

static void sampler_update_input(void *clazz, int buttons,
                                 int touch_position, int autofire) {
  core.updateInput(fake_env, clazz, buttons, touch_position, autofire);
}

static void configure_input_sampler(DrasticInputSamplerConfig *config,
                                    const RuntimeControls *controls,
                                    void *clazz) {
  memset(config, 0, sizeof(*config));
  config->update = sampler_update_input;
  config->user = clazz;
  config->binding_count = (int)(sizeof(bindings) / sizeof(*bindings));
  for (int index = 0; index < config->binding_count; index++) {
    config->bindings[index].switch_mask = bindings[index].switch_mask;
    config->bindings[index].ds_mask = bindings[index].ds_mask;
  }
  config->analog_dpad = analog_dpad_enabled;
  config->analog_deadzone = analog_dpad_deadzone;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_MENU] = controls->hotkeys.menu;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_FAST_FORWARD] =
      controls->hotkeys.fast_forward;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_SWAP_SCREENS] =
      controls->hotkeys.swap_screens;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_MICROPHONE] =
      controls->hotkeys.microphone;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_MOTION_STYLUS_RECENTER] =
      controls->hotkeys.motion_stylus_recenter;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_AUTOFIRE] = controls->hotkeys.autofire;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_LID] = controls->hotkeys.lid;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_SAVE_STATE] =
      controls->hotkeys.save_state;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_LOAD_STATE] =
      controls->hotkeys.load_state;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_NEXT_SLOT] =
      controls->hotkeys.next_slot;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_PREVIOUS_SLOT] =
      controls->hotkeys.previous_slot;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_RESET] = controls->hotkeys.reset;
  config->hotkeys[DRASTIC_INPUT_HOTKEY_QUIT] = controls->hotkeys.quit;
  config->analog_touch_button = controls->analog_touch_button;
  config->stylus_speed = controls->stylus_speed;
  config->panel_width = panel_width;
  config->panel_height = panel_height;
}

static int process_input(DrasticRuntimeConfig *config,
                         RuntimeControls *controls, void *clazz,
                         DrasticIngameMenu *menu,
                         DrasticInputSampler *sampler) {
  DrasticInputSnapshot input;
  drastic_input_sampler_read(sampler, &input);
  const u64 held = input.buttons;
  const uint32_t pressed = input.hotkeys_pressed;
  config->stylus_x = input.stylus_x;
  config->stylus_y = input.stylus_y;
  config->stylus_visible = input.stylus_visible;

  if (menu && (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                            DRASTIC_INPUT_HOTKEY_MENU))) {
    drastic_input_sampler_update_runtime(sampler, config, false);
    return 1;
  }

  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                    DRASTIC_INPUT_HOTKEY_SAVE_STATE)) {
    core.pauseSystem(fake_env, clazz, 1);
    core.saveState(fake_env, clazz, controls->state_slot, 1);
    core.pauseSystem(fake_env, clazz, 0);
  }
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                    DRASTIC_INPUT_HOTKEY_LOAD_STATE)) {
    core.pauseSystem(fake_env, clazz, 1);
    core.loadState(fake_env, clazz, controls->state_slot);
    core.pauseSystem(fake_env, clazz, 0);
  }
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(DRASTIC_INPUT_HOTKEY_NEXT_SLOT))
    controls->state_slot = (controls->state_slot + 1) % 10;
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                    DRASTIC_INPUT_HOTKEY_PREVIOUS_SLOT))
    controls->state_slot = (controls->state_slot + 9) % 10;
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(DRASTIC_INPUT_HOTKEY_RESET)) {
    core.pauseSystem(fake_env, clazz, 1);
    core.resetDS(fake_env, clazz);
    core.pauseSystem(fake_env, clazz, 0);
  }
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(DRASTIC_INPUT_HOTKEY_QUIT))
    controls->exit_requested = 1;

  if (controls->fast_forward_toggle &&
      (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                     DRASTIC_INPUT_HOTKEY_FAST_FORWARD)))
    controls->fast_forward_latched ^= 1;
  const int fast_forward = controls->fast_forward_toggle
                               ? controls->fast_forward_latched
                               : combo_held(held, controls->hotkeys.fast_forward);
  if (fast_forward != controls->fast_forward) {
    controls->fast_forward = fast_forward;
    uint64_t packed = config->core_config;
    if (fast_forward) packed |= UINT64_C(1) << 29;
    core.applyConfig(fake_env, clazz, (jlong)packed);
  }
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(
                    DRASTIC_INPUT_HOTKEY_SWAP_SCREENS)) {
    config->swap_screens ^= 1;
    drastic_config_calculate_layout(config, panel_width, panel_height);
  }
  const int microphone_feed =
      config->microphone_enabled &&
      config->microphone_source == DRASTIC_MICROPHONE_SIMULATED &&
      combo_held(held, controls->hotkeys.microphone);
  if (microphone_feed != controls->microphone_feed) {
    controls->microphone_feed = microphone_feed;
    core.setWhitenoiseFeed(fake_env, clazz, microphone_feed != 0);
  }
  if (pressed & DRASTIC_INPUT_HOTKEY_BIT(DRASTIC_INPUT_HOTKEY_LID)) {
    controls->hinge_closed ^= 1;
    core.setHingeStatus(fake_env, clazz, controls->hinge_closed != 0);
  }

  if (config->lua_enabled && core.luaUpdateAxisValues)
    core.luaUpdateAxisValues(fake_env, clazz,
                             normalized_stick_axis(input.left.x),
                             -normalized_stick_axis(input.left.y),
                             normalized_stick_axis(input.right.x),
                             -normalized_stick_axis(input.right.y));
  if (config->lua_enabled && core.luaUpdateRotation &&
      controls->lua_rotation_sent != config->rotation) {
    static const int rotation_degrees[] = {0, 90, 180, -90};
    core.luaUpdateRotation(fake_env, clazz,
                           rotation_degrees[config->rotation & 3]);
    controls->lua_rotation_sent = config->rotation;
  }

  update_rumble(config, clazz);
  update_motion(config, clazz, &input, &controls->motion_sample_sent,
                &controls->motion_source_sent);
  drastic_input_sampler_update_runtime(sampler, config, true);
  return 0;
}

static int has_archive_extension(const char *path) {
  const char *extension = strrchr(path, '.');
  return extension && (!strcasecmp(extension, ".zip") ||
                       !strcasecmp(extension, ".rar"));
}

static void shutdown_core(void *clazz, CoreGameThread *game) {
  if (!__atomic_load_n(&game->finished, __ATOMIC_ACQUIRE)) {
    /* Match DraSticEmuActivity.onPause() followed by its shutdown helper:
     * pauseSystem(1), wake the render wait, quitSystem(), join(), and finally
     * releaseSystem().  In particular, never let the menu resume the core and
     * race straight into teardown. */
    core.pauseSystem(fake_env, clazz, 1);
    core.signalScreen(fake_env, clazz);
    core.quitSystem(fake_env, clazz);
  }
  pthread_join(game->thread, NULL);
  core.releaseSystem(fake_env, clazz);
  if (core.JNI_OnUnload)
    core.JNI_OnUnload(fake_vm, NULL);
}

static jlong configured_start_clock(void) {
  if (!prefs_get_bool("Drastic/CustomClockEnable", false)) return -1;
  const int64_t clock = prefs_get_int64("Drastic/CustomClock", 0);
  return clock != 0 ? (jlong)clock : -1;
}

typedef struct {
  void *clazz;
  CoreGameThread *game;
  DrasticRuntimeConfig *runtime;
  RuntimeControls *controls;
  DrasticIngameMenu *menu;
  DrasticInputSampler *input_sampler;
  int suspended;
  int resume_core;
  int resumed;
} AppletLifecycle;

static void suspend_emulation(AppletLifecycle *lifecycle) {
  if (!lifecycle || lifecycle->suspended) return;
  lifecycle->suspended = 1;
  drastic_input_sampler_update_runtime(
      lifecycle->input_sampler, lifecycle->runtime, false);
  lifecycle->resume_core =
      !__atomic_load_n(&lifecycle->game->finished, __ATOMIC_ACQUIRE) &&
      !drastic_menu_is_open(lifecycle->menu);
  if (lifecycle->resume_core)
    core.pauseSystem(fake_env, lifecycle->clazz, 1);
  opensles_set_suspended(true);
  drastic_renderer_suspend();
}

static void resume_emulation(AppletLifecycle *lifecycle) {
  if (!lifecycle || !lifecycle->suspended) return;
  drastic_renderer_resume();
  opensles_set_suspended(false);
  if (lifecycle->resume_core &&
      !__atomic_load_n(&lifecycle->game->finished, __ATOMIC_ACQUIRE))
    core.pauseSystem(fake_env, lifecycle->clazz, 0);
  lifecycle->resume_core = 0;
  lifecycle->suspended = 0;
  lifecycle->resumed = 1;
  drastic_input_sampler_update_runtime(
      lifecycle->input_sampler, lifecycle->runtime,
      !drastic_menu_is_open(lifecycle->menu));
}

static void applet_lifecycle_hook(AppletHookType hook, void *parameter) {
  if (hook != AppletHookType_OnFocusState &&
      hook != AppletHookType_OnResume) return;
  AppletLifecycle *lifecycle = (AppletLifecycle *)parameter;
  const int focused = appletGetFocusState() == AppletFocusState_InFocus;
  if (!focused) {
    suspend_emulation(lifecycle);
    /* NoSuspend lets us receive the focus-loss message and drain services
     * first. Hand control back to the OS only after the host is quiescent. */
    appletSetFocusHandlingMode(
        AppletFocusHandlingMode_SuspendHomeSleepNotify);
  } else {
    /* Keep running long enough to restore every service before rendering the
     * first post-resume frame. */
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    resume_emulation(lifecycle);
  }
}

int main(void) {
  cpu_boost(1);
  bool cpu_boost_active = true;
  setup_directories();
  prefs_init(PREFS_PATH);
  const char *configured_renderer =
      prefs_get_string("Wrapper/Renderer", "deko3d");
  drastic_renderer_select(configured_renderer);
  /* The unified Horizon Mesa EGL archive contains both native NVC0 and Zink.
   * Select the configured backend before the process creates its first EGL
   * display; an EGLDisplay keeps that backend for its entire lifetime. */
  if (drastic_renderer_backend() == DRASTIC_RENDERER_BACKEND_GL) {
    const bool use_zink = !strcmp(configured_renderer, "zink");
    setenv("MESA_SWITCH_GL_DRIVER", use_zink ? "zink" : "nvc0", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", use_zink ? "zink" : "nouveau", 1);
  } else {
    unsetenv("MESA_SWITCH_GL_DRIVER");
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
  }
  /* Drastic builds mirrored ARM7/ARM9 address-space views from Android ashmem.
   * The Switch shim provides those aliases and the lazy 4 GiB fastmem window. */
  fastmem_set_mode(FASTMEM_MODE_ON);
  load_bindings();

  DrasticRuntimeConfig runtime;
  drastic_config_load(&runtime);
  opensles_set_microphone_enabled(runtime.microphone_enabled != 0);
  opensles_set_microphone_source(
      runtime.microphone_source == DRASTIC_MICROPHONE_EXTERNAL
          ? OPENSLES_MIC_SOURCE_EXTERNAL
          : OPENSLES_MIC_SOURCE_SIMULATED);
  char storage_error[256];
  if (!switchStorageInitializeForPath(DATA_ROOT "/launcher.ini", runtime.rom_path,
                                      sizeof(runtime.rom_path), storage_error,
                                      sizeof(storage_error)))
    fatal_error("Could not mount game storage:\n%s\n\n%s",
                runtime.rom_path, storage_error);
  validate_inputs(&runtime);
  check_jit_services();
  select_panel_size();

  extern char *fake_heap_start;
  const size_t heap_mb = ((char *)heap_so_base - fake_heap_start) / (1024 * 1024);
  if (heap_mb < 384)
    fatal_error("Not enough memory (%u MiB).\n\n"
                "Launch hbmenu over an installed game.", (unsigned)heap_mb);

  if (so_load(&emu_mod, runtime.core_path, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load Drastic core:\n%s", runtime.core_path);
  update_imports();
  so_relocate(&emu_mod);
  so_resolve(&emu_mod, dynlib_functions, dynlib_numfunctions, 1);
  resolve_core();
  if (!drastic_compat_install(&emu_mod))
    fatal_error("Unsupported Drastic ARM64 core compatibility layout.");
  if (drastic_renderer_is_deko3d() && !drastic_native_frame_install(&emu_mod))
    fatal_error("Unsupported Drastic ARM64 native framebuffer layout.\n"
                "Deko3D requires r2.6.0.4a build 109.");
  if (!configure_core_jit(&emu_mod))
    fatal_error("Unsupported Drastic ARM64 core JIT layout.");
  so_finalize(&emu_mod);
  so_flush_caches(&emu_mod);

  pthr_install_fake_tls();
  so_execute_init_array(&emu_mod);
  so_free_temp(&emu_mod);
  jni_init();

  void *clazz = jni_obj_new("com/dsemu/drastic/DraSticJNI");
  void *activity = jni_obj_new("com/dsemu/drastic/DraSticActivity");
  const int jni_result = core.JNI_OnLoad(fake_vm, NULL);
  if (jni_result < 0)
    fatal_error("Drastic JNI initialization failed.");
  core.onInit(fake_env, clazz, activity, DRASTIC_APK_VERSION_CODE,
              ANDROID_SDK_INT);
  core.setFirmwareUserdata(fake_env, clazz,
                           jni_make_string(runtime.firmware_nickname),
                           (int)runtime.firmware_userdata);
  core.setAutosaveInterval(fake_env, clazz, runtime.autosave_seconds);
  core.setAudioVolume(fake_env, clazz, runtime.volume);
  opensles_set_master_volume((unsigned)runtime.volume);
  core.setHingeStatus(fake_env, clazz, 0);
  core.setWhitenoiseFeed(fake_env, clazz, 0);

  drastic_config_calculate_layout(&runtime, panel_width, panel_height);
  if (!drastic_renderer_init(&runtime)) {
    const char *renderer_error = drastic_renderer_last_error();
    if (renderer_error && renderer_error[0])
      fatal_error("Could not initialize the %s renderer:\n%s",
                  drastic_renderer_backend_name(),
                  renderer_error);
    fatal_error("Could not initialize the %s renderer.",
                drastic_renderer_backend_name());
  }
  fatal_error_set_graphics_active(1);
  overlay_init(runtime.rotation);
  drastic_config_calculate_layout(&runtime, panel_width, panel_height);

  char prepared_rom_path[sizeof(runtime.rom_path)];
  snprintf(prepared_rom_path, sizeof(prepared_rom_path), "%s",
           runtime.rom_path);
  jboolean native_archive = has_archive_extension(runtime.rom_path);
  const char *extension = strrchr(runtime.rom_path, '.');
  if (extension && !strcasecmp(extension, ".zip")) {
    char archive_error[256];
    if (!drastic_zip_prepare(runtime.rom_path, prepared_rom_path,
                             sizeof(prepared_rom_path), archive_error,
                             sizeof(archive_error)))
      fatal_error("Could not open the ZIP game:\n%s\n\n%s",
                  runtime.rom_path, archive_error);
    native_archive = 0;
  }

  void *rom = jni_make_string(prepared_rom_path);
  const int rom_type = core.getRomType(fake_env, clazz, rom);
  if (rom_type <= 0)
    fatal_error("Drastic does not recognize this ROM:\n%s", runtime.rom_path);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  hidInitializeTouchScreen();
  hidInitializeMouse();
  vibration_player_ready = R_SUCCEEDED(hidInitializeVibrationDevices(
      vibration_player, 2, HidNpadIdType_No1, HidNpadStyleSet_NpadStandard));
  vibration_handheld_ready = R_SUCCEEDED(hidInitializeVibrationDevices(
      vibration_handheld, 2, HidNpadIdType_Handheld,
      HidNpadStyleSet_NpadStandard));

  RuntimeControls controls = {
    .state_slot = prefs_get_int("Wrapper/StateSlot", 0),
  };
  if (controls.state_slot < 0 || controls.state_slot > 9)
    controls.state_slot = 0;
  load_runtime_controls(&controls);

  DrasticMenuCore menu_core = {
    .env = fake_env,
    .clazz = clazz,
    .pause_system = core.pauseSystem,
    .save_state = core.saveState,
    .load_state = core.loadState,
    .is_saving = core.isSaving,
    .get_saving_slot = core.getSavingSlot,
    .get_snapshots = core.getSnapshots16,
    .get_snapshots_direct = core.getSnapshots16Direct,
    .reset_ds = core.resetDS,
    .apply_config = core.applyConfig,
    .set_audio_volume = core.setAudioVolume,
    .set_autosave_interval = core.setAutosaveInterval,
    .get_cheat_count = core.getCheatCount,
    .get_cheat_enabled = core.getCheatEnabled,
    .get_cheat_name = core.getCheatName,
    .get_cheat_note = core.getCheatNote,
    .get_cheat_folder_id = core.getCheatFolderId,
    .get_cheat_folder_count = core.getCheatFolderCount,
    .get_cheat_folder_multi_select = core.getCheatFolderMultiSelect,
    .get_cheat_folder_name = core.getCheatFolderName,
    .set_cheat_enabled = core.setCheatEnabled,
    .get_custom_cheat_count = core.getCustomCheatCount,
    .get_custom_cheat_enabled = core.getCustomCheatEnabled,
    .get_custom_cheat_name = core.getCustomCheatName,
    .set_custom_cheat_enabled = core.setCustomCheatEnabled,
    .add_custom_cheat = core.addCustomCheat,
    .remove_custom_cheat = core.removeCustomCheat,
    .update_cheats = core.updateCheats,
  };
  DrasticIngameMenu *menu = drastic_menu_create(
      &runtime, &menu_core, &controls.state_slot);
  if (!menu) fatal_error("Could not allocate the in-game menu.");

  CoreGameThread game = {
    .clazz = clazz,
    .rom = rom,
    .load_slot = -1,
    .config = (jlong)runtime.core_config,
    .clock = configured_start_clock(),
    .startup_mode = 0,
    .archive = native_archive,
  };
  const int game_thread_result = core_game_thread_start(&game);
  if (game_thread_result != 0)
    fatal_error("Could not create the Drastic emulation thread (%d).",
                game_thread_result);

  DrasticInputSamplerConfig input_config;
  configure_input_sampler(&input_config, &controls, clazz);
  DrasticInputSampler *input_sampler =
      drastic_input_sampler_create(&input_config);
  if (!input_sampler)
    fatal_error("Could not create the dedicated input sampler.");
  drastic_input_sampler_update_runtime(input_sampler, &runtime, true);

  AppletLifecycle lifecycle = {
    .clazz = clazz,
    .game = &game,
    .runtime = &runtime,
    .controls = &controls,
    .menu = menu,
    .input_sampler = input_sampler,
  };
  AppletHookCookie lifecycle_cookie = {0};
  int lifecycle_hooked = 0;
  appletHook(&lifecycle_cookie, applet_lifecycle_hook, &lifecycle);
  if (R_SUCCEEDED(appletSetFocusHandlingMode(
          AppletFocusHandlingMode_NoSuspend))) {
    lifecycle_hooked = 1;
  } else {
    appletUnhook(&lifecycle_cookie);
  }

  unsigned boot_frames = 0;
  int persisted_cheats_applied = 0;
  RuntimeHud hud = {0};
  while (!controls.exit_requested &&
         !__atomic_load_n(&game.finished, __ATOMIC_ACQUIRE)) {
    if (!appletMainLoop()) break;
    if (lifecycle.suspended) {
      svcSleepThread(16 * 1000 * 1000LL);
      continue;
    }
    if (lifecycle.resumed) {
      lifecycle.resumed = 0;
      reset_runtime_fps_window(&hud);
      controls.fast_forward = -1;
    }
    if (drastic_menu_is_open(menu)) {
      reset_runtime_fps_window(&hud);
      drastic_input_sampler_update_runtime(input_sampler, &runtime, false);
      DrasticInputSnapshot input;
      drastic_input_sampler_read(input_sampler, &input);
      drastic_menu_update(menu, input.buttons, input.buttons_down,
                          input.left, input.right);
      if (drastic_menu_take_exit_request(menu)) controls.exit_requested = 1;
      if (!drastic_menu_is_open(menu)) {
        /* Overlay changes rebuild DraStic's base config, which intentionally
         * excludes the transient fast-forward bit. Reconcile that bit on the
         * first resumed input sample even when a toggle remains latched. */
        controls.fast_forward = -1;
      }
      drastic_input_sampler_update_runtime(
          input_sampler, &runtime, !drastic_menu_is_open(menu));
      drastic_renderer_present(&runtime, core.renderFrame, fake_env, clazz,
                               overlay_frame(), false);
      continue;
    }
    /* The low 16 bits are Drastic's short transition counter, not a loading
     * state.  Android keeps drawing its transition textures while nonzero.
     * Keep our cached DS textures visible and input responsive for the same
     * interval.  The pthread bridge latches the core's edge-based screen-ready
     * notification so the first wait after the counter reaches zero cannot
     * miss the signal emitted during the transition. */
    const int frame_info = core.getFrameInfo
                               ? core.getFrameInfo(fake_env, clazz)
                               : 0;
    const unsigned transition_state = (unsigned)frame_info & 0xffffu;
    if (transition_state != 0) {
      const int open_menu = process_input(
          &runtime, &controls, clazz, menu, input_sampler);
      update_runtime_hud(&hud, &runtime, &controls, 0);
      drastic_renderer_present(&runtime, core.renderFrame, fake_env, clazz,
                               overlay_frame(), false);
      if (open_menu) drastic_menu_open(menu);
      svcSleepThread(16 * 1000 * 1000LL);
      continue;
    }
    /* Handle hotkeys and runtime configuration before entering Drastic's
     * waitScreen/renderFrame handshake. The sampler continues delivering raw
     * gameplay input independently while this render loop is blocked. */
    const int open_menu = process_input(
        &runtime, &controls, clazz, menu, input_sampler);
    pthr_capture_next_cond_wait_as_frame_sync();
    core.waitScreen(fake_env, clazz);
    if (__atomic_load_n(&game.finished, __ATOMIC_ACQUIRE))
      break;
    update_runtime_hud(&hud, &runtime, &controls, 1);
    drastic_renderer_present(&runtime, core.renderFrame, fake_env, clazz,
                             overlay_frame(), true);
    if (cpu_boost_active) {
      cpu_boost(0);
      cpu_boost_active = false;
    }
    if (!persisted_cheats_applied) {
      core.pauseSystem(fake_env, clazz, 1);
      drastic_menu_apply_persisted_cheats(menu);
      core.pauseSystem(fake_env, clazz, 0);
      persisted_cheats_applied = 1;
    }
    /* Complete the waitScreen/renderFrame pair before pauseSystem() opens the
     * menu; the request is local to this frame and needs no persistent state. */
    if (open_menu) drastic_menu_open(menu);
    boot_frames++;
  }
  if (lifecycle_hooked) {
    appletUnhook(&lifecycle_cookie);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
  }
  if (cpu_boost_active) cpu_boost(0);
  if (boot_frames == 0 &&
      __atomic_load_n(&game.finished, __ATOMIC_ACQUIRE) && !game.result)
    fatal_error("Drastic could not start:\n%s", runtime.rom_path);
  prefs_set_int("Wrapper/StateSlot", controls.state_slot);
  prefs_save();

  /* Match NetherSX2's chainload ordering: schedule the launcher while the
   * libnx environment is still intact, before core/JIT/runtime teardown. */
  if (controls.exit_requested && envHasNextLoad() && runtime.launcher_path[0])
    envSetNextLoad(runtime.launcher_path, runtime.launcher_path);

  drastic_input_sampler_destroy(input_sampler);
  HidVibrationValue stopped[2] = {0};
  send_rumble(stopped);
  drastic_menu_destroy(menu);
  drastic_renderer_detach_core();
  shutdown_core(clazz, &game);
  /* Stop DraStic-owned Android service workers while their code and any EGL
   * ownership they hold are still valid. hbloader reuses this process for the
   * launcher, so no libdrastic thread may survive the upcoming renderer/SO
   * teardown. */
  pthr_shutdown();
  opensles_shutdown();
  drastic_renderer_shutdown();
  libc_finalize_core();
  pthr_finalize();
  libc_memory_shutdown();
  so_unload(&emu_mod);
  switchStorageShutdown();
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
