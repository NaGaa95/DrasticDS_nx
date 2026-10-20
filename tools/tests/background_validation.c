#include "drastic_background.h"
#include "drastic_rotation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *cache;
static uint32_t ui_pixels[DRASTIC_OVERLAY_PIXELS];

static void write_cache(unsigned width, unsigned height, int truncate) {
  FILE *file = fopen(cache, "wb"); assert(file);
  DrasticBackgroundHeader header = {DRASTIC_BACKGROUND_MAGIC, width, height};
  assert(fwrite(&header, sizeof(header), 1, file) == 1);
  if (!truncate) {
    const uint32_t pixels[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0x80ffffff};
    assert(fwrite(pixels, sizeof(pixels), 1, file) == 1);
  }
  assert(fclose(file) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 4); cache = argv[1];
  DrasticRuntimeConfig config = {0};
  DrasticOverlayFrame ui = {ui_pixels, 1280, 720, 1, false};
  drastic_background_init(&config);
  assert(drastic_background_compose(&config,1280,720,&ui)->pixels == ui.pixels);
  strcpy(config.background, "test.png");
  snprintf(config.background_cache, sizeof(config.background_cache), "%s", cache);
  const unsigned bad_sizes[][2] = {{0,2}, {UINT32_MAX,UINT32_MAX}, {1921,1}, {2,2}};
  for(unsigned i=0;i<4;i++) {
    write_cache(bad_sizes[i][0],bad_sizes[i][1],1);
    drastic_background_init(&config);
    assert(drastic_background_compose(&config,1280,720,&ui)->pixels == ui.pixels);
  }
  write_cache(2,2,0);
  unsigned cases = 0;
  for(int style=0;style<5;style++) for(int rotation=0;rotation<4;rotation++)
  for(int docked=0;docked<2;docked++) for(int fit=0;fit<3;fit++) {
    const char *styles[] = {"test.png","builtin:white","builtin:pink","builtin:navy","builtin:black"};
    strcpy(config.background,styles[style]);
    config.rotation=rotation; config.background_fit=fit;
    const int pw=docked?1920:1280, ph=docked?1080:720;
    drastic_background_init(&config);
    ui.width=rotation&1?720:1280;ui.height=rotation&1?1280:720;
    ui.visible=false;
    for(int count=1;count<=3;count++) {
      config.screen_count=count;
      for(int i=0;i<count;i++)config.screens[i]=(DrasticScreenRect){
        pw*(0.07f+i*0.29f), ph*(0.12f+i*0.19f), pw*0.25f, ph*0.25f, i&1, i&1};
      const DrasticOverlayFrame *frame=drastic_background_compose(&config,pw,ph,&ui);
      assert(frame!=&ui && frame->visible && frame->width==ui.width && frame->height==ui.height);
      const uint64_t generation=frame->generation;
      const uint32_t *pixels=frame->pixels;
      /* Every game pixel stays transparent, including all three hybrid rectangles. */
      for(int y=0;y<frame->height;y++) for(int x=0;x<frame->width;x++) {
        float u,v;
        drastic_rotation_source_to_display(rotation,(x+0.5f)/frame->width,(y+0.5f)/frame->height,&u,&v);
        bool masked=false;
        for(int i=0;i<count;i++) {
          const DrasticScreenRect *r=&config.screens[i];
          if(u*pw>=r->x && u*pw<r->x+r->width && v*ph>=r->y && v*ph<r->y+r->height)
            assert(pixels[y*frame->width+x]==0);
          const float guard=(float)pw/DRASTIC_OVERLAY_WIDTH;
          if(u*pw>=r->x-guard && u*pw<r->x+r->width+guard &&
              v*ph>=r->y-guard && v*ph<r->y+r->height+guard) masked=true;
        }
        if (!style) {
          /* Compare the optimized scanline path with the original scalar
           * sampling/masking rule, including rotated rectangle boundaries. */
          float ax=frame->width/2.0f,ay=frame->height/2.0f;
          if (fit!=2) ax=ay=fit==1?fmaxf(ax,ay):fminf(ax,ay);
          const float sx=(x+0.5f-(frame->width-2*ax)*0.5f)/ax;
          const float sy=(y+0.5f-(frame->height-2*ay)*0.5f)/ay;
          const uint32_t source[]={0xffff0000,0xff00ff00,0xff0000ff,0x80ffffff};
          const uint32_t expected=!masked && sx>=0 && sy>=0 && sx<2 && sy<2 ?
              source[(unsigned)sy*2+(unsigned)sx]:0;
          assert(pixels[y*frame->width+x]==expected);
        }
      }
      for(int i=0;i<120;i++) {
        const DrasticOverlayFrame *same=drastic_background_compose(&config,pw,ph,&ui);
        assert(same==frame && same->pixels==pixels && same->generation==generation);
      }
      /* HUD/menu remains above artwork, while hiding it restores the cached background. */
      memset(ui_pixels,0,sizeof(ui_pixels));ui_pixels[0]=0xff123456;
      ui.visible=true;ui.generation++;
      frame=drastic_background_compose(&config,pw,ph,&ui);
      assert(frame->pixels[0]==0xff123456 && frame->generation>generation);
      ui.visible=false;
      frame=drastic_background_compose(&config,pw,ph,&ui);
      assert(frame->pixels[0]!=0xff123456);
      cases++;
    }
    drastic_background_shutdown();
  }
  /* Fit modes and straight-alpha compositing are independently observable. */
  config.rotation=0;config.screen_count=0;strcpy(config.background,"test.png");
  config.background_fit=0;drastic_background_init(&config);
  ui.visible=false;ui.width=1280;ui.height=720;
  const DrasticOverlayFrame *frame=drastic_background_compose(&config,1280,720,&ui);
  assert(frame->pixels[0]==0); /* Contain leaves side bars for a square image. */
  config.background_fit=1;
  frame=drastic_background_compose(&config,1280,720,&ui);
  assert(frame->pixels[0]==0xffff0000); /* Fill crops the square, preserving the aspect. */
  config.background_fit=2;
  frame=drastic_background_compose(&config,1280,720,&ui);
  assert(frame->pixels[719*1280+1279]==0x80ffffff);
  memset(ui_pixels,0,sizeof(ui_pixels));ui_pixels[0]=0x800000ff;
  ui.visible=true;ui.generation++;
  frame=drastic_background_compose(&config,1280,720,&ui);
  assert(frame->pixels[0]==0xff7f0080);
  config.screens[0]=(DrasticScreenRect){0,0,1280,720,0,0};config.screen_count=1;
  ui.visible=false;frame=drastic_background_compose(&config,1280,720,&ui);
  for(int i=0;i<DRASTIC_OVERLAY_PIXELS;i++)assert(frame->pixels[i]==0);
  drastic_background_shutdown();remove(cache);
  config.screen_count=0;ui.visible=true;
  uint64_t generation=0;
  const char *choices[]={"builtin:white","builtin:pink","builtin:navy","builtin:black","",argv[2]};
  char error[192];
  for(unsigned i=0;i<60;i++) {
    assert(drastic_background_select(&config,choices[i%6],argv[3],error,sizeof(error)));
    frame=drastic_background_compose(&config,1280,720,&ui);
    assert(frame && frame->generation>generation);
    generation=frame->generation;
    char previous[1024];strcpy(previous,config.background);
    assert(!drastic_background_select(&config,"missing-image.png",argv[3],error,sizeof(error)));
    assert(!strcmp(config.background,previous));
    frame=drastic_background_compose(&config,1280,720,&ui);
    assert(frame->generation==generation);
  }
  /* A cancel restores the saved cache even when the original image is gone. */
  DrasticRuntimeConfig backup=config;
  ui.visible=false;
  frame=drastic_background_compose(&config,1280,720,&ui);
  const uint32_t old_pixel=frame->pixels[0];
  assert(remove(argv[2])==0);
  assert(drastic_background_select(&config,"builtin:white",argv[3],error,sizeof(error)));
  assert(drastic_background_set_image(&backup,NULL,0,0));
  config=backup;
  frame=drastic_background_compose(&config,1280,720,&ui);
  assert(frame->pixels[0]==old_pixel && frame->generation>generation);
  unsigned layouts=0;
  for(int layout=0;layout<7;layout++)for(int rotation=0;rotation<4;rotation++)for(int docked=0;docked<2;docked++){
    config.layout=(DrasticLayoutMode)layout;config.rotation=rotation;
    config.custom_aspect_lock=1;
    config.custom_screens[0]=(DrasticScreenRect){0.02f,0.1f,0.4f,0.6f,0,0};
    config.custom_screens[1]=(DrasticScreenRect){0.52f,0.1f,0.4f,0.6f,1,1};
    int pw=docked?1920:1280,ph=docked?1080:720;
    drastic_config_calculate_layout(&config,pw,ph);
    assert(config.screen_count>=1&&config.screen_count<=3);
    for(int i=0;i<config.screen_count;i++){
      const DrasticScreenRect*r=&config.screens[i];
      assert(r->width>0&&r->height>0&&r->x>=-0.001f&&r->y>=-0.001f);
      assert(r->x+r->width<=pw+0.001f&&r->y+r->height<=ph+0.001f);
    }
    layouts++;
  }
  drastic_background_shutdown();
  printf("Background validation passed: %u composition cases, %u shared layouts, 60 live switches, rollback, monotonic GPU generations and stable-frame reuse\n",cases,layouts);
  return 0;
}
