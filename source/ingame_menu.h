#ifndef DRASTIC_NX_INGAME_MENU_H
#define DRASTIC_NX_INGAME_MENU_H

#include <switch.h>
#include <stdbool.h>
#include <stdint.h>

#include "drastic_config.h"

typedef unsigned char DrasticJBoolean;
typedef long long DrasticJLong;

typedef struct {
  void *env;
  void *clazz;
  void (*pause_system)(void *, void *, int);
  void (*set_hinge_status)(void *, void *, DrasticJBoolean);
  DrasticJBoolean (*save_state)(void *, void *, int, DrasticJBoolean);
  DrasticJBoolean (*load_state)(void *, void *, int);
  DrasticJBoolean (*is_saving)(void *, void *);
  int (*get_saving_slot)(void *, void *);
  void (*get_snapshots)(void *, void *, int, void *, void *);
  void (*get_snapshots_direct)(void *, void *, void *, void *, void *);
  void (*reset_ds)(void *, void *);
  void (*apply_config)(void *, void *, DrasticJLong);
  void (*set_audio_volume)(void *, void *, int);
  void (*set_autosave_interval)(void *, void *, int);

  int (*get_cheat_count)(void *, void *);
  DrasticJBoolean (*get_cheat_enabled)(void *, void *, int);
  void *(*get_cheat_name)(void *, void *, int);
  void *(*get_cheat_note)(void *, void *, int);
  int (*get_cheat_folder_id)(void *, void *, int);
  int (*get_cheat_folder_count)(void *, void *);
  DrasticJBoolean (*get_cheat_folder_multi_select)(void *, void *, int);
  void *(*get_cheat_folder_name)(void *, void *, int);
  void (*set_cheat_enabled)(void *, void *, int, DrasticJBoolean);
  int (*get_custom_cheat_count)(void *, void *);
  DrasticJBoolean (*get_custom_cheat_enabled)(void *, void *, int);
  void *(*get_custom_cheat_name)(void *, void *, int);
  void (*set_custom_cheat_enabled)(void *, void *, int, DrasticJBoolean);
  int (*add_custom_cheat)(void *, void *, void *, void *, int,
                          DrasticJBoolean);
  void (*remove_custom_cheat)(void *, void *, int);
  void (*update_cheats)(void *, void *, DrasticJBoolean);
} DrasticMenuCore;

typedef struct DrasticIngameMenu DrasticIngameMenu;

DrasticIngameMenu *drastic_menu_create(DrasticRuntimeConfig *config,
                                       const DrasticMenuCore *core,
                                       int *state_slot);
void drastic_menu_destroy(DrasticIngameMenu *menu);
void drastic_menu_open(DrasticIngameMenu *menu);
void drastic_menu_close(DrasticIngameMenu *menu, bool resume_core);
bool drastic_menu_is_open(const DrasticIngameMenu *menu);
void drastic_menu_toggle_lid(DrasticIngameMenu *menu);
void drastic_menu_apply_persisted_cheats(DrasticIngameMenu *menu);
void drastic_menu_update(DrasticIngameMenu *menu, u64 held, u64 pressed,
                         HidAnalogStickState left,
                         HidAnalogStickState right);
bool drastic_menu_take_exit_request(DrasticIngameMenu *menu);

#endif
