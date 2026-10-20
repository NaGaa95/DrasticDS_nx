#include "prefs.h"
#include <assert.h>
#include <stdio.h>

static void check(const char *path, const char *text, bool expected) {
  FILE *file=fopen(path,"wb");assert(file);fputs(text,file);fclose(file);
  prefs_init(path);
  assert(prefs_get_bool("Wrapper/LowLatency",!expected)==expected);
  assert(!prefs_contains("Wrapper/Deko3DLowLatency"));
  assert(!prefs_contains("Wrapper/VulkanLowLatency"));
  /* Switching backend must keep the same user-facing preference. */
  prefs_set_string("Wrapper/Renderer","vk");prefs_save();prefs_init(path);
  assert(prefs_get_bool("Wrapper/LowLatency",!expected)==expected);
  prefs_set_string("Wrapper/Renderer","deko3d");prefs_save();prefs_init(path);
  assert(prefs_get_bool("Wrapper/LowLatency",!expected)==expected);
}

int main(int argc,char **argv) {
  assert(argc==2);
  check(argv[1],"",true);
  check(argv[1],"Wrapper/Renderer = deko3d\nWrapper/Deko3DLowLatency = false\nWrapper/VulkanLowLatency = true\n",false);
  check(argv[1],"Wrapper/Renderer = vk\nWrapper/Deko3DLowLatency = true\nWrapper/VulkanLowLatency = false\n",false);
  check(argv[1],"Wrapper/Renderer = vk\nWrapper/Deko3DLowLatency = false\nWrapper/VulkanLowLatency = true\n",true);
  check(argv[1],"Wrapper/Renderer = deko3d\nWrapper/Deko3DLowLatency = true\nWrapper/VulkanLowLatency = false\n",true);
  check(argv[1],"Wrapper/LowLatency = false\nWrapper/Deko3DLowLatency = true\nWrapper/VulkanLowLatency = true\n",false);
  check(argv[1],"Wrapper/LowLatency = true\nWrapper/Deko3DLowLatency = false\nWrapper/VulkanLowLatency = false\n",true);
  check(argv[1],"Wrapper/Renderer = deko3d\nWrapper/VulkanLowLatency = false\n",true);
  remove(argv[1]);
  puts("Low latency preferences passed: legacy migration, explicit choices, persistence and backend switching");
  return 0;
}
