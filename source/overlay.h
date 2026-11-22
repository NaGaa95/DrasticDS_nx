#ifndef DRASTIC_NX_OVERLAY_H
#define DRASTIC_NX_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#define DRASTIC_OVERLAY_WIDTH  1280
#define DRASTIC_OVERLAY_HEIGHT 720
#define DRASTIC_OVERLAY_PIXELS \
  (DRASTIC_OVERLAY_WIDTH * DRASTIC_OVERLAY_HEIGHT)

typedef struct {
  const uint32_t *pixels; /* 0xAARRGGBB */
  int width;
  int height;
  uint64_t generation;
  bool visible;
} DrasticOverlayFrame;

typedef enum {
  OVERLAY_TEXT_SMALL,
  OVERLAY_TEXT_BODY,
  OVERLAY_TEXT_TITLE,
  OVERLAY_TEXT_COUNT,
} DrasticOverlayTextSize;

void overlay_init(int rotation);
void overlay_shutdown(void);
void overlay_set_rotation(int rotation);
int overlay_width(void);
int overlay_height(void);
void overlay_begin(void);
void overlay_finish(void);
void overlay_hide(void);
void overlay_draw_hud(bool show_fps, float fps, bool fast_forward);
const DrasticOverlayFrame *overlay_frame(void);

void overlay_fill_rect(int x, int y, int width, int height, uint32_t color);
void overlay_fill_round_rect(int x, int y, int width, int height, int radius,
                             uint32_t color);
void overlay_border_rect(int x, int y, int width, int height, int thickness,
                         uint32_t color);
int overlay_text_width(DrasticOverlayTextSize size, const char *text);
int overlay_text_height(DrasticOverlayTextSize size);
void overlay_draw_text(int x, int y, DrasticOverlayTextSize size,
                       uint32_t color, const char *text);
void overlay_draw_text_clipped(int x, int y, int max_width,
                               DrasticOverlayTextSize size, uint32_t color,
                               const char *text);
void overlay_draw_text_scrolling(int x, int y, int max_width,
                                 DrasticOverlayTextSize size, uint32_t color,
                                 const char *text);
void overlay_draw_text_right(int right, int y, DrasticOverlayTextSize size,
                             uint32_t color, const char *text);
void overlay_draw_wrapped(int x, int y, int max_width, int max_lines,
                          DrasticOverlayTextSize size, uint32_t color,
                          const char *text);
void overlay_blit_snapshot(int x, int y, int width, int height,
                           const int32_t *pixels, int source_width,
                           int source_height);

#endif
