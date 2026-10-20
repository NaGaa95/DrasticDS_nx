/* Compile on the host to exercise the real manifest/pack loader. The test root
 * is supplied by the package verifier; installed shaders are never modified. */
#include <cassert>
#include <cstdio>
#include <vector>
#include "../../source/config.h"
static const char *test_root;
#undef SHADERS_DIR
#define SHADERS_DIR test_root
#include "../../source/drastic_custom_shader.cpp"

int main(int argc, char **argv) {
  assert(argc == 2); test_root = argv[1];
  std::vector<DrasticCustomShaderEntry> entries(512);
  const size_t count = drastic_custom_shader_scan(entries.data(), entries.size());
  assert(count == 56);
  unsigned passes = 0, targets = 0, raw = 0;
  char error[512];
  for (size_t index = 0; index < count; index++) {
    const auto &entry = entries[index];
    assert(entry.vulkan_ready && entry.deko3d_ready);
    DrasticCustomShader shader{};
    if (!drastic_custom_shader_load(entry.relative_path,
        DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D | DRASTIC_CUSTOM_SHADER_LOAD_PIXELS,
        &shader, error, sizeof(error))) {
      std::fprintf(stderr, "%s: %s\n", entry.relative_path, error); return 1;
    }
    for (int pass = 0; pass < shader.pass_count; pass++) {
      const auto &p = shader.passes[pass];
      assert(drastic_dksh_valid(p.vertex_deko3d, p.vertex_deko3d_size, 0));
      assert(drastic_dksh_valid(p.fragment_deko3d, p.fragment_deko3d_size, 4));
      assert(p.sampler_count >= 1 && p.sampler_count <= 16);
      passes++;
    }
    for (int texture = 0; texture < shader.texture_count; texture++) {
      const auto &t = shader.textures[texture];
      if (t.kind == DRASTIC_CUSTOM_TEXTURE_TARGET) { assert(t.output_scale > 0); targets++; }
      if (t.kind == DRASTIC_CUSTOM_TEXTURE_RAW) { assert(t.pixels && t.pixels_size > 0); raw++; }
    }
    drastic_custom_shader_destroy(&shader);
  }
  assert(targets);
  const auto &entry = entries[0];
  const std::string pack = full_path(entry.relative_path) + ".nxdk/";
  std::vector<uint8_t> original;
  assert(read_file(pack + "pass0.frag.dksh", 8 * 1024 * 1024, original, error, sizeof(error)));
  FILE *file = std::fopen((pack + "pass0.frag.dksh").c_str(), "wb"); assert(file);
  assert(std::fwrite(original.data(), 1, original.size() - 1, file) == original.size() - 1);
  std::fclose(file);
  assert(!drastic_custom_shader_deko3d_ready(entry.relative_path, error, sizeof(error)) && error[0]);
  file = std::fopen((pack + "pass0.frag.dksh").c_str(), "wb"); assert(file);
  assert(std::fwrite(original.data(), 1, original.size(), file) == original.size()); std::fclose(file);
  const std::string marker = pack + "pack.info", backup = pack + "pack.info.test-backup";
  assert(std::rename(marker.c_str(), backup.c_str()) == 0);
  assert(!drastic_custom_shader_deko3d_ready(entry.relative_path, error, sizeof(error)) && error[0]);
  assert(std::rename(backup.c_str(), marker.c_str()) == 0);
  assert(drastic_custom_shader_deko3d_ready(entry.relative_path, error, sizeof(error)));
  std::printf("Custom loader: %zu bundled shaders, %u passes, %u targets, %u raw textures; corrupt and incomplete packs rejected\n",
              count, passes, targets, raw);
}
