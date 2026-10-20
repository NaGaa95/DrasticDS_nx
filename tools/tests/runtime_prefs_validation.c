/* Exercise the real persistence code in a temporary, contained profile root. */
#include "config.h"
#undef DATA_ROOT
#define DATA_ROOT "."
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#endif

static unsigned writes;
static FILE *counted_open(const char *path, const char *mode) {
  if (!strcmp(mode,"wb")) writes++;
  return fopen(path,mode);
}
#define fopen counted_open
#include "../../source/prefs.c"
#undef fopen

int main(void) {
  mkdir("gamecfg",0777);
  FILE *file=fopen("launch.ini","wb"); assert(file);
  fputs("Wrapper/GameConfigPath = ./gamecfg/title.ini\nWrapper/Renderer = deko3d\n",file);fclose(file);
  file=fopen("gamecfg/title.ini","wb");assert(file);
  fputs("# Keep this comment\nWrapper/Background = old.png\nWrapper/Background = duplicate.png\n"
        "Wrapper/Renderer = vk\nUnknown/Value = preserved\n",file);fclose(file);
  prefs_init("launch.ini");
  prefs_set_string("Wrapper/Background","new.png");
  prefs_set_string("Wrapper/BackgroundCache","private.nxbg");
  prefs_set_string("Wrapper/Layout","custom");
  prefs_set_int("Wrapper/Rotation",1);
  prefs_set_float("Wrapper/CustomTopX",0.125f);
  const char *keys[]={"Wrapper/Background","Wrapper/Layout","Wrapper/Rotation","Wrapper/CustomTopX",
                      "Missing/Key",NULL,"Wrapper/Background"};
  writes=0;
  prefs_save_runtime_keys(keys,sizeof(keys)/sizeof(*keys));
  assert(writes==2); /* One effective config and one profile write. */
  prefs_init("launch.ini");
  assert(!strcmp(prefs_get_string("Wrapper/Background",""),"new.png"));
  assert(!strcmp(prefs_get_string("Wrapper/BackgroundCache",""),"private.nxbg"));
  assert(!strcmp(prefs_get_string("Wrapper/Renderer",""),"deko3d"));
  file=fopen("gamecfg/title.ini","rb"); assert(file);
  char text[4096]={0};assert(fread(text,1,sizeof(text)-1,file)>0);fclose(file);
  assert(strstr(text,"# Keep this comment") && strstr(text,"Unknown/Value = preserved"));
  assert(!strstr(text,"duplicate.png") && !strstr(text,"BackgroundCache"));
  prefs_init("gamecfg/title.ini");
  assert(!strcmp(prefs_get_string("Wrapper/Background",""),"new.png"));
  assert(!strcmp(prefs_get_string("Wrapper/Renderer",""),"vk"));
  assert(!strcmp(prefs_get_string("Wrapper/Layout",""),"custom"));
  assert(prefs_get_int("Wrapper/Rotation",0)==1);
  assert(prefs_get_float("Wrapper/CustomTopX",0)==0.125f);
  prefs_init("launch.ini");
  prefs_set_string("Wrapper/GameConfigPath","./gamecfg/../forbidden.ini");
  prefs_set_int("Wrapper/Rotation",2);writes=0;
  prefs_save_runtime_key("Wrapper/Rotation");
  assert(writes==1 && !fopen("forbidden.ini","rb"));
  puts("Runtime preferences passed: one config/profile write per batch, per-title layout persistence, duplicate removal and unrelated settings preserved");
  return 0;
}
