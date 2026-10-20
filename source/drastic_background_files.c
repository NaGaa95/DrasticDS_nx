#include "drastic_background_files.h"
#include "drastic_background.h"
#include <SDL2/SDL_image.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(path,mode) _mkdir(path)
#define fsync _commit
#endif

static bool fail(char *error, size_t size, const char *message) {
  if (error && size) snprintf(error, size, "%s", message);
  return false;
}

#ifdef DRASTIC_BACKGROUND_TESTING
static SDL_atomic_t g_decodes, g_reads, g_writes;
#define BACKGROUND_COUNT(counter) SDL_AtomicAdd(&(counter),1)
#else
#define BACKGROUND_COUNT(counter) ((void)0)
#endif

SDL_Surface *drastic_background_image_load(const char *path, char *error, size_t size) {
  struct stat info;
  const char *message = "Could not prepare the background image.";
  if (!path || stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 1 ||
      (uint64_t)info.st_size > 32u*1024*1024) {
    fail(error,size,"Background image is missing or exceeds 32 MiB."); return NULL;
  }
  BACKGROUND_COUNT(g_decodes);
  SDL_Surface *source = IMG_Load(path);
  if (!source) { fail(error,size,"Could not decode the background image."); return NULL; }
  if (source->w <= 0 || source->h <= 0 || source->w > 8192 || source->h > 8192 ||
      (uint64_t)source->w*source->h > 16u*1024*1024) {
    message = "Background image dimensions exceed the supported limit.";
    SDL_FreeSurface(source); fail(error,size,message); return NULL;
  }
  const double scale = fmin(1.0, 1920.0 / fmax(source->w, source->h));
  int width = (int)lround(source->w * scale), height = (int)lround(source->h * scale);
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  SDL_Surface *image = SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_ARGB8888);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  if (!image || SDL_BlitScaled(source,NULL,image,NULL)) {
    SDL_FreeSurface(image); image = NULL; fail(error,size,message);
  }
  SDL_FreeSurface(source);
  return image;
}

bool drastic_background_cache_valid(const char *path) {
  if (!path || !*path) return false;
  FILE *file = fopen(path,"rb"); if (!file) return false;
  DrasticBackgroundHeader h; struct stat info;
  const bool valid = fread(&h,sizeof(h),1,file)==1 &&
      !memcmp(h.magic,DRASTIC_BACKGROUND_MAGIC,8) && h.width && h.height &&
      h.width<=DRASTIC_BACKGROUND_MAX_EDGE && h.height<=DRASTIC_BACKGROUND_MAX_EDGE &&
      !fstat(fileno(file),&info) &&
      (uint64_t)info.st_size==sizeof(h)+(uint64_t)h.width*h.height*4;
  fclose(file); return valid;
}

#define BACKGROUND_RAM_LIMIT (128u * 1024 * 1024)
typedef struct BackgroundImageEntry {
  DrasticBackgroundImage image;
  SDL_Surface *surface;
  char path[1024], cache_path[1024];
  int64_t size, modified, created;
  unsigned references;
  uint64_t used;
  struct BackgroundImageEntry *next;
} BackgroundImageEntry;

static BackgroundImageEntry *g_images;
static SDL_SpinLock g_images_lock;
static size_t g_image_bytes;
static uint64_t g_image_clock;

#ifdef DRASTIC_BACKGROUND_TESTING
DrasticBackgroundCacheStats drastic_background_cache_stats(void) {
  SDL_AtomicLock(&g_images_lock);
  const DrasticBackgroundCacheStats stats={g_image_bytes,SDL_AtomicGet(&g_decodes),
      SDL_AtomicGet(&g_reads),SDL_AtomicGet(&g_writes)};
  SDL_AtomicUnlock(&g_images_lock);
  return stats;
}
#endif

static const char *image_path(const char *path) {
  return path && !strncmp(path,"sdmc:",5) ? path+5 : path;
}

static size_t image_bytes(const BackgroundImageEntry *entry) {
  return (size_t)entry->image.width*entry->image.height*4;
}

static void free_image(BackgroundImageEntry *entry) {
  g_image_bytes-=image_bytes(entry);
  SDL_FreeSurface(entry->surface);
  free(entry);
}

/* The lock protects metadata only. Decoding and all storage access happen
 * outside it, so a worker can never hold up the menu on an SD operation. */
static BackgroundImageEntry *find_image(const char *path, const char *cache_path,
                                        const struct stat *info) {
  for (BackgroundImageEntry *entry=g_images;entry;entry=entry->next) {
    if (strcmp(entry->path,path)) continue;
    if (info ? entry->size==info->st_size && entry->modified==info->st_mtime &&
               entry->created==info->st_ctime :
               cache_path && *cache_path && !strcmp(entry->cache_path,cache_path)) {
      entry->references++;
      entry->used=++g_image_clock;
      if (cache_path && *cache_path)
        snprintf(entry->cache_path,sizeof(entry->cache_path),"%s",cache_path);
      return entry;
    }
  }
  return NULL;
}

static void trim_images(size_t required) {
  while (g_image_bytes+required>BACKGROUND_RAM_LIMIT) {
    BackgroundImageEntry **oldest=NULL;
    for (BackgroundImageEntry **p=&g_images;*p;p=&(*p)->next)
      if (!(*p)->references && (!oldest || (*p)->used<(*oldest)->used)) oldest=p;
    if (!oldest) break; /* Referenced images must remain usable. */
    BackgroundImageEntry *entry=*oldest;
    *oldest=entry->next;
    free_image(entry);
  }
}

static SDL_Surface *read_cached_image(const char *path) {
  FILE *file=path && *path ? fopen(path,"rb") : NULL;
  if (!file) return NULL;
  DrasticBackgroundHeader h;
  struct stat info;
  SDL_Surface *image=NULL;
  if (fread(&h,sizeof(h),1,file)==1 && !memcmp(h.magic,DRASTIC_BACKGROUND_MAGIC,8) &&
      h.width && h.height && h.width<=DRASTIC_BACKGROUND_MAX_EDGE &&
      h.height<=DRASTIC_BACKGROUND_MAX_EDGE && !fstat(fileno(file),&info) &&
      (uint64_t)info.st_size==sizeof(h)+(uint64_t)h.width*h.height*4) {
    image=SDL_CreateRGBSurfaceWithFormat(0,h.width,h.height,32,SDL_PIXELFORMAT_ARGB8888);
    const size_t bytes=(size_t)h.width*h.height*4;
    BACKGROUND_COUNT(g_reads);
    if (image && (fread(image->pixels,1,bytes,file)!=bytes || ferror(file))) {
      SDL_FreeSurface(image); image=NULL;
    }
  }
  fclose(file);
  return image;
}

const DrasticBackgroundImage *drastic_background_image_acquire(
    const char *path, const char *cache_path, char *error, size_t size) {
  path=image_path(path);
  cache_path=image_path(cache_path);
  if (!path || strlen(path)>=1024) { fail(error,size,"Could not prepare the background image."); return NULL; }
  struct stat info;
  const bool exists=stat(path,&info)==0 && S_ISREG(info.st_mode);
  SDL_AtomicLock(&g_images_lock);
  BackgroundImageEntry *entry=find_image(path,cache_path,exists?&info:NULL);
  SDL_AtomicUnlock(&g_images_lock);
  if (entry) return &entry->image;
  SDL_Surface *surface=read_cached_image(cache_path);
  if (!surface) surface=drastic_background_image_load(path,error,size);
  if (!surface) return NULL;
  BackgroundImageEntry *loaded=calloc(1,sizeof(*loaded));
  if (!loaded) { SDL_FreeSurface(surface); fail(error,size,"Could not prepare the background image."); return NULL; }
  loaded->surface=surface;
  loaded->image=(DrasticBackgroundImage){surface->pixels,(unsigned)surface->w,(unsigned)surface->h};
  snprintf(loaded->path,sizeof(loaded->path),"%s",path);
  if (cache_path) snprintf(loaded->cache_path,sizeof(loaded->cache_path),"%s",cache_path);
  if (exists) {
    loaded->size=info.st_size; loaded->modified=info.st_mtime; loaded->created=info.st_ctime;
  }
  loaded->references=1;
  SDL_AtomicLock(&g_images_lock);
  entry=find_image(path,cache_path,exists?&info:NULL);
  if (!entry) {
    trim_images(image_bytes(loaded));
    loaded->used=++g_image_clock;
    loaded->next=g_images; g_images=loaded;
    g_image_bytes+=image_bytes(loaded);
    entry=loaded; loaded=NULL;
  }
  SDL_AtomicUnlock(&g_images_lock);
  if (loaded) { SDL_FreeSurface(loaded->surface); free(loaded); }
  return &entry->image;
}

const DrasticBackgroundImage *drastic_background_image_retain(const DrasticBackgroundImage *image) {
  if (image) {
    SDL_AtomicLock(&g_images_lock);
    ((BackgroundImageEntry*)image)->references++;
    SDL_AtomicUnlock(&g_images_lock);
  }
  return image;
}

void drastic_background_image_release(const DrasticBackgroundImage *image) {
  if (!image) return;
  SDL_AtomicLock(&g_images_lock);
  ((BackgroundImageEntry*)image)->references--;
  trim_images(0);
  SDL_AtomicUnlock(&g_images_lock);
}

void drastic_background_cache_clear(void) {
  SDL_AtomicLock(&g_images_lock);
  BackgroundImageEntry **p=&g_images;
  while (*p) {
    BackgroundImageEntry *entry=*p;
    if (entry->references) p=&entry->next;
    else { *p=entry->next; free_image(entry); }
  }
  SDL_AtomicUnlock(&g_images_lock);
}

static bool prepared_path(const char *path, const char *directory, char *output, size_t output_size) {
  struct stat info;
  if (!path || stat(path,&info) || !S_ISREG(info.st_mode) || info.st_size<1 ||
      (uint64_t)info.st_size>32u*1024*1024)
    return false;
  /* A content hash reread every PNG at boot and on every preview. Changes to
   * the path, size or timestamps now invalidate the cache with one stat. */
  uint64_t hash = 14695981039346656037ULL;
  const char *normalized=image_path(path);
  for (const unsigned char *p=(const unsigned char*)normalized;*p;p++) { hash^=*p; hash*=1099511628211ULL; }
  const int64_t metadata[]={info.st_size,info.st_mtime,info.st_ctime};
  for (size_t i=0;i<sizeof(metadata);i++) { hash^=((const unsigned char*)metadata)[i]; hash*=1099511628211ULL; }
  const int length=snprintf(output,output_size,"%s/v2-%016llx.nxbg",directory,(unsigned long long)hash);
  return length>=0 && (size_t)length<output_size;
}

bool drastic_background_find_prepared(const char *path, const char *directory, char *output, size_t size) {
  return prepared_path(path,directory,output,size) && drastic_background_cache_valid(output);
}

bool drastic_background_prepare_image(const char *path, const char *directory,
                                      char *output, size_t output_size,
                                      char *error, size_t error_size) {
  if (!prepared_path(path,directory,output,output_size))
    return fail(error,error_size,"Background image is missing or exceeds 32 MiB.");
  mkdir(directory,0777);
  char temporary[1200],old[1200];
  if (snprintf(temporary,sizeof(temporary),"%s.tmp",output)>=(int)sizeof(temporary) ||
      snprintf(old,sizeof(old),"%s.old",output)>=(int)sizeof(old))
    return fail(error,error_size,"Could not save the background image.");
  /* Recover the same replacement protocol used by launcher imports. */
  struct stat info;
  if (stat(output,&info) && !stat(old,&info) && rename(old,output))
    return fail(error,error_size,"Could not save the background image.");
  remove(temporary);
  if (drastic_background_cache_valid(output)) { remove(old); return true; }
  const DrasticBackgroundImage *image=drastic_background_image_acquire(path,NULL,error,error_size);
  if (!image) return false;
  FILE *file=fopen(temporary,"wb");
  if (!file) { drastic_background_image_release(image); return fail(error,error_size,"Could not save the background image."); }
  const DrasticBackgroundHeader header={DRASTIC_BACKGROUND_MAGIC,image->width,image->height};
  BACKGROUND_COUNT(g_writes);
  /* Large sequential writes avoid a stdio/SD call for every scanline. */
  setvbuf(file,NULL,_IOFBF,64*1024);
  const size_t bytes=(size_t)image->width*image->height*4;
  bool ok=fwrite(&header,sizeof(header),1,file)==1 && fwrite(image->pixels,1,bytes,file)==bytes;
  drastic_background_image_release(image);
  if (fflush(file) || fsync(fileno(file))) ok=false;
  if (fclose(file)) ok=false;
  const bool exists=stat(output,&info)==0;
  if (!ok || !drastic_background_cache_valid(temporary)) goto failure;
  remove(old);
  if (exists && rename(output,old)) goto failure;
  if (rename(temporary,output)) {
    if (exists) rename(old,output);
    goto failure;
  }
  remove(old);
  return true;
failure:
  remove(temporary);
  return fail(error,error_size,"Could not save the background image.");
}

static void scan_directory(const char *root, const char *directory, unsigned depth,
                            DrasticBackgroundEntry *entries, size_t capacity, size_t *count) {
  if (depth>4 || *count>=capacity) return;
  DIR *dir=opendir(directory); if (!dir) return;
  struct dirent *entry;
  while ((entry=readdir(dir)) && *count<capacity) {
    if (entry->d_name[0]=='.') continue;
    char path[1024];
    if (snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name)>=(int)sizeof(path)) continue;
    struct stat info;
    if (stat(path,&info)) continue;
    if (S_ISDIR(info.st_mode)) scan_directory(root,path,depth+1,entries,capacity,count);
    else if (S_ISREG(info.st_mode)) {
      const char *ext=strrchr(entry->d_name,'.');
      if (!ext || (strcasecmp(ext,".png") && strcasecmp(ext,".jpg") &&
          strcasecmp(ext,".jpeg") && strcasecmp(ext,".webp") && strcasecmp(ext,".bmp"))) continue;
      snprintf(entries[*count].path,sizeof(entries[*count].path),"%s",path);
      snprintf(entries[*count].name,sizeof(entries[*count].name),"%s",path+strlen(root)+1);
      ++*count;
    }
  }
  closedir(dir);
}

static int compare_entries(const void *left, const void *right) {
  return strcasecmp(((const DrasticBackgroundEntry*)left)->path,((const DrasticBackgroundEntry*)right)->path);
}

size_t drastic_background_scan(const char *directory, DrasticBackgroundEntry *entries, size_t capacity) {
  size_t count=0;
  if (!directory || !entries || !capacity) return 0;
  scan_directory(directory,directory,0,entries,capacity,&count);
  qsort(entries,count,sizeof(*entries),compare_entries);
  return count;
}

bool drastic_background_apply_layout(DrasticRuntimeConfig *config, const char *path) {
  char name[1200],magic[32],extra;
  if (!config || !path || snprintf(name,sizeof(name),"%s.layout",path)>=(int)sizeof(name)) return false;
  FILE *file=fopen(name,"r"); if (!file) return false;
  float values[8];
  int rotation=0;
  bool valid=fscanf(file,"%31s",magic)==1;
  if (valid && !strcmp(magic,"NXBGLAYOUT2"))
    valid=fscanf(file,"%d",&rotation)==1 && rotation>=0 && rotation<=3;
  else valid=valid && !strcmp(magic,"NXBGLAYOUT1");
  for (int i=0;valid && i<8;i++)
    valid=fscanf(file,"%f",&values[i])==1 && isfinite(values[i]) && values[i]>=0 && values[i]<=1;
  if (valid) valid=fscanf(file," %c",&extra)==EOF;
  fclose(file);
  for (int i=0;valid && i<2;i++) {
    const float *v=values+i*4;
    valid=v[2]>=0.01f && v[3]>=0.01f && v[0]+v[2]<=1.000001f && v[1]+v[3]<=1.000001f;
  }
  if (!valid) return false;
  for (int i=0;i<2;i++) {
    const float *v=values+i*4;
    config->custom_screens[i]=(DrasticScreenRect){v[0],v[1],v[2],v[3],i,i};
  }
  config->layout=DRASTIC_LAYOUT_CUSTOM;
  config->custom_aspect_lock=0;
  config->rotation=rotation;
  config->background_fit=2;
  return true;
}

struct DrasticBackgroundLoader {
  SDL_Thread *thread;
  SDL_mutex *mutex;
  SDL_cond *condition;
  char directory[1024], error[192];
  DrasticRuntimeConfig request, result;
  const DrasticBackgroundImage *image;
  uint64_t serial;
  bool stop, pending;
  int status;
};

static bool builtin_background(const char *path) {
  return !*path || !strcmp(path,"builtin:white") || !strcmp(path,"builtin:pink") ||
      !strcmp(path,"builtin:navy") || !strcmp(path,"builtin:black");
}

static int background_loader_thread(void *opaque) {
  DrasticBackgroundLoader *loader=opaque;
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
  SDL_LockMutex(loader->mutex);
  while (!loader->stop) {
    while (!loader->stop && !loader->pending) SDL_CondWait(loader->condition,loader->mutex);
    if (loader->stop) break;
    const uint64_t serial=loader->serial;
    DrasticRuntimeConfig config=loader->request;
    loader->pending=false;
    SDL_UnlockMutex(loader->mutex);
    char error[192]={0};
    const DrasticBackgroundImage *image=NULL;
    bool ok=true;
    if (!builtin_background(config.background)) {
      if (loader->directory[0])
        ok=drastic_background_prepare_image(config.background,loader->directory,
            config.background_cache,sizeof(config.background_cache),error,sizeof(error));
      if (ok) {
        image=drastic_background_image_acquire(config.background,config.background_cache,error,sizeof(error));
        ok=image!=NULL;
      }
      if (ok) drastic_background_apply_layout(&config,config.background);
    }
    SDL_LockMutex(loader->mutex);
    if (!loader->stop && serial==loader->serial) {
      loader->result=config;
      loader->image=image; image=NULL;
      snprintf(loader->error,sizeof(loader->error),"%s",error);
      loader->status=ok?1:-1;
    }
    drastic_background_image_release(image);
  }
  SDL_UnlockMutex(loader->mutex);
  return 0;
}

DrasticBackgroundLoader *drastic_background_loader_create(const char *directory) {
  DrasticBackgroundLoader *loader=calloc(1,sizeof(*loader));
  if (!loader) return NULL;
  if (directory) snprintf(loader->directory,sizeof(loader->directory),"%s",directory);
  loader->mutex=SDL_CreateMutex();
  loader->condition=SDL_CreateCond();
  if (loader->mutex && loader->condition)
    loader->thread=SDL_CreateThread(background_loader_thread,"bezel-loader",loader);
  if (!loader->thread) { drastic_background_loader_destroy(loader); return NULL; }
  return loader;
}

void drastic_background_loader_request(DrasticBackgroundLoader *loader,
    const DrasticRuntimeConfig *config, const char *path) {
  if (!loader || !config || !path) return;
  SDL_LockMutex(loader->mutex);
  ++loader->serial;
  drastic_background_image_release(loader->image); loader->image=NULL;
  loader->request=*config;
  snprintf(loader->request.background,sizeof(loader->request.background),"%s",path);
  loader->request.background_cache[0]=0;
  loader->error[0]=0;
  loader->status=0;
  loader->pending=true;
  /* None and procedural bezels need no worker or file access. */
  if (builtin_background(path)) {
    loader->result=loader->request;
    loader->status=1; loader->pending=false;
  }
  if (strlen(path)>=sizeof(loader->request.background)) {
    snprintf(loader->error,sizeof(loader->error),"Could not prepare the background image.");
    loader->status=-1; loader->pending=false;
  }
  SDL_CondSignal(loader->condition);
  SDL_UnlockMutex(loader->mutex);
}

int drastic_background_loader_poll(DrasticBackgroundLoader *loader,
    DrasticRuntimeConfig *config, const DrasticBackgroundImage **image, char *error, size_t size) {
  if (!loader) return -1;
  SDL_LockMutex(loader->mutex);
  const int status=loader->status;
  if (status) {
    if (config) *config=loader->result;
    if (image) { *image=loader->image; loader->image=NULL; }
    if (error && size) snprintf(error,size,"%s",loader->error);
    loader->status=0;
  }
  SDL_UnlockMutex(loader->mutex);
  return status;
}

void drastic_background_loader_cancel(DrasticBackgroundLoader *loader) {
  if (!loader) return;
  SDL_LockMutex(loader->mutex);
  ++loader->serial; loader->pending=false; loader->status=0;
  drastic_background_image_release(loader->image); loader->image=NULL;
  SDL_UnlockMutex(loader->mutex);
}

void drastic_background_loader_destroy(DrasticBackgroundLoader *loader) {
  if (!loader) return;
  if (loader->thread) {
    SDL_LockMutex(loader->mutex);
    loader->stop=true;
    SDL_CondSignal(loader->condition);
    SDL_UnlockMutex(loader->mutex);
    SDL_WaitThread(loader->thread,NULL);
  }
  drastic_background_image_release(loader->image);
  if (loader->condition) SDL_DestroyCond(loader->condition);
  if (loader->mutex) SDL_DestroyMutex(loader->mutex);
  free(loader);
}
