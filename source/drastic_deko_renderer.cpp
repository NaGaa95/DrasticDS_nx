/* Native Horizon display backend. No Android renderFrame, GL, EGL, Vulkan,
 * runtime shader compiler, or CPU pixel conversion in the core frame path. */
#include "drastic_deko_renderer.h"
#include "drastic_native_frame.h"
#include "drastic_dksh.h"
#include "drastic_dfx.h"
#include "drastic_rotation.h"
#include "lsfg/drastic_deko_lsfg.h"
#include <deko3d.h>
#include <switch.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
extern "C" {
#include "config.h"
#include "error.h"
#include "prefs.h"
}
#include "deko_programs.h"
#include "drastic_smaa_area_rgb_bin.h"
#include "drastic_smaa_search_rgb_bin.h"

namespace {
constexpr unsigned kSlots = 2, kMaxImages = 3, kDescriptors = 1024;
constexpr unsigned kCommandBytes = 512 * 1024, kDrawBytes = 256 * 1024;
constexpr unsigned kPresenter = DRASTIC_DFX_SHADER_COUNT;
constexpr unsigned kCpuGpu = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
constexpr size_t kCustomBudget = 256u * 1024 * 1024;
constexpr size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

struct Image {
  DkMemBlock memory{};
  DkImage image{};
  unsigned width{}, height{};
  bool valid{}, opaque{};
  DkImageView view() const {
    DkImageView v; dkImageViewDefaults(&v, &image);
    if (opaque) v.swizzle[3] = DkImageSwizzle_One;
    return v;
  }
  void destroy() {
    if (memory) dkMemBlockDestroy(memory);
    *this = {};
  }
};
struct Program {
  DkMemBlock code[2]{};
  DkShader shaders[2]{};
  void destroy() {
    for (auto &m : code) { if (m) dkMemBlockDestroy(m); m = nullptr; }
  }
};
struct Slot {
  DkCmdBuf cmd{};
  DkMemBlock commands{}, data{}, descriptors{};
  DkFence fence{};
  bool pending{};
  unsigned used{}, descriptor_count{};
};
struct Custom {
  DrasticCustomShader spec{};
  Program programs[DRASTIC_CUSTOM_SHADER_MAX_PASSES];
  Image raw[DRASTIC_CUSTOM_SHADER_MAX_TEXTURES];
  Image targets[2][2][DRASTIC_CUSTOM_SHADER_MAX_TEXTURES];
  ~Custom() {
    for (auto &p : programs) p.destroy();
    for (auto &t : raw) t.destroy();
    for (auto &screen : targets) for (auto &resolution : screen)
      for (auto &t : resolution) t.destroy();
    drastic_custom_shader_destroy(&spec);
  }
};
struct DfxParameters {
  float texture_size[4], target_size[2], time, padding;
};
struct DrawParameters {
  int effect, mode;
  float texture_size[2], target_size[2], padding[2], color[4];
};
static_assert(sizeof(DfxParameters) == 32 && sizeof(DrawParameters) == 48);
struct Vertex { float x, y, u, v; };

DkDevice g_device{};
DkQueue g_queue{};
DkSwapchain g_swapchain{};
DkMemBlock g_core_memory{};
const void *g_core_allocation{};
DkFence g_copy_fences[2]{};
Slot g_slots[kSlots];
Image g_present[kMaxImages], g_sources[2][2][2], g_work[2][2][3];
Image g_area, g_search, g_overlay[2];
Image *g_screen[2]{};
Program g_programs[kPresenter + 1];
std::unique_ptr<Custom> g_custom;
std::unique_ptr<DrasticDekoLsfg> g_lsfg;
bool g_lsfg_requested{};
DrasticScreenRect g_lsfg_rects[3]{};
int g_lsfg_screen_count = -1, g_lsfg_rotation = -1;
unsigned g_frames{}, g_data_size{}, g_overlay_index{};
unsigned g_image_count{};
uint64_t g_overlay_generation = UINT64_MAX;
bool g_suspended{}, g_failed{}, g_attached{};
bool g_filter_valid[2]{};
int g_filter = -1;
char g_error[512];

bool fail(const char *message) {
  std::snprintf(g_error, sizeof(g_error), "%s", message);
  std::printf("Deko3D: %s\n", g_error);
  return false;
}
void require_healthy_renderer() {
  if (g_failed) fatal_error("Deko3D renderer failed:\n%s", g_error);
}
void debug(void *, const char *context, DkResult result, const char *message) {
  std::snprintf(g_error, sizeof(g_error), "%s: %s (%u)", context,
                message, unsigned(result));
  std::printf("Deko3D: %s\n", g_error);
  if (result != DkResult_Success) g_failed = true;
}
DkMemBlock memory(size_t size, unsigned flags, void *storage = nullptr) {
  if (!size || align_up(size, DK_MEMBLOCK_ALIGNMENT) > UINT32_MAX) return nullptr;
  DkMemBlockMaker maker;
  dkMemBlockMakerDefaults(&maker, g_device, align_up(size, DK_MEMBLOCK_ALIGNMENT));
  maker.flags = flags; maker.storage = storage;
  return dkMemBlockCreate(&maker);
}
bool create_image(Image &t, unsigned w, unsigned h, DkImageFormat format,
                  unsigned flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine) {
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, g_device);
  maker.format = format; maker.flags = flags;
  maker.dimensions[0] = w; maker.dimensions[1] = h;
  DkImageLayout layout; dkImageLayoutInitialize(&layout, &maker);
  t.memory = memory(dkImageLayoutGetSize(&layout),
                    DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!t.memory) return fail("Could not allocate native image memory");
  dkImageInitialize(&t.image, &layout, t.memory, 0);
  t.width = w; t.height = h;
  return !g_failed;
}
bool load_program(Program &p, const void *vert, size_t vs,
                   const void *frag, size_t fs) {
  const void *blobs[] = {vert, frag}; const size_t sizes[] = {vs, fs};
  for (unsigned stage = 0; stage < 2; stage++) {
    if (!drastic_dksh_valid(blobs[stage], sizes[stage], stage ? 4 : 0))
      return fail("Invalid native vertex/fragment shader");
    DrasticDkshHeader header;
    std::memcpy(&header, blobs[stage], sizeof(header));
    p.code[stage] = memory(header.code_size + DK_SHADER_CODE_UNUSABLE_SIZE,
                           kCpuGpu | DkMemBlockFlags_Code);
    if (!p.code[stage]) return fail("Could not allocate native shader code");
    std::memcpy(dkMemBlockGetCpuAddr(p.code[stage]),
                static_cast<const uint8_t *>(blobs[stage]) + header.control_size,
                header.code_size);
    DkShaderMaker maker; dkShaderMakerDefaults(&maker, p.code[stage], 0);
    maker.control = blobs[stage];
    dkShaderInitialize(&p.shaders[stage], &maker);
    if (!dkShaderIsValid(&p.shaders[stage]) ||
        dkShaderGetStage(&p.shaders[stage]) != (stage ? DkStage_Fragment : DkStage_Vertex))
      return fail("The GPU rejected a native shader");
  }
  return !g_failed;
}
void wait_core_copy(void *fence) {
  /* This runs on the producer thread under its framebuffer mutex. The command
   * list was already flushed before publishing this lease. Never reuse a bank
   * until DMA has finished reading it, including during fast-forward. */
  const DkResult result = dkFenceWait(static_cast<DkFence *>(fence), 5000000000LL);
  if (result != DkResult_Success) fatalThrow(MAKERESULT(Module_Libnx, LibnxError_IoError));
}
void idle() { if (g_queue) dkQueueWaitIdle(g_queue); }
bool begin(Slot &s, int64_t timeout = 50000000) {
  if (s.pending) {
    const auto result = dkFenceWait(&s.fence, timeout);
    if (result == DkResult_Timeout) return false;
    if (result != DkResult_Success) { g_failed = true; return fail("GPU frame fence failed"); }
  }
  s.pending = false; s.used = 0; s.descriptor_count = 0;
  dkCmdBufClear(s.cmd);
  dkCmdBufAddMemory(s.cmd, s.commands, 0, kCommandBytes);
  return true;
}
void submit(Slot &s) {
  dkCmdBufSignalFence(s.cmd, &s.fence, true);
  dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(s.cmd));
  dkQueueFlush(g_queue);
  s.pending = true;
}
unsigned reserve(Slot &s, unsigned bytes, unsigned alignment) {
  const auto offset = unsigned(align_up(s.used, alignment));
  if (offset > g_data_size || bytes > g_data_size - offset) {
    g_failed = true; fail("Native frame data arena exhausted"); return UINT32_MAX;
  }
  s.used = offset + bytes;
  return offset;
}
void copy_image(DkCmdBuf cmd, Image &image, DkGpuAddr source, unsigned stride) {
  const DkCopyBuf buffer{source, stride, 0};
  const auto view = image.view();
  const DkImageRect rect{0, 0, 0, image.width, image.height, 1};
  dkCmdBufCopyBufferToImage(cmd, &buffer, &view, &rect, 0);
  image.valid = true;
}
bool upload_once(Image &image, const uint8_t *pixels) {
  const size_t bytes = size_t(image.width) * image.height * 4;
  DkMemBlock staging = memory(bytes, kCpuGpu);
  if (!staging) return fail("Could not allocate shader texture staging memory");
  std::memcpy(dkMemBlockGetCpuAddr(staging), pixels, bytes);
  Slot &s = g_slots[0];
  if (!begin(s)) { dkMemBlockDestroy(staging); return false; }
  copy_image(s.cmd, image, dkMemBlockGetGpuAddr(staging), image.width * 4);
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
  submit(s); idle();
  dkMemBlockDestroy(staging);
  return !g_failed;
}
void target(Slot &s, Image &t, bool clear = false, bool opaque_clear = false) {
  const auto view = t.view(); const DkImageView *views[] = {&view};
  dkCmdBufBindRenderTargets(s.cmd, views, 1, nullptr);
  const DkViewport viewport{0, 0, float(t.width), float(t.height), 0, 1};
  const DkScissor scissor{0, 0, t.width, t.height};
  dkCmdBufSetViewports(s.cmd, 0, &viewport, 1);
  dkCmdBufSetScissors(s.cmd, 0, &scissor, 1);
  if (clear) {
    const float black[] = {0, 0, 0, opaque_clear ? 1.f : 0.f};
    dkCmdBufClearColor(s.cmd, 0, DkColorMask_RGBA, black);
  }
}
void state(Slot &s) {
  DkRasterizerState raster; dkRasterizerStateDefaults(&raster);
  raster.cullMode = DkFace_None;
  DkDepthStencilState depth; dkDepthStencilStateDefaults(&depth);
  DkMultisampleState samples; dkMultisampleStateDefaults(&samples);
  DkColorWriteState write; dkColorWriteStateDefaults(&write);
  DkBlendState blend; dkBlendStateDefaults(&blend);
  blend.dstAlphaBlendFactor = DkBlendFactor_InvSrcAlpha;
  dkCmdBufBindRasterizerState(s.cmd, &raster);
  dkCmdBufBindDepthStencilState(s.cmd, &depth);
  dkCmdBufBindMultisampleState(s.cmd, &samples);
  dkCmdBufBindColorWriteState(s.cmd, &write);
  dkCmdBufBindBlendStates(s.cmd, 0, &blend, 1);
  DkVtxAttribState attributes[2]{};
  for (auto &a : attributes) { a.size = DkVtxAttribSize_2x32; a.type = DkVtxAttribType_Float; }
  attributes[1].offset = 8;
  const DkVtxBufferState vertex{sizeof(Vertex), 0};
  dkCmdBufBindVtxAttribState(s.cmd, attributes, 2);
  dkCmdBufBindVtxBufferState(s.cmd, &vertex, 1);
  const auto addr = dkMemBlockGetGpuAddr(s.descriptors);
  dkCmdBufBindImageDescriptorSet(s.cmd, addr, kDescriptors);
  dkCmdBufBindSamplerDescriptorSet(s.cmd, addr + kDescriptors * sizeof(DkImageDescriptor), 4);
  dkCmdBufBarrier(s.cmd, DkBarrier_None,
      DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
}
bool draw(Slot &s, Image &out, const DrasticScreenRect &rect, int rotation,
           Program &program, Image *const *inputs, unsigned count,
           const unsigned *samplers, const void *parameters, unsigned parameter_size,
           bool blend = false) {
  if (rect.width <= 0 || rect.height <= 0 || count > 16) return true;
  if (s.descriptor_count + count > kDescriptors)
    return fail("Native image descriptor arena exhausted");
  const unsigned vo = reserve(s, 6 * sizeof(Vertex), 16);
  const unsigned uo = reserve(s, 256, 256);
  if (vo == UINT32_MAX || uo == UINT32_MAX) return false;
  auto *base = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s.data));
  auto *vertices = reinterpret_cast<Vertex *>(base + vo);
  static constexpr float corners[6][2] = {{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}};
  for (unsigned i = 0; i < 6; i++) {
    const auto u = corners[i][0], v = corners[i][1];
    vertices[i].x = 2 * (rect.x + u * rect.width) / out.width - 1;
    /* Deko's UpperLeft origin still has clip-space +Y pointing up. Its
     * viewport flips that axis; Vulkan's positive-height viewport does not. */
    vertices[i].y = 1 - 2 * (rect.y + v * rect.height) / out.height;
    drastic_rotation_display_to_source(rotation, u, v, &vertices[i].u, &vertices[i].v);
  }
  std::memcpy(base + uo, parameters, parameter_size);
  const DkBufExtents vertex{dkMemBlockGetGpuAddr(s.data) + vo, 6 * sizeof(Vertex)};
  const DkBufExtents uniform{dkMemBlockGetGpuAddr(s.data) + uo, 256};
  const DkShader *shaders[] = {&program.shaders[0], &program.shaders[1]};
  dkCmdBufBindShaders(s.cmd, DkStageFlag_GraphicsMask, shaders, 2);
  dkCmdBufBindVtxBuffers(s.cmd, 0, &vertex, 1);
  dkCmdBufBindUniformBuffers(s.cmd, DkStage_Vertex, 0, &uniform, 1);
  dkCmdBufBindUniformBuffers(s.cmd, DkStage_Fragment, 0, &uniform, 1);
  auto *descriptors = static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(s.descriptors));
  DkResHandle handles[16]{};
  for (unsigned i = 0; i < count; i++) {
    if (!inputs[i] || !inputs[i]->memory) return fail("Missing native filter input");
    const auto view = inputs[i]->view();
    const unsigned index = s.descriptor_count++;
    dkImageDescriptorInitialize(&descriptors[index], &view, false, false);
    handles[i] = dkMakeTextureHandle(index, samplers[i]);
  }
  dkCmdBufBindTextures(s.cmd, DkStage_Vertex, 0, handles, count);
  dkCmdBufBindTextures(s.cmd, DkStage_Fragment, 0, handles, count);
  DkColorState color; dkColorStateDefaults(&color);
  color.blendEnableMask = blend ? 1 : 0;
  dkCmdBufBindColorState(s.cmd, &color);
  dkCmdBufDraw(s.cmd, DkPrimitive_Triangles, 6, 1, 0, 0);
  return true;
}
DrasticScreenRect whole(const Image &t) { return {0, 0, float(t.width), float(t.height), 0, 0}; }
DfxParameters dfx_parameters(const Image &input, float width, float height) {
  return {{1.f/input.width, 1.f/input.height, float(input.width), float(input.height)},
          {width, height}, float(g_frames) / 60.f, 0};
}
Image *role(unsigned screen, DrasticDfxTextureRole r) {
  const unsigned resolution = g_screen[screen]->width == 512;
  switch (r) {
    case DRASTIC_DFX_SOURCE: return g_screen[screen];
    case DRASTIC_DFX_WORK_A: return &g_work[screen][resolution][0];
    case DRASTIC_DFX_WORK_B: return &g_work[screen][resolution][1];
    case DRASTIC_DFX_WORK_2X: return &g_work[screen][resolution][2];
    case DRASTIC_DFX_AREA: return &g_area;
    case DRASTIC_DFX_SEARCH: return &g_search;
  }
  return nullptr;
}
Image *custom_input(unsigned screen, unsigned index) {
  const auto &spec = g_custom->spec.textures[index];
  if (spec.kind == DRASTIC_CUSTOM_TEXTURE_FRAMEBUFFER) return g_screen[screen];
  if (spec.kind == DRASTIC_CUSTOM_TEXTURE_RAW) return &g_custom->raw[index];
  return &g_custom->targets[screen][g_screen[screen]->width == 512][index];
}
bool custom_pass(Slot &s, unsigned screen, unsigned pass_index, Image &out,
                  const DrasticScreenRect &rect, int rotation) {
  const auto &pass = g_custom->spec.passes[pass_index];
  Image *inputs[16]{}; unsigned filtering[16]{};
  for (int i = 0; i < pass.sampler_count; i++) {
    const auto index = pass.sampler_textures[i];
    inputs[i] = custom_input(screen, index);
    filtering[i] = (g_custom->spec.textures[index].min_linear ? 1u : 0u) | (g_custom->spec.textures[index].mag_linear ? 2u : 0u);
  }
  auto params = dfx_parameters(*inputs[0], (rotation & 1) ? rect.height : rect.width,
                                           (rotation & 1) ? rect.width : rect.height);
  return draw(s, out, rect, rotation, g_custom->programs[pass_index], inputs,
              pass.sampler_count, filtering, &params, sizeof(params));
}
bool filter_screen(Slot &s, unsigned screen, const DrasticRuntimeConfig &config) {
  if (config.video_filter == DRASTIC_FILTER_CUSTOM && g_custom) {
    for (int i = 0; i + 1 < g_custom->spec.pass_count; i++) {
      auto &out = *custom_input(screen, g_custom->spec.passes[i].output_texture);
      target(s, out, true);
      if (!custom_pass(s, screen, i, out, whole(out), 0)) return false;
      dkCmdBufBarrier(s.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    }
    return true;
  }
  if (g_filter_valid[screen]) return true;
  const auto &chain = *drastic_dfx_chain(config.video_filter);
  for (unsigned i = 0; i < chain.pass_count; i++) {
    const auto &pass = chain.passes[i];
    Image &out = *role(screen, pass.output);
    Image *inputs[3]{}; unsigned filtering[3]{};
    for (unsigned n = 0; n < pass.input_count; n++) {
      inputs[n] = role(screen, pass.inputs[n].texture);
      filtering[n] = pass.inputs[n].sampler == DRASTIC_DFX_LINEAR ? 3 : 0;
    }
    auto params = dfx_parameters(*inputs[0], out.width, out.height);
    target(s, out, true);
    if (!draw(s, out, whole(out), 0, g_programs[pass.shader], inputs,
               pass.input_count, filtering, &params, sizeof(params))) return false;
    dkCmdBufBarrier(s.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
  }
  g_filter_valid[screen] = true;
  return true;
}
bool final_screen(Slot &s, Image &out, const DrasticScreenRect &rect,
                    const DrasticRuntimeConfig &config) {
  if (rect.screen < 0 || rect.screen > 1 || !g_screen[rect.screen] ||
      !g_screen[rect.screen]->valid) return true;
  if (config.video_filter == DRASTIC_FILTER_CUSTOM && g_custom)
    return custom_pass(s, rect.screen, g_custom->spec.pass_count - 1,
                        out, rect, config.rotation);
  const auto &chain = *drastic_dfx_chain(config.video_filter);
  Image *input = role(rect.screen, chain.final_texture);
  const unsigned filtering = chain.final_sampler == DRASTIC_DFX_LINEAR ? 3 : 0;
  if (chain.final_shader == DRASTIC_DFX_SCANLINE) {
    /* Use the Vulkan presenter's established scanline math, compiled to native
     * Maxwell code. It has one UV varying and explicit level-zero sampling,
     * avoiding the corrupt legacy scanline program on the native driver. */
    DrawParameters params{};
    params.effect = 3; params.mode = 2;
    params.texture_size[0] = input->width; params.texture_size[1] = input->height;
    params.target_size[0] = (config.rotation & 1) ? rect.height : rect.width;
    params.target_size[1] = (config.rotation & 1) ? rect.width : rect.height;
    for (auto &component : params.color) component = 1;
    return draw(s, out, rect, config.rotation, g_programs[kPresenter],
                &input, 1, &filtering, &params, sizeof(params));
  }
  auto params = dfx_parameters(*input,
      (config.rotation & 1) ? rect.height : rect.width,
      (config.rotation & 1) ? rect.width : rect.height);
  return draw(s, out, rect, config.rotation, g_programs[chain.final_shader],
              &input, 1, &filtering, &params, sizeof(params));
}
bool presentation_draw(Slot &s, Image &out, const DrasticScreenRect &rect,
                         Image *input, int mode, const float *color, int rotation = 0) {
  DrawParameters params{};
  params.effect = mode == 0 ? 1 : 0;
  params.mode = mode;
  params.texture_size[0] = input->width; params.texture_size[1] = input->height;
  std::memcpy(params.color, color, sizeof(params.color));
  const unsigned sampler = mode == 0 ? 3 : 0;
  return draw(s, out, rect, rotation, g_programs[kPresenter], &input, 1, &sampler,
              &params, sizeof(params), true);
}
bool draw_ui(Slot &s, Image &out, const DrasticRuntimeConfig &config,
              const DrasticOverlayFrame *overlay) {
  const float white[] = {1,1,1,1}, black[] = {0,0,0,1}, cyan[] = {0.25f,0.9f,1,1};
  if (config.stylus_visible) {
    float x, y;
    if (drastic_config_map_stylus(&config, config.stylus_x, config.stylus_y, &x, &y)) {
      const float radius = out.width >= 1600 ? 10.f : 7.f;
      const DrasticScreenRect cross[] = {
        {x-radius,y-2,2*radius+1,5,0,0},{x-2,y-radius,5,2*radius+1,0,0},
        {x-radius+1,y-1,2*radius-1,3,0,0},{x-1,y-radius+1,3,2*radius-1,0,0}};
      for (unsigned i = 0; i < 4; i++)
        if (!presentation_draw(s, out, cross[i], &g_area, 1, i < 2 ? black : cyan)) return false;
    }
  }
  return !overlay || !overlay->visible || !overlay->pixels ||
      presentation_draw(s, out, whole(out), &g_overlay[g_overlay_index], 0, white, config.rotation);
}
void copy_frame(Slot &s, const DkImage *image, Image &out) {
  DkImageView source; dkImageViewDefaults(&source, image);
  const auto destination = out.view();
  const DkImageRect rect{0, 0, 0, out.width, out.height, 1};
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
  dkCmdBufCopyImage(s.cmd, &source, &rect, &destination, &rect, 0);
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
  state(s); // Compute uses a different descriptor set; restore graphics state.
  target(s, out);
}
bool update_overlay(Slot &s, const DrasticOverlayFrame *overlay) {
  if (!overlay || !overlay->visible || !overlay->pixels) return true;
  const unsigned orientation = overlay->height > overlay->width;
  auto &image = g_overlay[orientation];
  if (unsigned(overlay->width) != image.width || unsigned(overlay->height) != image.height)
    return fail("Overlay dimensions do not match the output");
  g_overlay_index = orientation;
  if (image.valid && g_overlay_generation == overlay->generation) return true;
  const unsigned bytes = image.width * image.height * 4;
  const unsigned offset = reserve(s, bytes, 256);
  if (offset == UINT32_MAX) return false;
  std::memcpy(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s.data)) + offset,
              overlay->pixels, bytes);
  copy_image(s.cmd, image, dkMemBlockGetGpuAddr(s.data) + offset, image.width * 4);
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
  g_overlay_generation = overlay->generation;
  return true;
}
bool attach_core() {
  DrasticNativeFrame frame;
  if (!drastic_native_frame_lock(&frame)) return fail("Unsupported native framebuffer state");
  g_core_allocation = frame.allocation;
  g_core_memory = memory(DRASTIC_NATIVE_POOL_BYTES,
      DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached, const_cast<void *>(frame.allocation));
  drastic_native_frame_unlock(nullptr, nullptr, 0);
  g_attached = g_core_memory != nullptr;
  return g_attached || fail("Could not map the core framebuffer allocation for native DMA");
}
bool upload_core(Slot &s, DrasticNativeFrame &frame, unsigned &uploaded) {
  if (frame.allocation != g_core_allocation)
    return fail("The core framebuffer allocation changed unexpectedly");
  for (unsigned screen = 0; screen < 2; screen++) {
    auto &image = g_sources[screen][frame.width[screen] == 512][frame.bytes_per_pixel == 4];
    if (image.valid && g_screen[screen] == &image && !(frame.dirty & (1u << screen))) continue;
    const size_t offset = frame.bank * DRASTIC_NATIVE_BANK_BYTES + screen * DRASTIC_NATIVE_SCREEN_BYTES;
    const size_t bytes = size_t(frame.width[screen]) * frame.height[screen] * frame.bytes_per_pixel;
    if (dkMemBlockFlushCpuCache(g_core_memory, offset, bytes) != DkResult_Success)
      return fail("Could not flush the native framebuffer CPU cache");
    copy_image(s.cmd, image, dkMemBlockGetGpuAddr(g_core_memory) + offset,
                frame.width[screen] * frame.bytes_per_pixel);
    if (g_lsfg && g_screen[screen] != &image) g_lsfg->reset();
    g_screen[screen] = &image;
    g_filter_valid[screen] = false;
    uploaded |= 1u << screen;
  }
  if (uploaded) {
    dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    dkCmdBufSignalFence(s.cmd, &g_copy_fences[frame.bank], true);
  }
  return true;
}
} // namespace

extern "C" bool drastic_deko_renderer_init(const DrasticRuntimeConfig *config) {
  g_error[0] = 0; g_failed = false; g_suspended = false; g_frames = 0;
  DkDeviceMaker dm; dkDeviceMakerDefaults(&dm); dm.cbDebug = debug;
  g_device = dkDeviceCreate(&dm);
  if (!g_device) return fail("Could not create a Deko3D device");
  DkQueueMaker qm; dkQueueMakerDefaults(&qm, g_device);
  qm.flags = DkQueueFlags_Graphics | DkQueueFlags_DisableZcull;
  const bool lsfg_prepared = prefs_get_bool("Wrapper/LSFGEnabled", false);
  /* Like SA2's double-buffer option, leave one image on scanout and one being
   * produced. Acquire then paces capture of the newest completed core bank,
   * rather than letting a third image queue another source frame. LSFG's real
   * frame stays in its separate history pool: its two outputs are acquired and
   * presented one at a time, so they can also use this shorter display queue. */
  const bool low_latency = prefs_get_bool("Wrapper/LowLatency", true);
  g_image_count = low_latency ? 2 : kMaxImages;
  if (lsfg_prepared) qm.flags |= DkQueueFlags_Compute;
  qm.commandMemorySize = 512 * 1024; qm.flushThreshold = 64 * 1024;
  qm.perWarpScratchMemorySize = 65536;
  g_queue = dkQueueCreate(&qm);
  if (!g_queue) return fail("Could not create a native graphics queue");
  g_data_size = unsigned(align_up(size_t(DRASTIC_OVERLAY_WIDTH) * DRASTIC_OVERLAY_HEIGHT * 4 + kDrawBytes, 4096));
  for (auto &s : g_slots) {
    DkCmdBufMaker cm; dkCmdBufMakerDefaults(&cm, g_device);
    s.cmd = dkCmdBufCreate(&cm);
    s.commands = memory(kCommandBytes, kCpuGpu);
    s.data = memory(g_data_size, kCpuGpu);
    s.descriptors = memory(kDescriptors * sizeof(DkImageDescriptor) +
                          4 * sizeof(DkSamplerDescriptor), kCpuGpu);
    if (!s.cmd || !s.commands || !s.data || !s.descriptors) return fail("Could not create frame resources");
    auto *samplers = reinterpret_cast<DkSamplerDescriptor *>(
        static_cast<uint8_t *>(dkMemBlockGetCpuAddr(s.descriptors)) + kDescriptors * sizeof(DkImageDescriptor));
    for (unsigned i = 0; i < 4; i++) {
      DkSampler sampler; dkSamplerDefaults(&sampler);
      sampler.minFilter = (i & 1) ? DkFilter_Linear : DkFilter_Nearest;
      sampler.magFilter = (i & 2) ? DkFilter_Linear : DkFilter_Nearest;
      sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
      dkSamplerDescriptorInitialize(&samplers[i], &sampler);
    }
  }
  const DkImage *images[kMaxImages];
  for (unsigned i = 0; i < g_image_count; i++) {
    if (!create_image(g_present[i], panel_width, panel_height, DkImageFormat_RGBA8_Unorm,
          DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)) return false;
    images[i] = &g_present[i].image;
  }
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, g_device, nwindowGetDefault(), images, g_image_count);
  /* A partial native window setup must never fall back to consoleInit. */
  fatal_error_set_graphics_active(1);
  g_swapchain = dkSwapchainCreate(&sm);
  if (!g_swapchain) return fail("Could not create a native swapchain");
  dkSwapchainSetSwapInterval(g_swapchain, 1);
  for (unsigned i = 0; i <= kPresenter; i++) {
    const auto &p = deko_programs[i];
    if (!load_program(g_programs[i], p.vertex, p.vertex_size, p.fragment, p.fragment_size)) return false;
  }
  for (unsigned screen = 0; screen < 2; screen++) for (unsigned resolution = 0; resolution < 2; resolution++) {
    const unsigned w = 256u << resolution, h = 192u << resolution;
    for (unsigned format = 0; format < 2; format++) {
      if (!create_image(g_sources[screen][resolution][format], w, h,
            format ? DkImageFormat_RGBA8_Unorm : DkImageFormat_BGR565_Unorm,
            DkImageFlags_Usage2DEngine)) return false;
      g_sources[screen][resolution][format].opaque = true;
    }
    for (unsigned work = 0; work < 3; work++) {
      const unsigned scale = work == 2 ? 2 : 1;
      if (!create_image(g_work[screen][resolution][work], w * scale, h * scale,
                        DkImageFormat_RGBA8_Unorm)) return false;
    }
  }
  for (unsigned i = 0; i < 2; i++)
    if (!create_image(g_overlay[i], i ? DRASTIC_OVERLAY_HEIGHT : DRASTIC_OVERLAY_WIDTH,
        i ? DRASTIC_OVERLAY_WIDTH : DRASTIC_OVERLAY_HEIGHT, DkImageFormat_BGRA8_Unorm)) return false;
  const uint8_t *tables[] = {drastic_smaa_area_rgb_bin, drastic_smaa_search_rgb_bin};
  Image *lookup[] = {&g_area, &g_search};
  const unsigned widths[] = {160, 64}, heights[] = {560, 16};
  for (unsigned i = 0; i < 2; i++) {
    if (!create_image(*lookup[i], widths[i], heights[i], DkImageFormat_RGBA8_Unorm)) return false;
    std::vector<uint8_t> rgba(size_t(widths[i]) * heights[i] * 4, 255);
    for (size_t pixel = 0; pixel < rgba.size() / 4; pixel++)
      std::memcpy(rgba.data() + pixel * 4, tables[i] + pixel * 3, 3);
    if (!upload_once(*lookup[i], rgba.data())) return false;
  }
  if (!attach_core()) return false;
  if (config->custom_shader[0] && !drastic_deko_renderer_set_custom_shader(
      config->custom_shader, nullptr, 0) && config->video_filter == DRASTIC_FILTER_CUSTOM)
    return false;
  if (lsfg_prepared) {
    std::string error;
    const unsigned flow = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f) == 0.5f ? 2 : 4;
    g_lsfg = DrasticDekoLsfg::create(g_device, panel_width, panel_height,
        prefs_get_bool("Wrapper/LSFGPerformance", true), flow,
        "sdmc:" DATA_ROOT "/lsfg/Lossless.dll", "sdmc:" CACHE_DIR "/lsfg-deko3d", error);
    if (!g_lsfg) return fail(error.c_str());
    g_lsfg_requested = true;
  }
  std::printf("Deko3D initialized: %dx%d, %u swapchain images, low latency %s, direct core framebuffer DMA\n",
              panel_width, panel_height, g_image_count, low_latency ? "on" : "off");
  return !g_failed;
}

extern "C" void drastic_deko_renderer_present(const DrasticRuntimeConfig *config,
    DrasticCoreRenderFrame, void *, void *, const DrasticOverlayFrame *overlay,
    bool consume_core_frame) {
  require_healthy_renderer();
  if (!g_queue || !g_attached || g_suspended || !config) return;
  if (dkQueueIsInErrorState(g_queue)) {
    g_failed = true; fail("Native graphics queue failed"); require_healthy_renderer(); return;
  }
  Slot &s = g_slots[g_frames % kSlots];
  if (!begin(s)) { require_healthy_renderer(); return; }
  const int image_index = dkQueueAcquireImage(g_queue, g_swapchain);
  if (image_index < 0 || unsigned(image_index) >= g_image_count) {
    g_failed = true; fail("Native swapchain acquire failed"); require_healthy_renderer(); return;
  }
  /* DMA writes must wait for the previous frame's texture reads. */
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
  DrasticNativeFrame frame{}; bool locked = false; unsigned uploaded = 0;
  if (consume_core_frame) {
    /* Acquire can wait for scanout. Select the latest bank only after it
     * returns; never retain a captured source frame through that wait. */
    locked = drastic_native_frame_lock(&frame);
    if (!locked || !upload_core(s, frame, uploaded)) {
      if (locked) drastic_native_frame_unlock(nullptr, nullptr, 0);
      g_failed = true;
      if (!g_error[0]) fail("Could not acquire the completed core screens");
      require_healthy_renderer(); return;
    }
  }
  state(s);
  bool ok = update_overlay(s, overlay);
  if (g_filter != config->video_filter) {
    g_filter_valid[0] = g_filter_valid[1] = false;
    g_filter = config->video_filter;
    if (g_lsfg) g_lsfg->reset();
  }
  if (g_lsfg && (g_lsfg_screen_count != config->screen_count ||
      g_lsfg_rotation != config->rotation ||
      std::memcmp(g_lsfg_rects, config->screens, sizeof(g_lsfg_rects)))) {
    g_lsfg->reset();
    g_lsfg_screen_count = config->screen_count; g_lsfg_rotation = config->rotation;
    std::memcpy(g_lsfg_rects, config->screens, sizeof(g_lsfg_rects));
  }
  bool needed[2]{};
  for (int i = 0; i < config->screen_count && i < 3; i++)
    if (unsigned(config->screens[i].screen) < 2) needed[config->screens[i].screen] = true;
  for (unsigned screen = 0; screen < 2 && ok; screen++)
    if (needed[screen] && g_screen[screen] && g_screen[screen]->valid)
      ok = filter_screen(s, screen, *config);
  Image &out = g_present[image_index];
  const bool generating = g_lsfg && g_lsfg_requested && consume_core_frame;
  if (g_lsfg && !generating) g_lsfg->reset();
  Image history;
  if (generating) {
    history.image = *g_lsfg->input();
    history.width = out.width; history.height = out.height;
  }
  Image &scene = generating ? history : out;
  target(s, scene, true, true);
  for (int i = 0; i < config->screen_count && i < 3 && ok; i++)
    ok = final_screen(s, scene, config->screens[i], *config);
  const DkImage *generated = nullptr, *original = nullptr;
  if (generating && ok) {
    std::string error;
    ok = g_lsfg->record(s.cmd, generated, original, error);
    if (!ok) fail(error.c_str());
    if (ok) copy_frame(s, generated ? generated : original, out);
  }
  if (ok) ok = draw_ui(s, out, *config, overlay);
  /* Also submit on a recording error: an early upload fence may already be in
   * this list. Publishing only a submitted fence keeps the producer safe. */
  dkCmdBufBarrier(s.cmd, DkBarrier_Full, 0);
  submit(s);
  if (locked) drastic_native_frame_unlock(uploaded ? &g_copy_fences[frame.bank] : nullptr,
                                          uploaded ? wait_core_copy : nullptr, uploaded);
  dkQueuePresentImage(g_queue, g_swapchain, image_index);
  if (!ok) g_failed = true;
  require_healthy_renderer();
  if (generated) {
    /* VI permits only one acquired image. Present the interpolated frame first,
     * then acquire its successor and present the preserved real frame. Keep a
     * separate fenced data/command slot so neither submission is overwritten. */
    Slot &restore = g_slots[(g_frames + 1) % kSlots];
    if (!begin(restore, 5000000000LL)) {
      g_failed = true; fail("LSFG original frame fence failed"); require_healthy_renderer();
    }
    const int real_index = dkQueueAcquireImage(g_queue, g_swapchain);
    if (real_index < 0 || unsigned(real_index) >= g_image_count) {
      g_failed = true; fail("LSFG original frame acquire failed"); require_healthy_renderer();
    }
    Image &real = g_present[real_index];
    copy_frame(restore, original, real);
    if (!draw_ui(restore, real, *config, overlay)) g_failed = true;
    dkCmdBufBarrier(restore.cmd, DkBarrier_Full, 0);
    submit(restore);
    dkQueuePresentImage(g_queue, g_swapchain, real_index);
    require_healthy_renderer();
  }
  g_frames++;
}

extern "C" bool drastic_deko_renderer_set_custom_shader(const char *path,
    char *error, size_t error_size) {
  if (error && error_size) error[0] = 0;
  if (g_custom && path && !std::strcmp(g_custom->spec.relative_path, path)) return true;
  auto next = std::make_unique<Custom>();
  if (!drastic_custom_shader_load(path,
        DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D | DRASTIC_CUSTOM_SHADER_LOAD_PIXELS,
        &next->spec, g_error, sizeof(g_error))) {
    if (error && error_size) std::snprintf(error, error_size, "%s", g_error);
    return false;
  }
  idle();
  bool ok = true; size_t total = 0;
  for (int i = 0; i < next->spec.pass_count && ok; i++) {
    const auto &p = next->spec.passes[i];
    ok = load_program(next->programs[i], p.vertex_deko3d, p.vertex_deko3d_size,
                       p.fragment_deko3d, p.fragment_deko3d_size);
  }
  for (int i = 0; i < next->spec.texture_count && ok; i++) {
    const auto &spec = next->spec.textures[i];
    if (spec.kind == DRASTIC_CUSTOM_TEXTURE_FRAMEBUFFER) continue;
    if (spec.kind == DRASTIC_CUSTOM_TEXTURE_TARGET) {
      for (unsigned screen = 0; screen < 2 && ok; screen++) for (unsigned r = 0; r < 2 && ok; r++) {
        const unsigned w = (256u << r) * spec.output_scale, h = (192u << r) * spec.output_scale;
        total += size_t(w) * h * 4;
        ok = total <= kCustomBudget && create_image(next->targets[screen][r][i], w, h, DkImageFormat_RGBA8_Unorm);
      }
      continue;
    }
    total += size_t(spec.width) * spec.height * 4;
    if (total > kCustomBudget) { ok = false; break; }
    std::vector<uint8_t> rgba(size_t(spec.width) * spec.height * 4);
    for (size_t pixel = 0; pixel < rgba.size() / 4; pixel++) {
      const auto *src = spec.pixels + pixel * spec.channels;
      auto *dst = rgba.data() + pixel * 4;
      switch (spec.format) {
        case DRASTIC_CUSTOM_FORMAT_ALPHA: dst[0]=dst[1]=dst[2]=255; dst[3]=src[0]; break;
        case DRASTIC_CUSTOM_FORMAT_LUMINANCE: dst[0]=dst[1]=dst[2]=src[0]; dst[3]=255; break;
        case DRASTIC_CUSTOM_FORMAT_LUMINANCE_ALPHA: dst[0]=dst[1]=dst[2]=src[0]; dst[3]=src[1]; break;
        case DRASTIC_CUSTOM_FORMAT_RED: dst[0]=src[0]; dst[1]=dst[2]=0; dst[3]=255; break;
        case DRASTIC_CUSTOM_FORMAT_RG: dst[0]=src[0]; dst[1]=src[1]; dst[2]=0; dst[3]=255; break;
        case DRASTIC_CUSTOM_FORMAT_RGB: std::memcpy(dst, src, 3); dst[3]=255; break;
        case DRASTIC_CUSTOM_FORMAT_RGBA: std::memcpy(dst, src, 4); break;
      }
    }
    ok = create_image(next->raw[i], spec.width, spec.height, DkImageFormat_RGBA8_Unorm) &&
         upload_once(next->raw[i], rgba.data());
  }
  if (!ok) {
    idle();
    if (total > kCustomBudget) fail("Custom native shader targets exceed the 256 MiB budget");
    if (error && error_size) std::snprintf(error, error_size, "%s", g_error);
    return false;
  }
  idle();
  g_custom = std::move(next);
  if (g_lsfg) g_lsfg->reset();
  g_filter_valid[0] = g_filter_valid[1] = false;
  g_error[0] = 0;
  return true;
}
extern "C" void drastic_deko_renderer_suspend() {
  idle(); if (g_lsfg) g_lsfg->reset(); g_suspended = true;
}
extern "C" void drastic_deko_renderer_resume() { g_suspended = false; g_overlay_generation = UINT64_MAX; }
extern "C" void drastic_deko_renderer_detach_core() {
  if (!g_attached) return;
  idle();
  drastic_native_frame_detach();
  if (g_core_memory) dkMemBlockDestroy(g_core_memory);
  g_core_memory = nullptr; g_core_allocation = nullptr; g_attached = false;
}
extern "C" void drastic_deko_renderer_shutdown() {
  drastic_deko_renderer_detach_core(); idle(); g_custom.reset();
  g_lsfg.reset(); g_lsfg_requested = false;
  g_lsfg_screen_count = g_lsfg_rotation = -1;
  if (g_swapchain) dkSwapchainDestroy(g_swapchain);
  g_swapchain = nullptr;
  for (auto &s : g_slots) {
    if (s.cmd) dkCmdBufDestroy(s.cmd);
    for (auto m : {s.commands, s.data, s.descriptors}) if (m) dkMemBlockDestroy(m);
    s = {};
  }
  for (auto &p : g_programs) p.destroy();
  for (auto &t : g_present) t.destroy();
  for (auto &screen : g_sources) for (auto &res : screen) for (auto &t : res) t.destroy();
  for (auto &screen : g_work) for (auto &res : screen) for (auto &t : res) t.destroy();
  for (auto &t : g_overlay) t.destroy();
  g_area.destroy(); g_search.destroy();
  if (g_queue) dkQueueDestroy(g_queue);
  if (g_device) dkDeviceDestroy(g_device);
  g_queue = nullptr; g_device = nullptr;
  g_image_count = 0;
  g_screen[0] = g_screen[1] = nullptr;
  g_overlay_generation = UINT64_MAX;
  g_filter_valid[0] = g_filter_valid[1] = false; g_filter = -1;
}
extern "C" unsigned drastic_deko_renderer_frame_count() { return g_frames; }
extern "C" bool drastic_deko_renderer_lsfg_available() { return g_lsfg != nullptr; }
extern "C" bool drastic_deko_renderer_lsfg_enabled() { return g_lsfg && g_lsfg_requested; }
extern "C" bool drastic_deko_renderer_lsfg_request_enabled(bool enabled) {
  if (enabled && !g_lsfg) return false;
  if (g_lsfg && enabled != g_lsfg_requested) g_lsfg->reset();
  g_lsfg_requested = enabled;
  return true;
}
extern "C" const char *drastic_deko_renderer_last_error() { return g_error; }
