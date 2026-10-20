#include "drastic_background.h"
#include "drastic_rotation.h"
#include "drastic_background_files.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t *g_source;
static uint32_t *g_owned_source, *g_background, *g_composite;
static const DrasticBackgroundImage *g_image;
static bool g_enabled;
static unsigned g_source_width, g_source_height;
static int g_style, g_fit, g_rotation, g_panel_width, g_panel_height, g_count;
static DrasticScreenRect g_rects[3];
static DrasticOverlayFrame g_frame;
static uint64_t g_ui_generation;
static const uint32_t *g_ui_pixels;
static bool g_ui_visible, g_background_visible, g_dirty;
static uint64_t g_generation;
static DrasticOverlayFrame g_passthrough;
static uint64_t g_passthrough_generation;
static bool g_output_dirty = true;

void drastic_background_shutdown(void) {
  drastic_background_image_release(g_image); g_image=NULL;
  free(g_owned_source); free(g_background); free(g_composite);
  g_owned_source=g_background=g_composite=NULL; g_source=NULL;
  g_enabled=false;
  g_source_width = g_source_height = 0;
  g_style = 0;
  memset(&g_frame, 0, sizeof(g_frame));
  g_output_dirty = true;
}

static bool load_config(const DrasticRuntimeConfig *config, const uint32_t *pixels,
                        unsigned width, unsigned height, const DrasticBackgroundImage *cached) {
  if (!config || !config->background[0]) {
    drastic_background_image_release(g_image); g_image=NULL;
    free(g_owned_source); g_owned_source=NULL; g_source=NULL;
    g_enabled=false; g_output_dirty=true;
    return true;
  }
  const uint32_t *source = NULL;
  uint32_t *owned=NULL;
  const DrasticBackgroundImage *image=NULL;
  unsigned source_width=0, source_height=0;
  int style=0;
  const char *styles[] = {"builtin:white", "builtin:pink", "builtin:navy", "builtin:black"};
  for (int i = 0; i < 4; ++i)
    if (!strcmp(config->background, styles[i])) style = i + 1;
  if (!style && pixels) {
    if (!width || !height || width>DRASTIC_BACKGROUND_MAX_EDGE || height>DRASTIC_BACKGROUND_MAX_EDGE) return false;
    const size_t bytes=(size_t)width*height*4;
    owned=malloc(bytes); if (!owned) return false;
    memcpy(owned,pixels,bytes); source=owned; source_width=width; source_height=height;
  } else if (!style) {
    image=cached ? drastic_background_image_retain(cached) :
        drastic_background_image_acquire(config->background,config->background_cache,NULL,0);
    if (!image) return false;
    source=image->pixels; source_width=image->width; source_height=image->height;
  }
  uint32_t *background = g_background ? g_background : malloc(DRASTIC_OVERLAY_PIXELS * 4);
  uint32_t *composite = g_composite ? g_composite : malloc(DRASTIC_OVERLAY_PIXELS * 4);
  if (!background || !composite) {
    if (background != g_background) free(background);
    if (composite != g_composite) free(composite);
    free(owned); drastic_background_image_release(image); return false;
  }
  free(g_owned_source); g_owned_source=owned;
  drastic_background_image_release(g_image); g_image=image;
  g_source=source; g_source_width=source_width; g_source_height=source_height;
  g_background=background; g_composite=composite; g_style=style;
  g_enabled=true;
  g_panel_width = g_panel_height = 0;
  g_ui_pixels = NULL;
  g_ui_visible = false;
  g_dirty = true;
  g_output_dirty = true;
  return true;
}

void drastic_background_init(const DrasticRuntimeConfig *config) {
  drastic_background_shutdown();
  load_config(config,NULL,0,0,NULL);
}

bool drastic_background_set_cached_image(const DrasticRuntimeConfig *config,
                                         const DrasticBackgroundImage *image) {
  if (!image && config && config->background[0] &&
      strcmp(config->background,"builtin:white") && strcmp(config->background,"builtin:pink") &&
      strcmp(config->background,"builtin:navy") && strcmp(config->background,"builtin:black")) return false;
  return load_config(config,NULL,0,0,image);
}

const DrasticBackgroundImage *drastic_background_acquire_current_image(void) {
  return drastic_background_image_retain(g_image);
}

bool drastic_background_set_image(const DrasticRuntimeConfig *config,
                                  const uint32_t *pixels, unsigned width, unsigned height) {
  return load_config(config,pixels,width,height,NULL);
}

bool drastic_background_select(DrasticRuntimeConfig *config, const char *path, const char *directory,
                                char *error, size_t error_size) {
  if (!config || !path || strlen(path)>=sizeof(config->background)) return false;
  DrasticRuntimeConfig candidate=*config;
  snprintf(candidate.background,sizeof(candidate.background),"%s",path);
  candidate.background_cache[0]=0;
  bool builtin=!*path;
  const char *styles[]={"builtin:white","builtin:pink","builtin:navy","builtin:black"};
  for (unsigned i=0;i<4;i++) if (!strcmp(path,styles[i])) builtin=true;
  if (!builtin && !drastic_background_prepare_image(path,directory,candidate.background_cache,
          sizeof(candidate.background_cache),error,error_size)) return false;
  if (!builtin) drastic_background_apply_layout(&candidate,path);
  if (!load_config(&candidate,NULL,0,0,NULL)) {
    if (error && error_size) snprintf(error,error_size,"Could not prepare the background image.");
    return false;
  }
  *config=candidate;
  return true;
}

/* Keep one generation namespace across image switches, None and menu/HUD
 * frames. Otherwise a new generation 1 can alias an old GPU overlay upload. */
static const DrasticOverlayFrame *passthrough(const DrasticOverlayFrame *ui) {
  if (!ui) return NULL;
  if (g_output_dirty || ui->generation!=g_passthrough_generation ||
      ui->pixels!=g_passthrough.pixels || ui->visible!=g_passthrough.visible ||
      ui->width!=g_passthrough.width || ui->height!=g_passthrough.height) {
    g_passthrough=*ui;
    g_passthrough_generation=ui->generation;
    g_passthrough.generation=++g_generation;
    g_output_dirty=false;
  }
  return &g_passthrough;
}

static uint32_t over(uint32_t destination, uint32_t source) {
  const unsigned sa = source >> 24, da = destination >> 24;
  if (!sa) return destination;
  if (sa == 255 || !da) return source;
  const unsigned inverse = 255 - sa;
  const unsigned alpha = sa + (da * inverse + 127) / 255;
  uint32_t result = alpha << 24;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const unsigned channel = ((source >> shift) & 255) * sa +
        (((destination >> shift) & 255) * da * inverse + 127) / 255;
    result |= ((channel + alpha / 2) / alpha) << shift;
  }
  return result;
}

/* PocketArch NDS bezel palette, edge shading and black screen surround,
 * adapted to the actual rectangles instead of a fixed two-screen shader.
 * MIT, stunrelay. See third_party/backgrounds/pocketarch/. */
static uint32_t bezel_pixel(int x, int y, int width, int height,
                            float display_x, float display_y) {
  static const unsigned colors[] = {0, 0xf5f5f5, 0xf6c6c0, 0x2b3856, 0x222222};
  const int edge = (int)fminf(fminf(x, width-1-x), fminf(y, height-1-y));
  float shadow = fmaxf(0, 1.0f - edge / 48.0f);
  shadow = shadow * shadow * (3 - 2 * shadow) * 0.25f;
  const int grain = (int)(((unsigned)x * 1664525u ^ (unsigned)y * 1013904223u) >> 30) - 1;
  uint32_t pixel = 0xff000000u;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    int value = (int)(((colors[g_style] >> shift) & 255) * (1 - shadow)) + grain;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    pixel |= (unsigned)value << shift;
  }
  const float margin = 8.0f * g_panel_width / 1280.0f;
  for (int i = 0; i < g_count; ++i) {
    const DrasticScreenRect *r = &g_rects[i];
    if (display_x >= r->x-margin && display_x < r->x+r->width+margin &&
        display_y >= r->y-margin && display_y < r->y+r->height+margin)
      return 0xff080808u;
  }
  return pixel;
}

static void clear_image_screens(int width, int height, int pw, int ph) {
  /* Each rotated rectangle is still axis aligned. Determine its scanline
   * spans once instead of rotating and testing every artwork pixel. Keep
   * the same pixel-center comparisons and one-texel guard as the renderer. */
  const float guard=(float)pw/DRASTIC_OVERLAY_WIDTH;
  for (int i=0;i<g_count;i++) {
    const DrasticScreenRect *r=&g_rects[i];
    int x0=width,x1=0,y0=height,y1=0;
    for (int x=0;x<width;x++) {
      float u,v;
      drastic_rotation_source_to_display(g_rotation,(x+0.5f)/width,0,&u,&v);
      const float value=g_rotation&1 ? v*ph : u*pw;
      const float start=g_rotation&1 ? r->y : r->x;
      const float extent=g_rotation&1 ? r->height : r->width;
      if (value>=start-guard && value<start+extent+guard) { if (x<x0) x0=x; x1=x+1; }
    }
    for (int y=0;y<height;y++) {
      float u,v;
      drastic_rotation_source_to_display(g_rotation,0,(y+0.5f)/height,&u,&v);
      const float value=g_rotation&1 ? u*pw : v*ph;
      const float start=g_rotation&1 ? r->x : r->y;
      const float extent=g_rotation&1 ? r->width : r->height;
      if (value>=start-guard && value<start+extent+guard) { if (y<y0) y0=y; y1=y+1; }
    }
    if (x1>x0) for (int y=y0;y<y1;y++)
      memset(g_background+(size_t)y*width+x0,0,(size_t)(x1-x0)*4);
  }
}

static void rebuild(const DrasticRuntimeConfig *config, int pw, int ph) {
  g_rotation = config->rotation;
  g_fit = config->background_fit;
  g_panel_width = pw; g_panel_height = ph;
  g_count = config->screen_count;
  memcpy(g_rects, config->screens, sizeof(g_rects));
  const int width = (g_rotation & 1) ? DRASTIC_OVERLAY_HEIGHT : DRASTIC_OVERLAY_WIDTH;
  const int height = (g_rotation & 1) ? DRASTIC_OVERLAY_WIDTH : DRASTIC_OVERLAY_HEIGHT;
  float scale_x = g_source ? (float)width / g_source_width : 1;
  float scale_y = g_source ? (float)height / g_source_height : 1;
  if (g_fit != 2) scale_x = scale_y = g_fit == 1 ? fmaxf(scale_x, scale_y) : fminf(scale_x, scale_y);
  const float left = (width - g_source_width * scale_x) * 0.5f;
  const float top = (height - g_source_height * scale_y) * 0.5f;
  g_background_visible = false;
  if (g_source) {
    int columns[DRASTIC_OVERLAY_WIDTH];
    for (int x=0;x<width;x++) {
      const float sx=(x+0.5f-left)/scale_x;
      columns[x]=sx>=0 && sx<g_source_width ? (int)sx : -1;
    }
    for (int y=0;y<height;y++) {
      const float sy=(y+0.5f-top)/scale_y;
      uint32_t *destination=g_background+(size_t)y*width;
      if (sy<0 || sy>=g_source_height) memset(destination,0,(size_t)width*4);
      else {
        const uint32_t *row=g_source+(size_t)(unsigned)sy*g_source_width;
        for (int x=0;x<width;x++) destination[x]=columns[x]<0 ? 0 : row[columns[x]];
      }
    }
    clear_image_screens(width,height,pw,ph);
    uint32_t alpha=0;
    for (size_t i=0;i<DRASTIC_OVERLAY_PIXELS;i++) alpha|=g_background[i];
    g_background_visible=(alpha>>24)!=0;
    g_frame.width=width; g_frame.height=height; g_dirty=true;
    return;
  }
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    float u, v;
    drastic_rotation_source_to_display(g_rotation, (x+0.5f)/width, (y+0.5f)/height, &u, &v);
    const float dx = u * pw, dy = v * ph;
    bool screen = false;
    /* Transparent one-texel guard prevents filtered artwork bleeding over games. */
    const float guard = (float)pw / DRASTIC_OVERLAY_WIDTH;
    for (int i = 0; i < g_count; ++i) {
      const DrasticScreenRect *r = &g_rects[i];
      if (dx >= r->x-guard && dx < r->x+r->width+guard &&
          dy >= r->y-guard && dy < r->y+r->height+guard) { screen = true; break; }
    }
    uint32_t pixel = 0;
    if (!screen && g_style) pixel = bezel_pixel(x, y, width, height, dx, dy);
    g_background[(size_t)y*width+x] = pixel;
    if (pixel >> 24) g_background_visible = true;
  }
  g_frame.width = width; g_frame.height = height;
  g_dirty = true;
}

const DrasticOverlayFrame *drastic_background_compose(
    const DrasticRuntimeConfig *config, int pw, int ph, const DrasticOverlayFrame *ui) {
  if (!g_enabled || !g_background || !config || pw <= 0 || ph <= 0 ||
      config->screen_count < 0 || config->screen_count > 3) return passthrough(ui);
  if (g_panel_width != pw || g_panel_height != ph || g_rotation != config->rotation ||
      g_fit != config->background_fit || g_count != config->screen_count ||
      memcmp(g_rects, config->screens, sizeof(g_rects))) rebuild(config, pw, ph);
  const bool visible = ui && ui->visible && ui->pixels &&
      ui->width == g_frame.width && ui->height == g_frame.height;
  if (g_dirty || visible != g_ui_visible ||
      (visible && (ui->generation != g_ui_generation || ui->pixels != g_ui_pixels))) {
    if (visible) {
      for (size_t i = 0; i < DRASTIC_OVERLAY_PIXELS; ++i)
        g_composite[i] = over(g_background[i], ui->pixels[i]);
    } else memcpy(g_composite, g_background, DRASTIC_OVERLAY_PIXELS * 4);
    g_frame.pixels = g_composite;
    g_frame.visible = g_background_visible || visible;
    g_frame.generation=++g_generation;
    g_ui_visible = visible;
    g_ui_generation = visible ? ui->generation : 0;
    g_ui_pixels = visible ? ui->pixels : NULL;
    g_dirty = false;
  }
  return &g_frame;
}
