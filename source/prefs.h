/* Flat preference store shared by the Drastic launcher and native core host. */

#ifndef __PREFS_H__
#define __PREFS_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void prefs_init(const char *ini_path);

// flush the current map back to the ini file
void prefs_save(void);
/* Saves the effective launch config and mirrors one runtime-adjustable value
 * into the active per-title profile, when the launcher supplied one. */
void prefs_save_runtime_key(const char *key);
/* Writes the launch config and per-title profile once for a related edit. */
void prefs_save_runtime_keys(const char *const *keys, size_t count);

// lookups: return the stored value, or `def` on miss. `key` is the flat
// "Section/Key" string the SharedPreferences shim passes through.
bool        prefs_get_bool  (const char *key, bool def);
int         prefs_get_int   (const char *key, int def);
int64_t     prefs_get_int64 (const char *key, int64_t def);
float       prefs_get_float (const char *key, float def);
const char *prefs_get_string(const char *key, const char *def);
bool        prefs_contains  (const char *key);

// setters (Editor.put* / Set*Value path). Values are stored as strings.
void prefs_set_bool  (const char *key, bool v);
void prefs_set_int   (const char *key, int v);
void prefs_set_float (const char *key, float v);
void prefs_set_string(const char *key, const char *v);
void prefs_remove    (const char *key);

// Compatibility alias used by the inherited launcher-facing host code.
void prefs_set_disc_path(const char *path);

#endif
