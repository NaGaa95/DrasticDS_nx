#!/usr/bin/env python3
"""Run production background composition and launcher image/cache checks.

Requires a host clang/clang++, SDL2 + SDL2_image, and Pillow. The generated
fixture extracts the launcher's actual import/cache and atomic-save helpers;
only Horizon storage commits and profile access are replaced with host stubs.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "drastic_background.h"
#include "drastic_background_files.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(path,mode) _mkdir(path)
#define fsync _commit
#else
#include <unistd.h>
#endif
struct Choice { const char *label, *val; };
using Store=std::map<std::string,std::string>;
const char *CACHE_DIR,*BACKGROUNDS_DIR;
static bool validBackgroundCache(const std::string &p){return drastic_background_cache_valid(p.c_str());}
static const char *storeGet(Store &s,const char *k,const char *fallback) {
  auto i=s.find(k);return i==s.end()?fallback:i->second.c_str();
}
static void storeSet(Store &s,const char *k,const char *v){s[k]=v;}
static void storeRemove(Store &s,const char *k){s.erase(k);}
static std::string toEmu(const std::string &s){return s;}
static std::string fileNameOf(const std::string &s){return s.substr(s.find_last_of("/\\")+1);}
static std::string sanitize(std::string s){for(char &c:s)if(!isalnum((unsigned char)c))c='_';return s;}
static void fsdevCommitDevice(const char*){}
static bool queryRegularFile(const std::string &p,bool &exists){
  struct stat s{};exists=stat(p.c_str(),&s)==0;return !exists||S_ISREG(s.st_mode);
}
'''

TEST = r'''
int main(int argc,char **argv){
  assert(argc==2); const std::string root=argv[1];
  const std::string cache=root+"/cache",backgrounds=root+"/backgrounds";
  CACHE_DIR=cache.c_str();BACKGROUNDS_DIR=backgrounds.c_str();
  // Same CPU-only path as the emulator: no SDL_Init or image subsystem setup.
  Store settings;std::string error;
  for(const char *path:{"/switch/drastic/backgrounds/Bundled/DS Logo.png",
                        "sdmc:/switch/drastic/backgrounds/Bundled/DS Logo.png"}){
    settings["Wrapper/Background"]=path;settings["Wrapper/BackgroundCache"]="old.nxbg";
    assert(retireDSLogoBackground(settings)&&settings.at("Wrapper/Background").empty());
    assert(!settings.count("Wrapper/BackgroundCache")&&!retireDSLogoBackground(settings));
  }
  settings["Wrapper/Background"]="/switch/drastic/backgrounds/User/DS Logo.png";
  assert(!retireDSLogoBackground(settings));settings.clear();
  assert(prepareBackground(settings,error)&&settings.count("Wrapper/BackgroundCache")==0);
  settings["Wrapper/Background"]="builtin:navy";
  assert(prepareBackground(settings,error)&&settings.count("Wrapper/BackgroundCache")==0);
  const std::atomic_bool cancel{false};unsigned formats=0;
  for(const char *format:{"png","jpg","webp","bmp"}){
    const std::string source=root+"/input."+format;
    auto image=loadBackgroundImage(source,error);assert(image&&image->w==1920&&image->h==960);
    if(!strcmp(format,"png")||!strcmp(format,"webp"))
      assert((*(uint32_t*)image->pixels>>24)==128);
    std::string imported;assert(importBackground(source,imported,error,cancel));
    assert(imported.rfind(backgrounds+"/User/",0)==0);
    settings["Wrapper/Background"]=imported;
    assert(prepareBackground(settings,error));
    const std::string prepared=settings.at("Wrapper/BackgroundCache");
    assert(prepareBackground(settings,error,true));
    assert(validBackgroundCache(prepared));
    auto importedImage=loadBackgroundImage(imported,error);
    assert(importedImage&&importedImage->w==1920&&importedImage->h==960);
    // Unchanged input reuses the cache, including after the original USB/SMB file disappears.
    assert(prepareBackground(settings,error)&&settings.at("Wrapper/BackgroundCache")==prepared);
    assert(remove(source.c_str())==0);
    assert(prepareBackground(settings,error));
    FILE *file=fopen(prepared.c_str(),"wb");assert(file);fputs("broken",file);fclose(file);
    assert(!validBackgroundCache(prepared)&&prepareBackground(settings,error)&&validBackgroundCache(prepared));
    // Recover interrupted atomic replacement before reading the cache.
    assert(rename(prepared.c_str(),(prepared+".old").c_str())==0);
    file=fopen((prepared+".tmp").c_str(),"wb");assert(file);fputs("partial",file);fclose(file);
    assert(prepareBackground(settings,error)&&validBackgroundCache(prepared));
    formats++;
  }
  for(const char *name:{"invalid.png","large.png","too_many_pixels.png","missing.png"}){
    const std::string path=root+"/"+name;
    assert(!loadBackgroundImage(path,error));
    settings["Wrapper/Background"]=path;
    assert(!prepareBackground(settings,error)&&!settings.count("Wrapper/BackgroundCache"));
  }
  std::atomic_bool cancelled{true};std::string unused;
  assert(!importBackground(root+"/valid.png",unused,error,cancelled));
  // A skin's layout and rotation survive import to managed SD storage.
  const std::string presetSource=root+"/valid.png";
  FILE *layout=fopen((presetSource+".layout").c_str(),"wb");assert(layout);
  fputs("NXBGLAYOUT2 1\n0.1 0.2 0.3 0.4\n0.6 0.2 0.3 0.4\n",layout);fclose(layout);
  std::string imported;assert(importBackground(presetSource,imported,error,cancel));
  DrasticRuntimeConfig preset{};
  assert(drastic_background_apply_layout(&preset,imported.c_str()));
  assert(preset.layout==DRASTIC_LAYOUT_CUSTOM && preset.rotation==1 && preset.background_fit==2);
  assert(std::fabs(preset.custom_screens[0].x-0.1f)<0.00001f);
  layout=fopen((presetSource+".layout").c_str(),"wb");assert(layout);
  fputs("NXBGLAYOUT2 8\nnan 0 1 1\n0 0 1 1\n",layout);fclose(layout);
  assert(!drastic_background_apply_layout(&preset,presetSource.c_str()));
  assert(preset.rotation==1 && preset.custom_screens[0].x==0.1f);
  IMG_Quit();
  printf("Launcher background validation passed: %u formats, alpha, resizing, import persistence, cache recovery, rotated presets and malformed metadata\n",formats);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True, help='host clang++ or g++')
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    env = dict(os.environ)
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    source = (ROOT / 'launcher/source/main.cpp').read_text(encoding='utf-8')
    styles = source[source.index('static const Choice BACKGROUND_STYLES'):source.index('static std::string backgroundValue')]
    start = source.index('static bool writeAtomicText')
    atomic = source[source.index('static bool recoverAtomicFile(const std::string &path) {'):source.index('\nstatic ', start + 1)]
    helpers = source[source.index('using BackgroundSurface='):source.index('static void scanBackgrounds')]
    retirement = source[source.index('static bool retireDSLogoBackground'):source.index('static bool migrateLauncherSettings')]
    with tempfile.TemporaryDirectory(prefix='drastic-background-test-') as temporary:
        folder = Path(temporary)
        for extension in ('png', 'jpg', 'webp', 'bmp'):
            image = Image.new('RGBA', (2560, 1280), (20, 70, 150, 128))
            if extension == 'jpg':
                image = image.convert('RGB')
            image.save(folder / f'input.{extension}')
        Image.new('RGBA', (2, 2), (1, 2, 3, 128)).save(folder / 'valid.png')
        Image.new('RGB', (8193, 2)).save(folder / 'too_many_pixels.png')
        (folder / 'invalid.png').write_bytes(b'not an image')
        with (folder / 'large.png').open('wb') as file:
            file.truncate(32 * 1024 * 1024 + 1)
        fixture = folder / 'launcher.cpp'
        fixture.write_text(PRELUDE + styles + atomic + retirement + helpers + TEST, encoding='utf-8')
        executable = folder / 'launcher.exe'
        prefix = compiler.parent.parent
        c_compiler = compiler.with_name(compiler.name.replace('++', ''))
        shared = folder / 'background_files.o'
        subprocess.run([str(c_compiler), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DDRASTIC_BACKGROUND_TESTING',
                        '-I' + str(ROOT / 'source'), '-I' + str(prefix / 'include'),
                        '-c', str(ROOT / 'source/drastic_background_files.c'), '-o', str(shared)], check=True, env=env)
        subprocess.run([str(compiler), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'source'), '-I' + str(prefix / 'include'),
                        '-L' + str(prefix / 'lib'), str(fixture), str(shared), '-lSDL2_image', '-lSDL2',
                        '-o', str(executable)], check=True, env=env)
        subprocess.run([str(executable), str(folder)], check=True, env=env)
        executable = folder / 'cache.exe'
        subprocess.run([str(c_compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DDRASTIC_BACKGROUND_TESTING',
                        '-I' + str(prefix / 'include'), '-L' + str(prefix / 'lib'),
                        '-I' + str(ROOT / 'source'), str(ROOT / 'source/drastic_background.c'),
                        str(ROOT / 'tools/tests/background_cache_validation.c'), str(shared),
                        '-lSDL2_image', '-lSDL2', '-lm', '-o', str(executable)], check=True, env=env)
        subprocess.run([str(executable), str(folder / 'valid.png'), str(folder)], check=True, env=env)
        executable = folder / 'compose.exe'
        subprocess.run([str(c_compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(prefix / 'include'), '-L' + str(prefix / 'lib'),
                        '-I' + str(ROOT / 'source'), str(ROOT / 'source/drastic_background.c'),
                        str(ROOT / 'source/drastic_layout.c'),
                        str(ROOT / 'tools/tests/background_validation.c'), str(shared), '-lSDL2_image', '-lSDL2', '-lm',
                        '-o', str(executable)], check=True, env=env)
        subprocess.run([str(executable), str(folder / 'fixture.nxbg'), str(folder / 'valid.png'), str(folder)], check=True, env=env)
        executable = folder / 'prefs.exe'
        subprocess.run([str(c_compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'source'), str(ROOT / 'tools/tests/runtime_prefs_validation.c'),
                        '-o', str(executable)], check=True, env=env)
        subprocess.run([str(executable)], cwd=folder, check=True, env=env)


if __name__ == '__main__':
    main()
