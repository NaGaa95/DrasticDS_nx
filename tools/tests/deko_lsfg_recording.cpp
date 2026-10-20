// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the production integration against a recording deko API. This checks
// CPU-side lifetime, allocation and command contracts; it does not execute GPU code.
#include "../../source/lsfg/drastic_deko_lsfg.h"
#include <lsfg/common/dksh.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace dk::detail {
struct Device {};
struct MemBlock { uint32_t size, flags; std::vector<uint8_t> cpu; };
struct CmdBuf { unsigned dispatches{}, barriers{}, images{}, samplers{}; bool shader{}; };
}
namespace {
unsigned allocations{}, live_blocks{};
void require(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
struct Layout { uint64_t bytes; uint32_t flags, width, height; };
template<class T, class U> T metadata(const U *object) {
  static_assert(sizeof(T) <= sizeof(U)); T data; std::memcpy(&data, object, sizeof(T)); return data;
}
template<class T, class U> void store(U *object, T data) {
  static_assert(sizeof(T) <= sizeof(U)); std::memset(object, 0, sizeof(U)); std::memcpy(object, &data, sizeof(T));
}
}
extern "C" {
DkMemBlock dkMemBlockCreate(const DkMemBlockMaker *m) {
  require(m->size && !(m->size % DK_MEMBLOCK_ALIGNMENT), "unaligned memory block");
  ++allocations; ++live_blocks;
  auto *block = new dk::detail::MemBlock{m->size, m->flags, {}};
  // Image storage is never CPU accessed. Preserve its size without allocating it.
  if (!(m->flags & DkMemBlockFlags_Image)) block->cpu.resize(m->size);
  return block;
}
void dkMemBlockDestroy(DkMemBlock m) { --live_blocks; delete m; }
void *dkMemBlockGetCpuAddr(DkMemBlock m) { require(!m->cpu.empty(), "CPU access to GPU-only image pool"); return m->cpu.data(); }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock m) { return reinterpret_cast<uintptr_t>(m->cpu.data()); }
uint32_t dkMemBlockGetSize(DkMemBlock m) { return m->size; }
void dkImageLayoutInitialize(DkImageLayout *o, const DkImageLayoutMaker *m) {
  const unsigned pixel = m->format == DkImageFormat_RGBA16_Float ? 8 : m->format == DkImageFormat_R8_Unorm ? 1 : 4;
  const auto bytes = (uint64_t(m->dimensions[0]) * m->dimensions[1] * pixel + 4095) & ~uint64_t(4095);
  require(m->flags & DkImageFlags_UsageLoadStore, "compute image missing load/store usage");
  store(o, Layout{bytes, m->flags, m->dimensions[0], m->dimensions[1]});
}
uint64_t dkImageLayoutGetSize(const DkImageLayout *o) { return metadata<Layout>(o).bytes; }
uint32_t dkImageLayoutGetAlignment(const DkImageLayout *) { return 4096; }
void dkImageInitialize(DkImage *o, const DkImageLayout *l, DkMemBlock m, uint32_t offset) {
  const auto layout = metadata<Layout>(l);
  require(uint64_t(offset) + layout.bytes <= m->size && offset % 4096 == 0, "image exceeds memory pool");
  store(o, layout);
}
void dkShaderInitialize(DkShader *o, const DkShaderMaker *m) {
  lsfg::dksh::FileHeader h; std::memcpy(&h, m->control, sizeof(h));
  require(uint64_t(m->codeOffset) + h.code_size + DK_SHADER_CODE_UNUSABLE_SIZE <= m->codeMem->size,
          "shader code exceeds allocation");
  uint32_t type; std::memcpy(&type, static_cast<const uint8_t *>(m->control) + h.programs_offset, sizeof(type));
  require(type == 5, "non-compute shader"); store(o, type);
}
bool dkShaderIsValid(const DkShader *o) { return metadata<uint32_t>(o) == 5; }
DkStage dkShaderGetStage(const DkShader *) { return DkStage_Compute; }
void dkImageDescriptorInitialize(DkImageDescriptor *o, const DkImageView *v, bool storage, bool) {
  require(v->pImage != nullptr, "null descriptor image"); store(o, uint32_t(storage));
}
void dkSamplerDescriptorInitialize(DkSamplerDescriptor *o, const DkSampler *s) {
  require(s->minFilter == DkFilter_Linear && s->magFilter == DkFilter_Linear, "incorrect LSFG sampling");
  store(o, uint32_t(1));
}
void dkCmdBufBarrier(DkCmdBuf c, DkBarrier mode, uint32_t) { require(mode != DkBarrier_None, "empty barrier"); ++c->barriers; }
void dkCmdBufBindImageDescriptorSet(DkCmdBuf c, DkGpuAddr address, uint32_t count) {
  require(address && count, "empty image table"); c->images = count;
}
void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf c, DkGpuAddr address, uint32_t count) {
  require(address && count == 4, "incorrect sampler table"); c->samplers = count;
}
void dkCmdBufBindShaders(DkCmdBuf c, uint32_t mask, const DkShader *const shaders[], uint32_t count) {
  require(mask == DkStageFlag_Compute && count == 1 && dkShaderIsValid(shaders[0]), "invalid compute shader binding"); c->shader = true;
}
void dkCmdBufBindTextures(DkCmdBuf c, DkStage stage, uint32_t first, const DkResHandle h[], uint32_t count) {
  require(stage == DkStage_Compute && first == 0, "invalid texture range");
  for (unsigned i = 0; i < count; ++i) require((h[i] & 0xfffff) < c->images && (h[i] >> 20) < c->samplers, "invalid texture handle");
}
void dkCmdBufBindImages(DkCmdBuf c, DkStage stage, uint32_t first, const DkResHandle h[], uint32_t count) {
  require(stage == DkStage_Compute && first == 0, "invalid storage range");
  for (unsigned i = 0; i < count; ++i) require(h[i] < c->images, "invalid storage handle");
}
void dkCmdBufBindUniformBuffers(DkCmdBuf, DkStage stage, uint32_t, const DkBufExtents b[], uint32_t count) {
  require(stage == DkStage_Compute && count == 1 && b[0].addr && b[0].size == 256, "invalid constant buffer");
}
void dkCmdBufDispatchCompute(DkCmdBuf c, uint32_t x, uint32_t y, uint32_t z) {
  require(c->shader && c->images && c->samplers && x && y && z == 1 && c->barriers, "invalid dispatch state"); ++c->dispatches;
}
}

void validate_native_recording(const char *dll, const char *cache) {
  dk::detail::Device device;
  for (bool perf : {false, true}) for (unsigned flow : {2u, 4u}) for (unsigned width : {1280u, 1920u}) {
    std::string error;
    auto session = DrasticDekoLsfg::create(&device, width, width * 9 / 16, perf, flow, dll, cache, error);
    require(bool(session), error.c_str());
    dk::detail::CmdBuf cmd;
    unsigned generated_count = 0, allocated = allocations;
    for (unsigned i = 0; i < 24; ++i) {
      const auto *input = session->input();
      require(metadata<Layout>(input).flags & DkImageFlags_UsageRender, "history not renderable");
      const DkImage *generated, *original;
      require(session->record(&cmd, generated, original, error), error.c_str());
      require(input == original && generated != original, "real frame history was not preserved");
      require(allocations == allocated, "per-frame GPU allocation");
      if (generated) ++generated_count;
      if (i == 11) session->reset();
      if (i == 0 || i == 12) require(!generated, "uninitialized history presented after reset");
    }
    require(generated_count > 0 && cmd.dispatches > 24 && cmd.barriers > 48, "generation did not run");
    session.reset(); require(live_blocks == 0, "GPU block leak");
  }
  std::cout << "Production LSFG recording passed: 8 configurations, 192 frames, warmup/reset and allocation checks\n";
}
