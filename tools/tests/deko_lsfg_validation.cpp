// SPDX-License-Identifier: GPL-3.0-or-later
// Host validation of real DLL extraction, native compilation and GPU bindings.
// Takes a user-supplied DLL; neither it nor generated shaders belong in Git.
#include <lsfg/common/prepare.hpp>
#include <lsfg/common/dksh.hpp>
#include <lsfg/common/version.hpp>
#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/binding.hpp>
#include <lsfg/backend/schedule.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef DEKO3D_RECORDING_TEST
void validate_native_recording(const char *dll, const char *cache);
#endif

static void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
static void ok(lsfg::ErrorCode code, const char *message) {
  if (!lsfg::succeeded(code)) throw std::runtime_error(std::string(message) + ": " + std::string(lsfg::error_name(code)));
}
int main(int argc, char **argv) {
  try {
    check(argc == 3, "usage: deko_lsfg_validation Lossless.dll cache-root");
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    check(bool(file), "DLL open failed");
    std::vector<uint8_t> dll(static_cast<size_t>(file.tellg()));
    file.seekg(0); check(bool(file.read(reinterpret_cast<char *>(dll.data()), dll.size())), "DLL read failed");
    unsigned modules = 0, bindings = 0;
    for (bool performance : {false, true}) for (unsigned flow : {2u, 4u}) {
      lsfg::prepare::Options options;
      options.graph.performance = performance; options.graph.flow_denominator = flow;
      options.graph.generated_frames = 1;
      lsfg::prepare::Result result;
      ok(lsfg::prepare::run(dll, options, result, [](const auto &m) {
        std::cout << "Compiled " << m.name << ": " << m.dksh_bytes << " bytes, " << m.registers << " GPRs\n" << std::flush;
      }), "native preparation failed");
      const std::string directory = lsfg::prepare::directory_for(argv[2], result);
      ok(lsfg::cache::write(directory, result.contents), "cache write");
      lsfg::cache::Loaded cache;
      ok(lsfg::cache::read(directory, cache), "cache reload");
      check(lsfg::cache::same_modules(cache, result.contents), "cache round trip differs");
      for (const auto &pass : cache.passes) {
        lsfg::dksh::ComputeProgram program;
        ok(lsfg::dksh::validate(pass.dksh, program), "compute DKSH validation");
        auto corrupted = pass.dksh; corrupted[0] ^= 0xff;
        check(!lsfg::succeeded(lsfg::dksh::validate(corrupted, program)), "invalid DKSH accepted");
        ++modules;
      }
      lsfg::backend::Schedule schedule;
      ok(lsfg::backend::schedule(cache.graph, schedule), "schedule");
      check(schedule.generated_frames() == 1 && schedule.warmup_frames >= 1, "invalid 2x schedule");
      lsfg::backend::DescriptorLayout descriptors;
      ok(lsfg::backend::describe(cache.graph, descriptors), "descriptor layout");
      for (auto extent : {lsfg::graph::Extent{1280, 720}, lsfg::graph::Extent{1920, 1080}}) {
        lsfg::backend::Request request{.config = options.graph, .output = extent,
          .memory_budget_bytes = 192ull * 1024 * 1024};
        lsfg::backend::Plan plan; lsfg::backend::Rejection why;
        check(lsfg::backend::accept(cache, request, plan, why), why.reason.data());
        check(plan.max_scratch_bytes_per_warp <= 65536, "scratch exceeds queue capacity");
        for (unsigned phase = 0; phase < schedule.cycle; ++phase)
          for (unsigned i = 0; i < cache.graph.dispatches.size(); ++i) {
            lsfg::backend::DispatchBinding b;
            ok(lsfg::backend::bind(cache, plan, descriptors, i, phase, b), "dispatch binding");
            check(b.pass < cache.passes.size() && b.groups_x && b.groups_y, "invalid dispatch");
            for (unsigned t = 0; t < b.texture_count; ++t)
              check(b.textures[t].image < plan.images.size() && b.textures[t].descriptor < descriptors.image_descriptors &&
                    b.textures[t].sampler < lsfg::backend::sampler_descriptor_count, "invalid texture");
            for (unsigned t = 0; t < b.storage_count; ++t)
              check(b.storages[t].image < plan.images.size() && b.storages[t].descriptor < descriptors.image_descriptors, "invalid storage");
            if (b.uniform_slot != lsfg::backend::no_slot)
              check(b.uniform_buffer < plan.uniform_buffers, "invalid uniform");
            ++bindings;
          }
        request.memory_budget_bytes = 1;
        check(!lsfg::backend::accept(cache, request, plan, why), "image budget ignored");
      }
      // A damaged module must invalidate the on-disk cache, rather than reach deko.
      const auto module = std::filesystem::path(directory) / lsfg::cache::module_name(0);
      { std::fstream damaged(module, std::ios::binary | std::ios::in | std::ios::out); damaged.put('X'); }
      check(!lsfg::succeeded(lsfg::cache::read(directory, cache)), "corrupt cache accepted");
      ok(lsfg::cache::write(directory, result.contents), "repair test cache");
      std::cout << "Validated mode performance=" << performance << " flow=1/" << flow << '\n';
    }
    std::cout << "Native LSFG passed: " << modules << " modules, " << bindings << " phase/extent bindings\n";
#ifdef DEKO3D_RECORDING_TEST
    validate_native_recording(argv[1], argv[2]);
#endif
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
