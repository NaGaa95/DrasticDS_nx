#define SDL_MAIN_HANDLED
#include "drastic_background.h"
#include "drastic_background_files.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wait_result(DrasticBackgroundLoader *loader, DrasticRuntimeConfig *config,
                       const DrasticBackgroundImage **image) {
  char error[192];
  int status=0;
  for (unsigned i=0;i<10000 && !status;i++) {
    status=drastic_background_loader_poll(loader,config,image,error,sizeof(error));
    if (!status) SDL_Delay(1);
  }
  assert(status);
  return status;
}

static void write_raw(const char *path) {
  FILE *file=fopen(path,"wb"); assert(file);
  const DrasticBackgroundHeader header={DRASTIC_BACKGROUND_MAGIC,1920,1920};
  assert(fwrite(&header,sizeof(header),1,file)==1);
  assert(fseek(file,(long)sizeof(header)+1920L*1920*4-1,SEEK_SET)==0);
  assert(fputc(0,file)!=EOF && fclose(file)==0);
}

int main(int argc, char **argv) {
  assert(argc==3);
  char error[192],prepared[1024],warm[1024];
  /* No SDL_Init is needed in the native Deko3D process. */
  assert(drastic_background_prepare_image(argv[1],argv[2],prepared,sizeof(prepared),error,sizeof(error)));
  const DrasticBackgroundImage *image=drastic_background_image_acquire(argv[1],prepared,error,sizeof(error));
  assert(image && image->width==2 && image->height==2);
  DrasticBackgroundCacheStats before=drastic_background_cache_stats();
  assert(before.decodes==1 && before.reads==0 && before.writes==1);
  for (int i=0;i<100;i++) {
    assert(drastic_background_find_prepared(argv[1],argv[2],warm,sizeof(warm)) && !strcmp(warm,prepared));
    assert(drastic_background_prepare_image(argv[1],argv[2],warm,sizeof(warm),error,sizeof(error)));
    const DrasticBackgroundImage *again=drastic_background_image_acquire(argv[1],warm,error,sizeof(error));
    assert(again==image);
    drastic_background_image_release(again);
  }
  DrasticBackgroundCacheStats after=drastic_background_cache_stats();
  assert(after.decodes==before.decodes && after.reads==before.reads && after.writes==before.writes);

  DrasticRuntimeConfig config={0},ready;
  strcpy(config.background,argv[1]); strcpy(config.background_cache,prepared);
  assert(drastic_background_set_cached_image(&config,image));
  const DrasticBackgroundImage *backup=drastic_background_acquire_current_image();
  assert(backup==image);
  drastic_background_image_release(image);
  /* Eviction never invalidates active images or the pinned Cancel target. */
  char raw[1024],missing[1024];
  for (int i=0;i<12;i++) {
    snprintf(raw,sizeof(raw),"%s/pressure-%d.nxbg",argv[2],i);
    snprintf(missing,sizeof(missing),"missing-%d.png",i);
    write_raw(raw);
    const DrasticBackgroundImage *large=drastic_background_image_acquire(missing,raw,error,sizeof(error));
    assert(large && large->width==1920 && large->height==1920);
    drastic_background_image_release(large);
    assert(remove(raw)==0);
    assert(drastic_background_cache_stats().bytes<=128u*1024*1024);
  }
  drastic_background_cache_clear();
  assert(drastic_background_cache_stats().bytes==16);
  assert(backup->pixels[0]==0x80010203);
  assert(drastic_background_set_cached_image(&config,backup));
  drastic_background_image_release(backup);
  drastic_background_shutdown();
  drastic_background_cache_clear();
  before=drastic_background_cache_stats();
  image=drastic_background_image_acquire(argv[1],prepared,error,sizeof(error));
  after=drastic_background_cache_stats();
  assert(image && after.decodes==before.decodes && after.reads==before.reads+1);
  drastic_background_image_release(image);

  DrasticBackgroundLoader *loader=drastic_background_loader_create(argv[2]); assert(loader);
  for (int i=0;i<200;i++) {
    drastic_background_loader_request(loader,&config,i&1?argv[1]:"missing.png");
    if (i%3==0) drastic_background_loader_cancel(loader);
  }
  drastic_background_loader_request(loader,&config,argv[1]);
  image=NULL;
  assert(wait_result(loader,&ready,&image)>0 && !strcmp(ready.background,argv[1]) && image);
  drastic_background_image_release(image);
  /* Cancel discards a result even if its worker finishes after cancellation. */
  drastic_background_loader_request(loader,&config,argv[1]);
  drastic_background_loader_cancel(loader);
  SDL_Delay(20);
  assert(drastic_background_loader_poll(loader,&ready,&image,error,sizeof(error))==0);
  drastic_background_loader_request(loader,&config,"builtin:pink");
  image=NULL;
  assert(drastic_background_loader_poll(loader,&ready,&image,error,sizeof(error))==1);
  assert(!image && !strcmp(ready.background,"builtin:pink"));
  drastic_background_loader_request(loader,&config,"builtin:invalid");
  assert(wait_result(loader,&ready,&image)<0);
  drastic_background_loader_destroy(loader);
  /* Preview mode reads neither a raw file nor writes one. */
  loader=drastic_background_loader_create(NULL); assert(loader);
  before=drastic_background_cache_stats();
  drastic_background_loader_request(loader,&config,argv[1]);
  image=NULL;
  assert(wait_result(loader,&ready,&image)>0 && !ready.background_cache[0]);
  drastic_background_image_release(image);
  after=drastic_background_cache_stats();
  assert(after.decodes==before.decodes && after.reads==before.reads && after.writes==before.writes);
  drastic_background_loader_destroy(loader);
  drastic_background_cache_clear();
  assert(drastic_background_cache_stats().bytes==0);
  /* Replacing an image at the same path invalidates RAM and disk caches. */
  snprintf(raw,sizeof(raw),"%s/changed.bmp",argv[2]);
  for (int width=3;width<=4;width++) {
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,width,2,32,SDL_PIXELFORMAT_ARGB8888);
    assert(surface);SDL_FillRect(surface,NULL,0xff123456);
    assert(SDL_SaveBMP(surface,raw)==0);SDL_FreeSurface(surface);
    assert(drastic_background_prepare_image(raw,argv[2],warm,sizeof(warm),error,sizeof(error)));
    if (width==3) strcpy(prepared,warm);
    else assert(strcmp(prepared,warm));
    image=drastic_background_image_acquire(raw,warm,error,sizeof(error));
    assert(image && image->width==(unsigned)width && image->pixels[0]==0xff123456);
    drastic_background_image_release(image);
  }
  drastic_background_cache_clear();
  assert(drastic_background_cache_stats().bytes==0);
  puts("Background cache validation passed: 100 warm selections without decoding/pixel I/O/writes, 128 MiB eviction, pinned rollback, raw-cache boot, 200 queued selections, cancellation, RAM-only previews and file replacement");
  return 0;
}
