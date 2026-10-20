#include "drastic_custom_shader.h"
#include "drastic_dksh.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

namespace {

constexpr size_t kMaxTextFile = 2u * 1024u * 1024u;
constexpr size_t kMaxStageSource = 4u * 1024u * 1024u;
constexpr size_t kMaxRawTexture = 64u * 1024u * 1024u;
constexpr size_t kMaxSpirv = 8u * 1024u * 1024u;

struct Section {
  std::string tag;
  std::string suffix;
  std::string body;
};

static void set_error(char *error, size_t size, const char *format, ...) {
  if (!error || !size) return;
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(error, size, format, arguments);
  va_end(arguments);
  error[size - 1] = '\0';
}

static std::string trim(const std::string &value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

static bool equals_ignore_case(const std::string &left,
                               const char *right) {
  if (!right || left.size() != std::strlen(right)) return false;
  for (size_t index = 0; index < left.size(); index++)
    if (std::tolower((unsigned char)left[index]) !=
        std::tolower((unsigned char)right[index])) return false;
  return true;
}

static bool has_suffix_ignore_case(const std::string &value,
                                   const char *suffix) {
  const size_t length = std::strlen(suffix);
  if (value.size() < length) return false;
  return equals_ignore_case(value.substr(value.size() - length), suffix);
}

static bool read_file(const std::string &path, size_t limit,
                      std::vector<uint8_t> &contents, char *error,
                      size_t error_size) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    set_error(error, error_size, "Could not open %s", path.c_str());
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    set_error(error, error_size, "Could not inspect %s", path.c_str());
    return false;
  }
  const long length = std::ftell(file);
  if (length < 0 || (size_t)length > limit) {
    std::fclose(file);
    set_error(error, error_size, "%s is empty or exceeds the size limit",
              path.c_str());
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    set_error(error, error_size, "Could not read %s", path.c_str());
    return false;
  }
  contents.resize((size_t)length);
  if (length && std::fread(contents.data(), 1, (size_t)length, file) !=
                    (size_t)length) {
    std::fclose(file);
    set_error(error, error_size, "Could not read %s", path.c_str());
    return false;
  }
  std::fclose(file);
  return true;
}

static bool read_text(const std::string &path, std::string &text,
                      char *error, size_t error_size) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, kMaxTextFile, bytes, error, error_size)) return false;
  if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) {
    set_error(error, error_size, "%s is not a text file", path.c_str());
    return false;
  }
  text.assign((const char *)bytes.data(), bytes.size());
  if (text.size() >= 3 && (uint8_t)text[0] == 0xef &&
      (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
    text.erase(0, 3);
  return true;
}

static std::vector<Section> parse_sections(const std::string &text) {
  std::vector<Section> sections;
  size_t cursor = 0;
  while ((cursor = text.find('<', cursor)) != std::string::npos) {
    if (cursor + 1 >= text.size() || text[cursor + 1] == '/' ||
        text[cursor + 1] == '!' || text[cursor + 1] == '?') {
      cursor++;
      continue;
    }
    const size_t open_end = text.find('>', cursor + 1);
    if (open_end == std::string::npos) break;
    std::string opening = trim(text.substr(cursor + 1,
                                           open_end - cursor - 1));
    const size_t colon = opening.find(':');
    const std::string tag = trim(opening.substr(0, colon));
    if (tag.empty() || !std::all_of(tag.begin(), tag.end(), [](char ch) {
          return std::isalnum((unsigned char)ch) || ch == '_';
        })) {
      cursor = open_end + 1;
      continue;
    }
    const std::string close = "</" + tag + ">";
    const size_t close_at = text.find(close, open_end + 1);
    if (close_at == std::string::npos) {
      cursor = open_end + 1;
      continue;
    }
    sections.push_back({tag,
                        colon == std::string::npos
                            ? std::string() : trim(opening.substr(colon + 1)),
                        text.substr(open_end + 1, close_at - open_end - 1)});
    cursor = close_at + close.size();
  }
  return sections;
}

static std::vector<std::pair<std::string, std::string>> key_values(
    const std::string &body) {
  std::vector<std::pair<std::string, std::string>> values;
  size_t cursor = 0;
  while (cursor <= body.size()) {
    const size_t end = body.find('\n', cursor);
    std::string line = body.substr(cursor, end == std::string::npos
        ? std::string::npos : end - cursor);
    const size_t comment = line.find("//");
    if (comment != std::string::npos) line.erase(comment);
    line = trim(line);
    const size_t equals = line.find('=');
    if (equals != std::string::npos) {
      std::string key = trim(line.substr(0, equals));
      std::string value = trim(line.substr(equals + 1));
      if (!key.empty()) values.emplace_back(std::move(key), std::move(value));
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  return values;
}

static std::string last_value(
    const std::vector<std::pair<std::string, std::string>> &values,
    const char *wanted) {
  std::string result;
  for (const auto &entry : values)
    if (entry.first == wanted) result = entry.second;
  return result;
}

static std::vector<std::string> section_bodies(
    const std::vector<Section> &sections, const char *tag) {
  std::vector<std::string> bodies;
  for (const Section &section : sections)
    if (section.tag == tag) bodies.push_back(section.body);
  return bodies;
}

static bool parse_integer(const std::string &text, int minimum, int maximum,
                          int &value) {
  if (text.empty()) return false;
  char *end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (!end || *end || parsed < minimum || parsed > maximum) return false;
  value = (int)parsed;
  return true;
}

static bool normalize_relative(const std::string &base,
                               const std::string &requested,
                               std::string &normalized) {
  if (requested.empty() || requested[0] == '/' ||
      requested.find(':') != std::string::npos) return false;
  for (unsigned char character : requested)
    if (character < 0x20 || character == 0x7f) return false;
  std::string joined = base.empty() ? requested : base + "/" + requested;
  std::replace(joined.begin(), joined.end(), '\\', '/');
  std::vector<std::string> components;
  size_t cursor = 0;
  while (cursor <= joined.size()) {
    const size_t slash = joined.find('/', cursor);
    const std::string component = joined.substr(cursor,
        slash == std::string::npos ? std::string::npos : slash - cursor);
    if (component.empty() || component == ".") {
      // Skip.
    } else if (component == "..") {
      if (components.empty()) return false;
      components.pop_back();
    } else {
      components.push_back(component);
    }
    if (slash == std::string::npos) break;
    cursor = slash + 1;
  }
  normalized.clear();
  for (const std::string &component : components) {
    if (!normalized.empty()) normalized += '/';
    normalized += component;
  }
  return !normalized.empty() &&
      normalized.size() < DRASTIC_CUSTOM_SHADER_PATH_MAX;
}

static std::string directory_of(const std::string &relative) {
  const size_t slash = relative.find_last_of('/');
  return slash == std::string::npos ? std::string() : relative.substr(0, slash);
}

static std::string full_path(const std::string &relative) {
  return std::string(SHADERS_DIR) + "/" + relative;
}

static std::string strip_versions(const std::string &source) {
  std::string result;
  size_t cursor = 0;
  while (cursor <= source.size()) {
    const size_t end = source.find('\n', cursor);
    const std::string line = source.substr(cursor, end == std::string::npos
        ? std::string::npos : end - cursor);
    const std::string clean = trim(line);
    if (clean.rfind("#version", 0) != 0) {
      result += line;
      result += '\n';
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
  }
  return result;
}

static bool append_stage_part(std::string &stage, const std::string &part,
                              char *error, size_t error_size) {
  if (stage.size() + part.size() + 1 > kMaxStageSource) {
    set_error(error, error_size, "Combined shader source exceeds 4 MiB");
    return false;
  }
  stage += part;
  stage += '\n';
  return true;
}

static bool duplicate_string(const std::string &source, char **target,
                             char *error, size_t error_size) {
  char *copy = (char *)std::malloc(source.size() + 1);
  if (!copy) {
    set_error(error, error_size, "Out of memory while loading shader source");
    return false;
  }
  std::memcpy(copy, source.c_str(), source.size() + 1);
  *target = copy;
  return true;
}

static bool parse_format(const std::string &value,
                         DrasticCustomPixelFormat &format, int &channels) {
  if (value == "GL_ALPHA") { format = DRASTIC_CUSTOM_FORMAT_ALPHA; channels = 1; }
  else if (value == "GL_LUMINANCE") { format = DRASTIC_CUSTOM_FORMAT_LUMINANCE; channels = 1; }
  else if (value == "GL_LUMINANCE_ALPHA") { format = DRASTIC_CUSTOM_FORMAT_LUMINANCE_ALPHA; channels = 2; }
  else if (value == "GL_RGB") { format = DRASTIC_CUSTOM_FORMAT_RGB; channels = 3; }
  else if (value == "GL_RGBA") { format = DRASTIC_CUSTOM_FORMAT_RGBA; channels = 4; }
  else if (value == "GL_RED") { format = DRASTIC_CUSTOM_FORMAT_RED; channels = 1; }
  else if (value == "GL_RG") { format = DRASTIC_CUSTOM_FORMAT_RG; channels = 2; }
  else return false;
  return true;
}

static bool parse_filter(const std::string &value, int &linear) {
  if (value.empty() || value == "GL_NEAREST") linear = 0;
  else if (value == "GL_LINEAR") linear = 1;
  else return false;
  return true;
}

static bool load_spirv_file(const std::string &path, uint8_t **data,
                            size_t *size, char *error, size_t error_size) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, kMaxSpirv, bytes, error, error_size)) return false;
  if (bytes.size() < 20 || (bytes.size() & 3) ||
      bytes[0] != 0x03 || bytes[1] != 0x02 || bytes[2] != 0x23 ||
      bytes[3] != 0x07) {
    set_error(error, error_size, "%s is not valid SPIR-V", path.c_str());
    return false;
  }
  uint8_t *copy = (uint8_t *)std::malloc(bytes.size());
  if (!copy) {
    set_error(error, error_size, "Out of memory while loading SPIR-V");
    return false;
  }
  std::memcpy(copy, bytes.data(), bytes.size());
  *data = copy;
  *size = bytes.size();
  return true;
}

static bool spirv_file_ready(const std::string &path, char *error,
                             size_t error_size) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    set_error(error, error_size, "Could not open %s", path.c_str());
    return false;
  }
  uint8_t header[4] = {};
  const bool magic = std::fread(header, 1, sizeof(header), file) ==
                         sizeof(header) &&
      header[0] == 0x03 && header[1] == 0x02 &&
      header[2] == 0x23 && header[3] == 0x07;
  const bool seek_ok = std::fseek(file, 0, SEEK_END) == 0;
  const long length = seek_ok ? std::ftell(file) : -1;
  std::fclose(file);
  if (!magic || length < 20 || ((unsigned long)length & 3) ||
      (size_t)length > kMaxSpirv) {
    set_error(error, error_size, "%s is not valid SPIR-V", path.c_str());
    return false;
  }
  return true;
}

static bool load_deko_file(const std::string &path, unsigned stage,
                           uint8_t **destination, size_t *size,
                           char *error, size_t error_size) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, kMaxSpirv, bytes, error, error_size)) return false;
  if (!drastic_dksh_valid(bytes.data(), bytes.size(), stage)) {
    set_error(error, error_size, "%s is not a supported native DKSH stage",
              path.c_str());
    return false;
  }
  *destination = (uint8_t *)std::malloc(bytes.size());
  if (!*destination) {
    set_error(error, error_size, "Out of memory loading %s", path.c_str());
    return false;
  }
  std::memcpy(*destination, bytes.data(), bytes.size());
  *size = bytes.size();
  return true;
}

static bool parse_manifest(const std::string &relative, unsigned flags,
                           DrasticCustomShader *shader, char *error,
                           size_t error_size) {
  std::string manifest_text;
  if (!read_text(full_path(relative), manifest_text, error, error_size))
    return false;
  const std::vector<Section> sections = parse_sections(manifest_text);
  const auto options = section_bodies(sections, "options");
  if (options.size() != 1) {
    set_error(error, error_size, "%s must contain one <options> section",
              relative.c_str());
    return false;
  }
  const auto option_values = key_values(options[0]);
  const std::string name = last_value(option_values, "name");
  int texture_count = 0;
  if (name.empty() || name.size() >= DRASTIC_CUSTOM_SHADER_NAME_MAX ||
      !parse_integer(last_value(option_values, "textures"), 1,
                     DRASTIC_CUSTOM_SHADER_MAX_TEXTURES, texture_count)) {
    set_error(error, error_size,
              "%s has invalid name or texture count", relative.c_str());
    return false;
  }
  std::snprintf(shader->name, sizeof(shader->name), "%s", name.c_str());
  std::snprintf(shader->relative_path, sizeof(shader->relative_path), "%s",
                relative.c_str());
  shader->texture_count = texture_count;

  bool texture_seen[DRASTIC_CUSTOM_SHADER_MAX_TEXTURES] = {};
  const std::string manifest_directory = directory_of(relative);
  for (const Section &section : sections) {
    if (section.tag != "texture") continue;
    int index = -1;
    if (!parse_integer(section.suffix, 0, texture_count - 1, index) ||
        texture_seen[index]) {
      set_error(error, error_size, "%s has an invalid texture index",
                relative.c_str());
      return false;
    }
    texture_seen[index] = true;
    DrasticCustomTexture &texture = shader->textures[index];
    const auto values = key_values(section.body);
    const std::string input = last_value(values, "input");
    if (input == "framebuffer") {
      texture.kind = DRASTIC_CUSTOM_TEXTURE_FRAMEBUFFER;
      texture.format = DRASTIC_CUSTOM_FORMAT_RGBA;
      texture.channels = 4;
    } else if (input == "null") {
      texture.kind = DRASTIC_CUSTOM_TEXTURE_TARGET;
      texture.format = DRASTIC_CUSTOM_FORMAT_RGBA;
      texture.channels = 4;
    } else {
      std::string raw_relative;
      if (!has_suffix_ignore_case(input, ".raw") ||
          !normalize_relative(manifest_directory, input, raw_relative)) {
        set_error(error, error_size, "%s texture %d has an unsafe input",
                  relative.c_str(), index);
        return false;
      }
      texture.kind = DRASTIC_CUSTOM_TEXTURE_RAW;
      if (!parse_integer(last_value(values, "width"), 1, 4096,
                         texture.width) ||
          !parse_integer(last_value(values, "height"), 1, 4096,
                         texture.height) ||
          !parse_format(last_value(values, "format"), texture.format,
                        texture.channels) ||
          last_value(values, "type") != "GL_UNSIGNED_BYTE") {
        set_error(error, error_size,
                  "%s texture %d has an unsupported raw format",
                  relative.c_str(), index);
        return false;
      }
      std::snprintf(texture.source_path, sizeof(texture.source_path), "%s",
                    raw_relative.c_str());
      const size_t expected = (size_t)texture.width * texture.height *
                              texture.channels;
      if (expected > kMaxRawTexture) {
        set_error(error, error_size, "%s texture %d is too large",
                  relative.c_str(), index);
        return false;
      }
      if (flags & DRASTIC_CUSTOM_SHADER_LOAD_PIXELS) {
        std::vector<uint8_t> pixels;
        if (!read_file(full_path(raw_relative), kMaxRawTexture, pixels,
                       error, error_size)) return false;
        if (pixels.size() != expected) {
          set_error(error, error_size,
                    "%s must contain exactly %lu bytes",
                    raw_relative.c_str(), (unsigned long)expected);
          return false;
        }
        texture.pixels = (uint8_t *)std::malloc(expected);
        if (!texture.pixels) {
          set_error(error, error_size,
                    "Out of memory while loading %s", raw_relative.c_str());
          return false;
        }
        std::memcpy(texture.pixels, pixels.data(), expected);
        texture.pixels_size = expected;
      }
    }
    if (!parse_filter(last_value(values, "min_filter"),
                      texture.min_linear) ||
        !parse_filter(last_value(values, "mag_filter"),
                      texture.mag_linear)) {
      set_error(error, error_size, "%s texture %d has an invalid filter",
                relative.c_str(), index);
      return false;
    }
  }
  for (int index = 0; index < texture_count; index++) {
    if (!texture_seen[index]) {
      set_error(error, error_size, "%s is missing texture %d",
                relative.c_str(), index);
      return false;
    }
  }

  std::vector<const Section *> pass_sections;
  for (const Section &section : sections)
    if (section.tag == "pass") pass_sections.push_back(&section);
  if (pass_sections.empty() ||
      pass_sections.size() > DRASTIC_CUSTOM_SHADER_MAX_PASSES) {
    set_error(error, error_size, "%s has an invalid pass count",
              relative.c_str());
    return false;
  }
  shader->pass_count = (int)pass_sections.size();

  std::string vertex_prefix;
  std::string fragment_prefix;
  if (flags & DRASTIC_CUSTOM_SHADER_LOAD_SOURCES) {
    for (const std::string &body : section_bodies(sections, "vheader"))
      if (!append_stage_part(vertex_prefix, body, error, error_size))
        return false;
    for (const std::string &body : section_bodies(sections, "fheader"))
      if (!append_stage_part(fragment_prefix, body, error, error_size))
        return false;
    for (const std::string &body : section_bodies(sections, "header")) {
      if (!append_stage_part(vertex_prefix, body, error, error_size) ||
          !append_stage_part(fragment_prefix, body, error, error_size))
        return false;
    }
    for (const std::string &body : section_bodies(sections, "include")) {
      const std::string include_name = last_value(key_values(body), "file");
      std::string include_relative;
      if (!normalize_relative(manifest_directory, include_name,
                              include_relative)) {
        set_error(error, error_size, "%s has an unsafe include path",
                  relative.c_str());
        return false;
      }
      std::string include_text;
      if (!read_text(full_path(include_relative), include_text,
                     error, error_size) ||
          !append_stage_part(vertex_prefix, include_text, error, error_size) ||
          !append_stage_part(fragment_prefix, include_text,
                             error, error_size)) return false;
    }
  }

  bool produced[DRASTIC_CUSTOM_SHADER_MAX_TEXTURES] = {};
  for (int index = 0; index < texture_count; index++)
    produced[index] = shader->textures[index].kind !=
                      DRASTIC_CUSTOM_TEXTURE_TARGET;
  for (int pass_index = 0; pass_index < shader->pass_count; pass_index++) {
    DrasticCustomPass &pass = shader->passes[pass_index];
    const auto values = key_values(pass_sections[pass_index]->body);
    const std::string shader_name = last_value(values, "shader");
    std::string shader_relative;
    if (!normalize_relative(manifest_directory, shader_name,
                            shader_relative) ||
        !has_suffix_ignore_case(shader_relative, ".dsd")) {
      set_error(error, error_size, "%s pass %d has an unsafe shader path",
                relative.c_str(), pass_index + 1);
      return false;
    }
    std::snprintf(pass.shader_path, sizeof(pass.shader_path), "%s",
                  shader_relative.c_str());
    for (const auto &entry : values) {
      constexpr const char prefix[] = "sampler:";
      if (entry.first.rfind(prefix, 0) != 0) continue;
      if (pass.sampler_count >= DRASTIC_CUSTOM_SHADER_MAX_SAMPLERS) {
        set_error(error, error_size, "%s pass %d has too many samplers",
                  relative.c_str(), pass_index + 1);
        return false;
      }
      const std::string sampler = entry.first.substr(sizeof(prefix) - 1);
      int texture_index = -1;
      if (sampler.empty() ||
          sampler.size() >= DRASTIC_CUSTOM_SHADER_SAMPLER_MAX ||
          !parse_integer(entry.second, 0, texture_count - 1,
                         texture_index) || !produced[texture_index]) {
        set_error(error, error_size,
                  "%s pass %d has an invalid sampler", relative.c_str(),
                  pass_index + 1);
        return false;
      }
      std::snprintf(pass.sampler_names[pass.sampler_count],
                    sizeof(pass.sampler_names[pass.sampler_count]), "%s",
                    sampler.c_str());
      pass.sampler_textures[pass.sampler_count++] = (uint8_t)texture_index;
    }
    if (!pass.sampler_count) {
      set_error(error, error_size, "%s pass %d has no samplers",
                relative.c_str(), pass_index + 1);
      return false;
    }
    pass.output_texture = -1;
    pass.output_scale = 1;
    if (pass_index + 1 < shader->pass_count) {
      const std::string output = last_value(values, "output");
      const size_t colon = output.find(':');
      int output_index = -1;
      int output_scale = 0;
      if (colon == std::string::npos ||
          !parse_integer(output.substr(0, colon), 0, texture_count - 1,
                         output_index) ||
          !parse_integer(output.substr(colon + 1), 1, 8, output_scale) ||
          shader->textures[output_index].kind !=
              DRASTIC_CUSTOM_TEXTURE_TARGET ||
          (shader->textures[output_index].output_scale &&
           shader->textures[output_index].output_scale != output_scale)) {
        set_error(error, error_size, "%s pass %d has an invalid output",
                  relative.c_str(), pass_index + 1);
        return false;
      }
      pass.output_texture = output_index;
      pass.output_scale = output_scale;
      shader->textures[output_index].output_scale = output_scale;
      produced[output_index] = true;
    }

    if (flags & DRASTIC_CUSTOM_SHADER_LOAD_SOURCES) {
      std::string dsd_text;
      if (!read_text(full_path(shader_relative), dsd_text,
                     error, error_size)) return false;
      const std::vector<Section> stages = parse_sections(dsd_text);
      std::string vertex_body;
      std::string fragment_body;
      for (const Section &stage : stages) {
        if (stage.tag == "vertex") vertex_body = stage.body;
        else if (stage.tag == "fragment") fragment_body = stage.body;
      }
      if (vertex_body.empty() || fragment_body.empty()) {
        set_error(error, error_size, "%s is missing a shader stage",
                  shader_relative.c_str());
        return false;
      }
      std::string vertex = strip_versions(vertex_prefix + vertex_body);
      std::string fragment = strip_versions(fragment_prefix + fragment_body);
      if (!duplicate_string(vertex, &pass.vertex_source,
                            error, error_size) ||
          !duplicate_string(fragment, &pass.fragment_source,
                            error, error_size)) return false;
    }
    if (flags & DRASTIC_CUSTOM_SHADER_LOAD_SPIRV) {
      const std::string pack = full_path(relative) + ".nxvk/pass" +
                               std::to_string(pass_index);
      if (!load_spirv_file(pack + ".vert.spv", &pass.vertex_spirv,
                           &pass.vertex_spirv_size, error, error_size) ||
          !load_spirv_file(pack + ".frag.spv", &pass.fragment_spirv,
                           &pass.fragment_spirv_size, error, error_size))
        return false;
    }
    if (flags & DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D) {
      const std::string pack = full_path(relative) + ".nxdk/pass" +
                               std::to_string(pass_index);
      if (!load_deko_file(pack + ".vert.dksh", 0, &pass.vertex_deko3d,
                          &pass.vertex_deko3d_size, error, error_size) ||
          !load_deko_file(pack + ".frag.dksh", 4, &pass.fragment_deko3d,
                          &pass.fragment_deko3d_size, error, error_size))
        return false;
    }
  }
  if (flags & DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D) {
    std::string marker;
    if (!read_text(full_path(relative) + ".nxdk/pack.info", marker,
                    error, error_size)) return false;
    const std::string count = "passes=" + std::to_string(shader->pass_count);
    if (marker.find("format=1\n") == std::string::npos ||
        marker.find(count + "\n") == std::string::npos) {
      set_error(error, error_size, "%s has an incomplete native shader pack",
                relative.c_str());
      return false;
    }
  }
  return true;
}

static bool entry_less(const DrasticCustomShaderEntry &left,
                       const DrasticCustomShaderEntry &right) {
  const unsigned char *a = (const unsigned char *)left.name;
  const unsigned char *b = (const unsigned char *)right.name;
  while (*a && *b) {
    const int ca = std::tolower(*a++);
    const int cb = std::tolower(*b++);
    if (ca != cb) return ca < cb;
  }
  return *a < *b;
}

static void scan_directory(const std::string &relative, int depth,
                           std::vector<DrasticCustomShaderEntry> &entries) {
  if (depth > 8 || entries.size() >= 512) return;
  const std::string directory = relative.empty()
      ? std::string(SHADERS_DIR) : full_path(relative);
  DIR *handle = opendir(directory.c_str());
  if (!handle) return;
  while (dirent *item = readdir(handle)) {
    if (!std::strcmp(item->d_name, ".") ||
        !std::strcmp(item->d_name, "..")) continue;
    std::string child;
    if (!normalize_relative(relative, item->d_name, child)) continue;
    const std::string path = full_path(child);
    struct stat status{};
    if (stat(path.c_str(), &status) != 0) continue;
    if (S_ISDIR(status.st_mode)) {
      if (std::strstr(item->d_name, ".nxvk") ||
          std::strstr(item->d_name, ".nxdk")) continue;
      scan_directory(child, depth + 1, entries);
      continue;
    }
    if (!S_ISREG(status.st_mode) || !has_suffix_ignore_case(child, ".dfx"))
      continue;
    DrasticCustomShader shader{};
    char ignored[256];
    if (!parse_manifest(child, 0, &shader, ignored, sizeof(ignored))) {
      drastic_custom_shader_destroy(&shader);
      continue;
    }
    DrasticCustomShaderEntry entry{};
    std::snprintf(entry.name, sizeof(entry.name), "%s", shader.name);
    std::snprintf(entry.relative_path, sizeof(entry.relative_path), "%s",
                  child.c_str());
    entry.vulkan_ready = drastic_custom_shader_vulkan_ready(
        child.c_str(), nullptr, 0);
    entry.deko3d_ready = drastic_custom_shader_deko3d_ready(
        child.c_str(), nullptr, 0);
    entries.push_back(entry);
    drastic_custom_shader_destroy(&shader);
    if (entries.size() >= 512) break;
  }
  closedir(handle);
}

}  // namespace

extern "C" bool drastic_custom_shader_load(
    const char *relative_path, unsigned flags, DrasticCustomShader *shader,
    char *error, size_t error_size) {
  if (error && error_size) error[0] = '\0';
  if (!shader) {
    set_error(error, error_size, "Invalid shader destination");
    return false;
  }
  std::memset(shader, 0, sizeof(*shader));
  std::string relative;
  if (!relative_path || !normalize_relative({}, relative_path, relative) ||
      !has_suffix_ignore_case(relative, ".dfx")) {
    set_error(error, error_size, "Select a .dfx file inside %s", SHADERS_DIR);
    return false;
  }
  if (!parse_manifest(relative, flags, shader, error, error_size)) {
    drastic_custom_shader_destroy(shader);
    return false;
  }
  return true;
}

extern "C" void drastic_custom_shader_destroy(DrasticCustomShader *shader) {
  if (!shader) return;
  for (int index = 0; index < DRASTIC_CUSTOM_SHADER_MAX_TEXTURES; index++)
    std::free(shader->textures[index].pixels);
  for (int index = 0; index < DRASTIC_CUSTOM_SHADER_MAX_PASSES; index++) {
    std::free(shader->passes[index].vertex_source);
    std::free(shader->passes[index].fragment_source);
    std::free(shader->passes[index].vertex_spirv);
    std::free(shader->passes[index].fragment_spirv);
    std::free(shader->passes[index].vertex_deko3d);
    std::free(shader->passes[index].fragment_deko3d);
  }
  std::memset(shader, 0, sizeof(*shader));
}

extern "C" size_t drastic_custom_shader_scan(
    DrasticCustomShaderEntry *entries, size_t capacity) {
  std::vector<DrasticCustomShaderEntry> found;
  scan_directory({}, 0, found);
  std::sort(found.begin(), found.end(), entry_less);
  const size_t count = std::min(capacity, found.size());
  if (entries && count)
    std::memcpy(entries, found.data(), count * sizeof(*entries));
  return count;
}

extern "C" bool drastic_custom_shader_vulkan_ready(
    const char *relative_path, char *error, size_t error_size) {
  DrasticCustomShader shader{};
  bool ready = drastic_custom_shader_load(relative_path, 0, &shader,
                                           error, error_size);
  for (int pass = 0; ready && pass < shader.pass_count; pass++) {
    const std::string base = full_path(shader.relative_path) + ".nxvk/pass" +
                             std::to_string(pass);
    ready = spirv_file_ready(base + ".vert.spv", error, error_size) &&
            spirv_file_ready(base + ".frag.spv", error, error_size);
  }
  drastic_custom_shader_destroy(&shader);
  return ready;
}

extern "C" bool drastic_custom_shader_deko3d_ready(
    const char *relative_path, char *error, size_t error_size) {
  DrasticCustomShader shader{};
  const bool ready = drastic_custom_shader_load(relative_path,
      DRASTIC_CUSTOM_SHADER_LOAD_DEKO3D, &shader, error, error_size);
  drastic_custom_shader_destroy(&shader);
  return ready;
}
