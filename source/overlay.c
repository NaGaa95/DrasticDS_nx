#include <switch.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"

static uint32_t g_pixels[DRASTIC_OVERLAY_PIXELS];
static DrasticOverlayFrame g_frame = {
  .pixels = g_pixels,
  .width = DRASTIC_OVERLAY_WIDTH,
  .height = DRASTIC_OVERLAY_HEIGHT,
};
enum { GLYPH_CACHE_SIZE = 512, GLYPH_MAX_DIMENSION = 48 };

static const struct {
  int pixels;
  int line_height;
  int baseline;
} g_text_sizes[OVERLAY_TEXT_COUNT] = {
  [OVERLAY_TEXT_SMALL] = {18, 24, 19},
  [OVERLAY_TEXT_BODY] = {22, 28, 22},
  [OVERLAY_TEXT_TITLE] = {32, 40, 31},
};

typedef struct {
  uint32_t codepoint;
  int size;
  int advance, left, top, width, height;
  bool valid;
  uint8_t pixels[GLYPH_MAX_DIMENSION * GLYPH_MAX_DIMENSION];
} OverlayGlyph;

static OverlayGlyph g_glyphs[GLYPH_CACHE_SIZE];
static FT_Library g_freetype;
static FT_Face g_faces[PlSharedFontType_Total];
static bool g_face_attempted[PlSharedFontType_Total];
static ConsoleFont g_console_font;
static bool g_font_service;
static uint64_t g_hud_generation = UINT64_MAX;
static int g_hud_show_fps = -1;
static int g_hud_fps_tenth = -2;
static int g_hud_fast_forward = -1;

static int clamp_int(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static FT_Face get_shared_face(PlSharedFontType type) {
  if (!g_freetype) return NULL;
  if (!g_face_attempted[type]) {
    g_face_attempted[type] = true;
    PlFontData data;
    if (R_SUCCEEDED(plGetSharedFontByType(&data, type)) &&
        FT_New_Memory_Face(g_freetype, data.address, data.size, 0,
                           &g_faces[type]) == 0 &&
        FT_Select_Charmap(g_faces[type], FT_ENCODING_UNICODE) != 0) {
      FT_Done_Face(g_faces[type]);
      g_faces[type] = NULL;
    }
  }
  return g_faces[type];
}

void overlay_init(int rotation) {
  const PrintConsole *console = consoleGetDefault();
  if (console) g_console_font = console->font;
  if (!g_font_service && R_SUCCEEDED(plInitialize(PlServiceType_User))) {
    g_font_service = true;
    if (FT_Init_FreeType(&g_freetype) == 0)
      get_shared_face(PlSharedFontType_Standard);
  }
  g_frame.width = (rotation & 1) ? DRASTIC_OVERLAY_HEIGHT
                                 : DRASTIC_OVERLAY_WIDTH;
  g_frame.height = (rotation & 1) ? DRASTIC_OVERLAY_WIDTH
                                  : DRASTIC_OVERLAY_HEIGHT;
  memset(g_pixels, 0, sizeof(g_pixels));
}

void overlay_shutdown(void) {
  if (g_freetype) FT_Done_FreeType(g_freetype);
  g_freetype = NULL;
  if (g_font_service) plExit();
  g_font_service = false;
  memset(g_faces, 0, sizeof(g_faces));
  memset(g_face_attempted, 0, sizeof(g_face_attempted));
  memset(g_glyphs, 0, sizeof(g_glyphs));
}

void overlay_set_rotation(int rotation) {
  const int width = (rotation & 1) ? DRASTIC_OVERLAY_HEIGHT
                                   : DRASTIC_OVERLAY_WIDTH;
  const int height = (rotation & 1) ? DRASTIC_OVERLAY_WIDTH
                                    : DRASTIC_OVERLAY_HEIGHT;
  if (g_frame.width == width && g_frame.height == height) return;
  g_frame.width = width;
  g_frame.height = height;
  memset(g_pixels, 0, sizeof(g_pixels));
  g_frame.visible = false;
  g_frame.generation++;
  g_hud_generation = UINT64_MAX;
}

int overlay_width(void) { return g_frame.width; }

int overlay_height(void) { return g_frame.height; }

void overlay_begin(void) {
  memset(g_pixels, 0, sizeof(g_pixels));
  g_frame.visible = true;
}

void overlay_finish(void) {
  g_frame.visible = true;
  g_frame.generation++;
}

void overlay_hide(void) {
  if (g_frame.visible) {
    g_frame.visible = false;
    g_frame.generation++;
  }
}

void overlay_draw_hud(bool show_fps, float fps, bool fast_forward) {
  int fps_tenth = -1;
  if (fps > 0.0f) {
    fps_tenth = (int)(fps * 10.0f + 0.5f);
    if (fps_tenth > 9999) fps_tenth = 9999;
  }

  if (!show_fps && !fast_forward) {
    overlay_hide();
    g_hud_show_fps = 0;
    g_hud_fast_forward = 0;
    g_hud_fps_tenth = fps_tenth;
    g_hud_generation = g_frame.generation;
    return;
  }

  if (g_frame.visible && g_hud_generation == g_frame.generation &&
      g_hud_show_fps == (int)show_fps &&
      g_hud_fast_forward == (int)fast_forward &&
      g_hud_fps_tenth == fps_tenth)
    return;

  enum {
    HUD_MARGIN = 18,
    HUD_HEIGHT = 36,
    HUD_GAP = 8,
  };
  const uint32_t background = 0xf20c1422u;
  const uint32_t text = 0xfff2f7fbu;
  const int right = g_frame.width - HUD_MARGIN;
  int top = HUD_MARGIN;

  overlay_begin();
  if (show_fps) {
    char label[32];
    if (fps_tenth < 0)
      snprintf(label, sizeof(label), "FPS --.-");
    else
      snprintf(label, sizeof(label), "FPS %d.%d",
               fps_tenth / 10, fps_tenth % 10);
    const int width = 36 + overlay_text_width(OVERLAY_TEXT_SMALL, "FPS 999.9");
    overlay_fill_round_rect(right - width, top, width, HUD_HEIGHT, 10,
                            background);
    overlay_draw_text(right - width + 16, top + 6, OVERLAY_TEXT_SMALL,
                      text, label);
    top += HUD_HEIGHT + HUD_GAP;
  }
  if (fast_forward) {
    const char *label = "Fast forward";
    const int width = 32 + overlay_text_width(OVERLAY_TEXT_SMALL, label);
    overlay_fill_round_rect(right - width, top, width, HUD_HEIGHT, 10,
                            background);
    overlay_draw_text(right - width + 16, top + 6, OVERLAY_TEXT_SMALL,
                      text, label);
  }
  overlay_finish();
  g_hud_show_fps = show_fps;
  g_hud_fast_forward = fast_forward;
  g_hud_fps_tenth = fps_tenth;
  g_hud_generation = g_frame.generation;
}

const DrasticOverlayFrame *overlay_frame(void) { return &g_frame; }

static uint32_t composite_argb(uint32_t destination, uint32_t source) {
  const unsigned source_alpha = source >> 24;
  if (!source_alpha) return destination;
  if (source_alpha == 255 || !(destination >> 24)) return source;

  const unsigned destination_alpha = destination >> 24;
  const unsigned inverse_alpha = 255 - source_alpha;
  const unsigned output_alpha = source_alpha +
      (destination_alpha * inverse_alpha + 127) / 255;
  uint32_t output = (uint32_t)output_alpha << 24;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    const unsigned source_channel = (source >> shift) & 255;
    const unsigned destination_channel = (destination >> shift) & 255;
    const unsigned premultiplied = source_channel * source_alpha +
        (destination_channel * destination_alpha * inverse_alpha + 127) / 255;
    const unsigned output_channel =
        (premultiplied + output_alpha / 2) / output_alpha;
    output |= (uint32_t)output_channel << shift;
  }
  return output;
}

static uint32_t with_coverage(uint32_t color, unsigned coverage) {
  const unsigned alpha = ((color >> 24) * coverage + 127) / 255;
  return (color & 0x00ffffffu) | (alpha << 24);
}

void overlay_fill_rect(int x, int y, int width, int height, uint32_t color) {
  if (width <= 0 || height <= 0 || !(color >> 24)) return;
  const int left = clamp_int(x, 0, g_frame.width);
  const int top = clamp_int(y, 0, g_frame.height);
  const int right = clamp_int(x + width, 0, g_frame.width);
  const int bottom = clamp_int(y + height, 0, g_frame.height);
  const bool opaque = (color >> 24) == 255;
  for (int row = top; row < bottom; row++) {
    uint32_t *destination = g_pixels + (size_t)row * g_frame.width;
    if (opaque) {
      for (int column = left; column < right; column++)
        destination[column] = color;
    } else {
      for (int column = left; column < right; column++)
        destination[column] = composite_argb(destination[column], color);
    }
  }
}

void overlay_fill_round_rect(int x, int y, int width, int height, int radius,
                             uint32_t color) {
  if (width <= 0 || height <= 0) return;
  radius = clamp_int(radius, 0, (width < height ? width : height) / 2);
  overlay_fill_rect(x, y + radius, width, height - 2 * radius, color);
  for (int row = 0; row < radius; row++) {
    const float dy = radius - row - 0.5f;
    const float inset = radius - sqrtf((float)(radius * radius) - dy * dy);
    const int edge = (int)ceilf(inset);
    const uint32_t edge_color =
        with_coverage(color, (unsigned)((edge - inset) * 255.0f));
    const int lines[] = {y + row, y + height - row - 1};
    for (int i = 0; i < 2; ++i) {
      overlay_fill_rect(x + edge, lines[i], width - 2 * edge, 1, color);
      overlay_fill_rect(x + edge - 1, lines[i], 1, 1, edge_color);
      overlay_fill_rect(x + width - edge, lines[i], 1, 1, edge_color);
    }
  }
}

void overlay_border_rect(int x, int y, int width, int height, int thickness,
                         uint32_t color) {
  if (thickness <= 0) return;
  overlay_fill_rect(x, y, width, thickness, color);
  overlay_fill_rect(x, y + height - thickness, width, thickness, color);
  overlay_fill_rect(x, y, thickness, height, color);
  overlay_fill_rect(x + width - thickness, y, thickness, height, color);
}

static uint32_t read_codepoint(const char **text) {
  const uint8_t *cursor = (const uint8_t *)*text;
  const uint8_t first = *cursor++;
  *text = (const char *)cursor;
  if (first < 0x80) return first;
  if (first < 0xc2 || first > 0xf4) return 0xfffd;

  uint32_t codepoint;
  int remaining;
  uint32_t minimum;
  if (first < 0xe0) {
    codepoint = first & 0x1f;
    remaining = 1;
    minimum = 0x80;
  } else if (first < 0xf0) {
    codepoint = first & 0x0f;
    remaining = 2;
    minimum = 0x800;
  } else {
    codepoint = first & 0x07;
    remaining = 3;
    minimum = 0x10000;
  }

  for (int i = 0; i < remaining; ++i) {
    if ((*cursor & 0xc0) != 0x80) return 0xfffd;
    codepoint = (codepoint << 6) | (*cursor++ & 0x3f);
    *text = (const char *)cursor;
  }
  if (codepoint < minimum || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff))
    return 0xfffd;
  return codepoint;
}

static bool load_outline_glyph(OverlayGlyph *glyph, FT_Face face,
                               DrasticOverlayTextSize size, uint32_t codepoint) {
  if (!face) return false;
  const FT_UInt index = FT_Get_Char_Index(face, codepoint);
  if (!index || FT_Set_Pixel_Sizes(face, 0, g_text_sizes[size].pixels) ||
      FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
    return false;

  const FT_GlyphSlot slot = face->glyph;
  const FT_Bitmap *bitmap = &slot->bitmap;
  if (bitmap->width > GLYPH_MAX_DIMENSION ||
      bitmap->rows > GLYPH_MAX_DIMENSION ||
      (bitmap->width && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY))
    return false;
  glyph->advance = (int)((slot->advance.x + 32) / 64);
  glyph->left = slot->bitmap_left;
  glyph->top = slot->bitmap_top;
  glyph->width = (int)bitmap->width;
  glyph->height = (int)bitmap->rows;
  const int pitch = bitmap->pitch;
  const uint8_t *row = bitmap->buffer;
  if (pitch < 0 && glyph->height > 0)
    row += (size_t)(glyph->height - 1) * (size_t)-pitch;
  for (int i = 0; i < glyph->height; ++i)
    memcpy(glyph->pixels + (size_t)i * glyph->width,
           row + (ptrdiff_t)i * pitch, glyph->width);
  return true;
}

static void load_console_glyph(OverlayGlyph *glyph, DrasticOverlayTextSize size,
                               uint32_t codepoint) {
  const ConsoleFont *font = &g_console_font;
  const int scale = size == OVERLAY_TEXT_TITLE ? 2 : 1;
  glyph->advance = font->tileWidth ? (int)font->tileWidth * scale : 8 * scale;
  if (!font->gfx || !font->tileWidth || !font->tileHeight ||
      font->tileWidth * scale > GLYPH_MAX_DIMENSION ||
      font->tileHeight * scale > GLYPH_MAX_DIMENSION)
    return;

  const uint32_t first = font->asciiOffset;
  const uint32_t end = first + font->numChars;
  if (codepoint < first || codepoint >= end) codepoint = '?';
  if (codepoint < first || codepoint >= end) return;

  const unsigned stride = (font->tileWidth + 7) / 8;
  const uint8_t *tile = (const uint8_t *)font->gfx +
      (size_t)(codepoint - first) * font->tileHeight * stride;
  glyph->width = font->tileWidth * scale;
  glyph->height = font->tileHeight * scale;
  glyph->top = g_text_sizes[size].baseline -
      (g_text_sizes[size].line_height - glyph->height) / 2;
  for (int row = 0; row < glyph->height; ++row) {
    for (int column = 0; column < glyph->width; ++column) {
      const unsigned bit = (unsigned)font->tileWidth - 1 - (column / scale);
      glyph->pixels[(size_t)row * glyph->width + column] =
          (tile[(size_t)(row / scale) * stride + bit / 8] &
           (1u << (bit & 7))) ? 255 : 0;
    }
  }
}

static const OverlayGlyph *get_glyph(DrasticOverlayTextSize size,
                                    uint32_t codepoint) {
  const uint32_t hash = codepoint * 2654435761u + size * 67u;
  OverlayGlyph *glyph = &g_glyphs[hash % GLYPH_CACHE_SIZE];
  if (glyph->valid && glyph->codepoint == codepoint && glyph->size == (int)size)
    return glyph;

  glyph->codepoint = codepoint;
  glyph->size = size;
  glyph->valid = true;
  glyph->advance = glyph->left = glyph->top = glyph->width = glyph->height = 0;
  const FT_Face standard = get_shared_face(PlSharedFontType_Standard);
  if (load_outline_glyph(glyph, standard, size, codepoint)) return glyph;
  if (g_freetype && codepoint >= 0x80 && codepoint != 0xfffd) {
    for (int type = PlSharedFontType_ChineseSimplified;
         type <= PlSharedFontType_KO; ++type)
      if (load_outline_glyph(glyph, get_shared_face(type), size, codepoint))
        return glyph;
  }
  if (load_outline_glyph(glyph, standard, size, '?')) return glyph;
  load_console_glyph(glyph, size, codepoint);
  return glyph;
}

int overlay_text_height(DrasticOverlayTextSize size) {
  return g_text_sizes[size].line_height;
}

int overlay_text_width(DrasticOverlayTextSize size, const char *text) {
  int width = 0;
  if (!text) return width;
  while (*text && *text != '\n' && *text != '\r')
    width += get_glyph(size, read_codepoint(&text))->advance;
  return width;
}

static void draw_glyph(int x, int baseline, const OverlayGlyph *glyph,
                       int clip_left, int clip_right, uint32_t color) {
  x += glyph->left;
  const int y = baseline - glyph->top;
  const int left = clamp_int(x, clip_left, clip_right);
  const int right = clamp_int(x + glyph->width, clip_left, clip_right);
  const int top = clamp_int(y, 0, g_frame.height);
  const int bottom = clamp_int(y + glyph->height, 0, g_frame.height);
  for (int row = top; row < bottom; ++row) {
    uint32_t *destination = g_pixels + (size_t)row * g_frame.width;
    const uint8_t *source =
        glyph->pixels + (size_t)(row - y) * glyph->width;
    for (int column = left; column < right; ++column) {
      const unsigned coverage = source[column - x];
      if (coverage)
        destination[column] =
            composite_argb(destination[column], with_coverage(color, coverage));
    }
  }
}

static int draw_text_range(int x, int y, int left, int right,
                           DrasticOverlayTextSize size, uint32_t color,
                           const char *text, const char *end) {
  left = clamp_int(left, 0, g_frame.width);
  right = clamp_int(right, left, g_frame.width);
  const int baseline = y + g_text_sizes[size].baseline;
  while (*text && *text != '\n' && *text != '\r' && (!end || text < end) &&
         x < right) {
    const OverlayGlyph *glyph = get_glyph(size, read_codepoint(&text));
    draw_glyph(x, baseline, glyph, left, right, color);
    x += glyph->advance;
  }
  return x;
}

void overlay_draw_text(int x, int y, DrasticOverlayTextSize size,
                       uint32_t color, const char *text) {
  if (text) draw_text_range(x, y, 0, g_frame.width, size, color, text, NULL);
}

void overlay_draw_text_clipped(int x, int y, int max_width,
                               DrasticOverlayTextSize size, uint32_t color,
                               const char *text) {
  if (!text || max_width <= 0) return;
  const int right = x + max_width;
  if (overlay_text_width(size, text) <= max_width) {
    draw_text_range(x, y, x, right, size, color, text, NULL);
    return;
  }

  const int dots_width = overlay_text_width(size, "...");
  if (dots_width >= max_width) {
    draw_text_range(x, y, x, right, size, color, text, NULL);
    return;
  }
  const char *end = text;
  int available = max_width - dots_width;
  while (*end && *end != '\n' && *end != '\r') {
    const char *next = end;
    const int advance = get_glyph(size, read_codepoint(&next))->advance;
    if (advance > available) break;
    available -= advance;
    end = next;
  }
  const int pen = draw_text_range(x, y, x, right, size, color, text, end);
  draw_text_range(pen, y, x, right, size, color, "...", NULL);
}

static int marquee_offset(int overflow) {
  const uint64_t frequency = armGetSystemTickFreq();
  if (overflow <= 0 || !frequency) return 0;
  const uint64_t ticks = armGetSystemTick();
  const uint64_t milliseconds = (ticks / frequency) * 1000u +
      ((ticks % frequency) * 1000u) / frequency;
  const uint64_t pause = 800;
  const uint64_t travel = 3600;
  const uint64_t phase = milliseconds % (2 * (pause + travel));
  if (phase < pause) return 0;
  if (phase < pause + travel)
    return (int)((phase - pause) * (uint64_t)overflow / travel);
  if (phase < 2 * pause + travel) return overflow;
  return overflow - (int)((phase - 2 * pause - travel) *
                           (uint64_t)overflow / travel);
}

void overlay_draw_text_scrolling(int x, int y, int max_width,
                                 DrasticOverlayTextSize size, uint32_t color,
                                 const char *text) {
  if (!text || max_width <= 0) return;
  const int offset = marquee_offset(overlay_text_width(size, text) - max_width);
  draw_text_range(x - offset, y, x, x + max_width, size, color, text, NULL);
}

void overlay_draw_text_right(int right, int y, DrasticOverlayTextSize size,
                             uint32_t color, const char *text) {
  overlay_draw_text(right - overlay_text_width(size, text), y, size, color, text);
}

void overlay_draw_wrapped(int x, int y, int max_width, int max_lines,
                          DrasticOverlayTextSize size, uint32_t color,
                          const char *text) {
  if (!text || max_width <= 0) return;
  for (int line = 0; line < max_lines; ++line) {
    while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')
      ++text;
    if (!*text) break;
    if (line == max_lines - 1) {
      overlay_draw_text_clipped(x, y, max_width, size, color, text);
      break;
    }

    int width = 0;
    const char *end = text;
    const char *space = NULL;
    while (*end && *end != '\n' && *end != '\r') {
      const char *next = end;
      const int advance = get_glyph(size, read_codepoint(&next))->advance;
      if (width + advance > max_width) {
        if (space) end = space;
        else if (end == text) end = next;
        break;
      }
      if (*end == ' ') space = end;
      end = next;
      width += advance;
    }
    draw_text_range(x, y, x, x + max_width, size, color, text, end);
    text = end;
    y += overlay_text_height(size);
  }
}

static uint32_t rgb565_to_argb(uint16_t pixel) {
  const unsigned red = (pixel >> 11) & 31;
  const unsigned green = (pixel >> 5) & 63;
  const unsigned blue = pixel & 31;
  return 0xff000000u | ((red * 255 / 31) << 16) |
         ((green * 255 / 63) << 8) | (blue * 255 / 31);
}

void overlay_blit_snapshot(int x, int y, int width, int height,
                           const int32_t *pixels, int source_width,
                           int source_height) {
  if (!pixels || width <= 0 || height <= 0 || source_width <= 0 ||
      source_height <= 0) return;
  int argb_pixels = 0;
  const int samples = source_width * source_height;
  for (int index = 0; index < samples; index += 257) {
    const uint32_t pixel = (uint32_t)pixels[index];
    if ((pixel & 0xffff0000u) && pixel != 0xffffffffu) {
      argb_pixels = 1;
      break;
    }
  }
  for (int destination_y = 0; destination_y < height; destination_y++) {
    const int output_y = y + destination_y;
    if (output_y < 0 || output_y >= g_frame.height) continue;
    const int source_y = destination_y * source_height / height;
    uint32_t *destination = g_pixels +
        (size_t)output_y * g_frame.width;
    for (int destination_x = 0; destination_x < width; destination_x++) {
      const int output_x = x + destination_x;
      if (output_x < 0 || output_x >= g_frame.width) continue;
      const int source_x = destination_x * source_width / width;
      uint32_t pixel = (uint32_t)pixels[source_y * source_width + source_x];
      if (!argb_pixels) pixel = rgb565_to_argb((uint16_t)pixel);
      else if (!(pixel & 0xff000000u)) pixel |= 0xff000000u;
      destination[output_x] = pixel;
    }
  }
}
