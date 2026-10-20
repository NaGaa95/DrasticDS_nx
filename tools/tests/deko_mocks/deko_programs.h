// Synthetic graphics programs for API/lifetime tests, never executed on a GPU.
#pragma once
#include <array>
constexpr auto fixture_program(unsigned stage) {
  std::array<uint32_t,128> words{};
  words[0]=0x48534b44; words[1]=24; words[2]=256; words[3]=256; words[4]=24; words[5]=1;
  words[6]=stage; words[7]=48; words[8]=4;
  return words;
}
constexpr auto fixture_vertex=fixture_program(0), fixture_fragment=fixture_program(1);
struct FixtureProgram { const void *vertex; size_t vertex_size; const void *fragment; size_t fragment_size; };
static const auto deko_programs=[] {
  std::array<FixtureProgram,12> programs{};
  for(auto &p:programs) p={fixture_vertex.data(),512,fixture_fragment.data(),512};
  return programs;
}();
