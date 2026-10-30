// Build-time shader compiler for Xenia's built-in shaders.
//
// Usage: xenia-shader-cc [--msl | --slang-dxil] [--depfile <path>]
//                       <input> <output.h>
//
// Default: GLSL/XeSL -> SPIR-V, linked in-process via glslang and
// SPIRV-Tools, emitted as `const uint32_t <id>[]`. Stage parsed from
// filename stem (foo.cs.glsl -> compute); XeSL sources compiled with
// SHADING_LANGUAGE_GLSL_XE=1 and includes resolved relative to the
// input's directory.
//
// --msl (Apple only): .xesl -> .metallib via `xcrun metal -x metal -D
// SHADING_LANGUAGE_MSL_XE=1`, emitted as `const uint8_t <id>_metallib[]`.
//
// --slang-dxil: .slang -> DXIL via a hardcoded local slangc.exe, emitted as
// `const uint8_t <id>[]`. Phase 1 of the Slang migration; the path will
// move to FetchContent / env override before this lands in CI.
// --slang-spirv: .slang -> SPIR-V via the same slangc, emitted as
// `const uint32_t <id>[]`. Vulkan binding mapping uses -fvk-use-dx-layout
// + b/t/u/s shift 0 so HLSL register slots translate directly.
// --slang-msl (Apple only): .slang -> .metal source via slangc, then
// xcrun metal/metallib to a metallib, emitted as `const uint8_t
// <id>_metallib[]` (matches the existing --msl format).
//
// --ios <min-version> selects the iPhoneOS SDK for Metal output.
//
// --depfile writes a Make-style dependency file listing every source the
// compile transitively read, so CMake/Ninja can trigger rebuilds when a
// cross-directory include (e.g. ui/shaders/xesl.xesli) changes.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DirStackFileIncluder.h"
#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

struct StageInfo {
  const char* key;
  EShLanguage language;
};

constexpr StageInfo kStages[] = {
    {"vs", EShLangVertex},         {"hs", EShLangTessControl},
    {"ds", EShLangTessEvaluation}, {"gs", EShLangGeometry},
    {"ps", EShLangFragment},       {"cs", EShLangCompute},
};

constexpr const char* kXeslWrapperPrefix =
    "#version 460\n"
    "#extension GL_EXT_control_flow_attributes : require\n"
    "#extension GL_EXT_samplerless_texture_functions : require\n"
    "#extension GL_GOOGLE_include_directive : require\n";

bool ParseStage(const std::string& filename, EShLanguage* out,
                std::string* stage_key_out) {
  // Strip extension, then take last 2 chars after the final dot.
  auto dot = filename.rfind('.');
  if (dot == std::string::npos || dot < 3 || filename[dot - 3] != '.') {
    return false;
  }
  std::string key = filename.substr(dot - 2, 2);
  for (const auto& stage : kStages) {
    if (key == stage.key) {
      *out = stage.language;
      *stage_key_out = key;
      return true;
    }
  }
  return false;
}

std::string IdentifierFromFilename(const std::string& filename) {
  // foo.bar.cs.glsl -> foo_bar_cs
  auto last_dot = filename.rfind('.');
  std::string stem = filename.substr(0, last_dot);
  std::replace(stem.begin(), stem.end(), '.', '_');
  return stem;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "failed to open %s\n", path.string().c_str());
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  *out = buf.str();
  return true;
}

// Escape spaces in Make-style depfile paths (Ninja's parser requires this).
std::string EscapeDepPath(const std::string& p) {
  std::string out;
  out.reserve(p.size());
  for (char c : p) {
    if (c == ' ' || c == '\t') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// Writes a Make-style depfile: "<target>: <dep1> <dep2> ..." with each dep on
// its own line (backslash-continued) so Ninja's DEPFILE parser accepts it.
bool WriteDepfile(const std::filesystem::path& depfile_path,
                  const std::filesystem::path& target,
                  const std::vector<std::string>& deps) {
  std::error_code ec;
  std::filesystem::create_directories(depfile_path.parent_path(), ec);
  std::ofstream out(depfile_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open depfile %s\n",
                 depfile_path.string().c_str());
    return false;
  }
  out << EscapeDepPath(target.string()) << ":";
  for (const auto& dep : deps) {
    out << " \\\n  " << EscapeDepPath(dep);
  }
  out << "\n";
  return bool(out);
}

// slangc's -depfile names its own temp output as the target. Ninja treats a
// target that isn't the edge's output as perpetually dirty, so rewrite it.
bool RetargetDepfile(const std::filesystem::path& depfile_path,
                     const std::filesystem::path& target) {
  std::string content;
  if (!ReadFile(depfile_path, &content)) {
    return false;
  }
  // Everything past the separating colon is the dependency list.
  size_t colon = std::string::npos;
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\\') {
      ++i;
    } else if (content[i] == ':' &&
               !(i == 1 &&
                 std::isalpha(static_cast<unsigned char>(content[0])))) {
      colon = i;
      break;
    }
  }
  if (colon == std::string::npos) {
    std::fprintf(stderr, "malformed depfile %s\n",
                 depfile_path.string().c_str());
    return false;
  }
  // Split on unescaped whitespace. WriteDepfile re-escapes on the way out.
  std::vector<std::string> deps;
  std::string dep;
  for (size_t i = colon + 1; i < content.size(); ++i) {
    char c = content[i];
    if (c == '\\' && i + 1 < content.size()) {
      char next = content[i + 1];
      if (next == '\n' || next == '\r') {
        continue;
      }
      if (next == ' ' || next == '\t') {
        dep.push_back(' ');
        ++i;
        continue;
      }
      dep.push_back(c);  // literal separator in a Windows path
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!dep.empty()) {
        deps.push_back(dep);
        dep.clear();
      }
    } else {
      dep.push_back(c);
    }
  }
  if (!dep.empty()) {
    deps.push_back(dep);
  }
  return WriteDepfile(depfile_path, target, deps);
}

std::string DisassembleSpirv(const std::vector<uint32_t>& spirv) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  const uint32_t options = SPV_BINARY_TO_TEXT_OPTION_INDENT |
                           SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;
  if (!tools.Disassemble(spirv, &text, options)) {
    text.clear();
  }
  return text;
}

bool WriteSpirvHeader(const std::filesystem::path& path,
                      const std::string& identifier,
                      const std::vector<uint32_t>& spirv) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n#if 0\n";
  std::string disasm = DisassembleSpirv(spirv);
  if (!disasm.empty()) {
    out << disasm;
    if (disasm.back() != '\n') {
      out << '\n';
    }
  }
  out << "#endif\n\n";
  out << "const uint32_t " << identifier << "[] = {";
  for (size_t i = 0; i < spirv.size(); ++i) {
    out << (i % 6 == 0 ? "\n    " : " ");
    char word[16];
    std::snprintf(word, sizeof(word), "0x%08X,", spirv[i]);
    out << word;
  }
  out << "\n};\n";
  return bool(out);
}

#if defined(_WIN32)
// Quote an argument for the MSVCRT command-line parser, which is what slangc
// (and most MSVC-built tools) use to re-split GetCommandLineW() back into
// argv. _spawnvp joins argv with literal spaces and no quoting, so any arg
// containing whitespace -- e.g. "C:\Program Files (x86)\..." -- must be
// wrapped here or it gets re-split into multiple tokens on the receiving end.
std::string QuoteForSpawn(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string out;
  out.push_back('"');
  for (size_t i = 0; i < arg.size(); ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    } else if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}
#endif

int RunCommand(const std::vector<std::string>& args) {
  if (args.empty()) return -1;
#if defined(_WIN32)
  std::vector<std::string> quoted;
  quoted.reserve(args.size());
  for (const auto& a : args) quoted.push_back(QuoteForSpawn(a));
  std::vector<char*> argv;
  argv.reserve(quoted.size() + 1);
  for (auto& a : quoted) argv.push_back(a.data());
  argv.push_back(nullptr);
  intptr_t rc = _spawnvp(_P_WAIT, args[0].c_str(), argv.data());
  if (rc < 0) {
    std::fprintf(stderr, "_spawnvp(%s) failed: %s\n", args[0].c_str(),
                 std::strerror(errno));
    return -1;
  }
  return static_cast<int>(rc);
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    std::fprintf(stderr, "posix_spawnp(%s) failed: %s\n", argv[0],
                 std::strerror(rc));
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::fprintf(stderr, "waitpid failed for %s\n", argv[0]);
    return -1;
  }
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
#endif
}

bool ReadBinaryFile(const std::filesystem::path& path,
                    std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  auto size = in.tellg();
  if (size < 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out->data()), size);
  return bool(in);
}

// Emits `const uint8_t <symbol>[] = { 0x.., ... };` to path. Used for any
// opaque binary blob (DXIL container, metallib, etc.).
bool WriteByteArrayHeader(const std::filesystem::path& path,
                          const std::string& symbol,
                          const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n";
  out << "const uint8_t " << symbol << "[] = {";
  for (size_t i = 0; i < bytes.size(); ++i) {
    out << (i % 16 == 0 ? "\n    " : " ");
    char byte[8];
    std::snprintf(byte, sizeof(byte), "0x%02X,", bytes[i]);
    out << byte;
  }
  out << "\n};\n";
  return bool(out);
}

#ifdef XE_SHADER_CC_METAL

bool WriteMetallibHeader(const std::filesystem::path& path,
                         const std::string& identifier,
                         const std::vector<uint8_t>& bytes) {
  return WriteByteArrayHeader(path, identifier + "_metallib", bytes);
}

#endif  // XE_SHADER_CC_METAL

// The path to slangc, taken from the SLANGC_PATH environment variable (set by
// CI and local build configs). There is no default. A missing SLANGC_PATH is
// a hard error so .slang compilation fails fast.
const char* GetSlangcPath() {
  const char* env_path = std::getenv("SLANGC_PATH");
  if (!env_path || !env_path[0]) {
    std::fprintf(stderr,
                 "SLANGC_PATH is not set - point it at the slangc executable "
                 "to compile .slang shaders.\n");
    std::exit(1);
  }
  return env_path;
}

// Maps our 2-letter filename stage suffix to slangc's stage names.
const char* SlangStageName(const std::string& stage_key) {
  if (stage_key == "vs") {
    return "vertex";
  }
  if (stage_key == "ps") {
    return "pixel";
  }
  if (stage_key == "cs") {
    return "compute";
  }
  if (stage_key == "hs") {
    return "hull";
  }
  if (stage_key == "ds") {
    return "domain";
  }
  if (stage_key == "gs") {
    return "geometry";
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef XE_SHADER_CC_METAL
  std::string metal_sdk = "macosx";
  std::string metal_std = "-std=macos-metal2.3";
  std::string metal_min_os = "-mmacosx-version-min=" XE_METAL_MIN_OS;
#endif
  bool msl_mode = false;
  bool slang_dxil_mode = false;
  bool slang_spirv_mode = false;
  bool slang_msl_mode = false;
  std::string depfile_path;
  int arg_idx = 1;
  while (arg_idx < argc && argv[arg_idx][0] == '-') {
    if (std::strcmp(argv[arg_idx], "--msl") == 0) {
#ifndef XE_SHADER_CC_METAL
      std::fprintf(stderr,
                   "--msl not available (built without XE_SHADER_CC_METAL)\n");
      return 1;
#endif
      msl_mode = true;
      ++arg_idx;
    } else if (std::strcmp(argv[arg_idx], "--slang-dxil") == 0) {
      slang_dxil_mode = true;
      ++arg_idx;
    } else if (std::strcmp(argv[arg_idx], "--slang-spirv") == 0) {
      slang_spirv_mode = true;
      ++arg_idx;
    } else if (std::strcmp(argv[arg_idx], "--slang-msl") == 0) {
#ifndef XE_SHADER_CC_METAL
      std::fprintf(stderr,
                   "--slang-msl not available (built without "
                   "XE_SHADER_CC_METAL)\n");
      return 1;
#endif
      slang_msl_mode = true;
      ++arg_idx;
#ifdef XE_SHADER_CC_METAL
    } else if (std::strcmp(argv[arg_idx], "--ios") == 0) {
      if (++arg_idx >= argc ||
          std::strspn(argv[arg_idx], "0123456789.") !=
              std::strlen(argv[arg_idx]) ||
          !argv[arg_idx][0]) {
        std::fprintf(stderr, "--ios requires a deployment version\n");
        return 1;
      }
      metal_sdk = "iphoneos";
      metal_std = "-std=ios-metal2.3";
      metal_min_os = "-mios-version-min=" + std::string(argv[arg_idx++]);
#endif
    } else if (std::strcmp(argv[arg_idx], "--depfile") == 0) {
      if (arg_idx + 1 >= argc) {
        std::fprintf(stderr, "--depfile requires a path\n");
        return 1;
      }
      depfile_path = argv[arg_idx + 1];
      arg_idx += 2;
    } else {
      std::fprintf(stderr, "unknown flag: %s\n", argv[arg_idx]);
      return 1;
    }
  }
  if (argc - arg_idx != 2) {
    std::fprintf(stderr,
                 "Usage: %s [--msl | --slang-dxil | --slang-spirv | "
                 "--slang-msl] [--ios <min-version>] [--depfile <path>] "
                 "<input> <output>\n",
                 argv[0]);
    return 1;
  }
  if (int(msl_mode) + int(slang_dxil_mode) + int(slang_spirv_mode) +
          int(slang_msl_mode) >
      1) {
    std::fprintf(stderr, "compile mode flags are mutually exclusive\n");
    return 1;
  }

  std::filesystem::path input_path = argv[arg_idx];
  std::filesystem::path output_path = argv[arg_idx + 1];
  std::string input_filename = input_path.filename().string();
  std::string identifier = IdentifierFromFilename(input_filename);

  if (msl_mode) {
#ifdef XE_SHADER_CC_METAL
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto tag = identifier + "_" + std::to_string(::getpid());
    auto air_path = tmp_dir / (tag + ".air");
    auto lib_path = tmp_dir / (tag + ".metallib");

    auto cleanup = [&] {
      std::error_code ec;
      std::filesystem::remove(air_path, ec);
      std::filesystem::remove(lib_path, ec);
    };

    std::vector<std::string> metal_cmd = {
        "xcrun", "-sdk",    metal_sdk,    "metal", "-x",
        "metal", metal_std, metal_min_os, "-D",    "SHADING_LANGUAGE_MSL_XE=1",
        "-w",
    };
    std::string input_dir = input_path.parent_path().string();
    if (!input_dir.empty()) {
      metal_cmd.push_back("-I");
      metal_cmd.push_back(input_dir);
    }
    if (!depfile_path.empty()) {
      // xcrun metal is clang-based and supports standard depfile flags.
      // -MT retargets the depfile at our final .h (not the intermediate .air)
      // so CMake/Ninja link the dependency graph to the right output.
      metal_cmd.push_back("-MD");
      metal_cmd.push_back("-MT");
      metal_cmd.push_back(output_path.string());
      metal_cmd.push_back("-MF");
      metal_cmd.push_back(depfile_path);
    }
    metal_cmd.push_back("-c");
    metal_cmd.push_back(input_path.string());
    metal_cmd.push_back("-o");
    metal_cmd.push_back(air_path.string());
    if (RunCommand(metal_cmd) != 0) {
      std::fprintf(stderr, "metal failed for %s\n",
                   input_path.string().c_str());
      cleanup();
      return 1;
    }
    if (RunCommand({"xcrun", "-sdk", metal_sdk, "metallib", air_path.string(),
                    "-o", lib_path.string()}) != 0) {
      std::fprintf(stderr, "metallib failed for %s\n",
                   input_path.string().c_str());
      cleanup();
      return 1;
    }

    std::vector<uint8_t> bytes;
    if (!ReadBinaryFile(lib_path, &bytes)) {
      std::fprintf(stderr, "failed to read %s\n", lib_path.string().c_str());
      cleanup();
      return 1;
    }
    cleanup();

    if (!WriteMetallibHeader(output_path, identifier, bytes)) {
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr,
                 "--msl not available (built without XE_SHADER_CC_METAL)\n");
    return 1;
#endif
  }

  EShLanguage stage;
  std::string stage_key;
  if (!ParseStage(input_filename, &stage, &stage_key)) {
    std::fprintf(stderr,
                 "cannot determine shader stage from filename: %s\n",
                 input_filename.c_str());
    return 1;
  }

  if (slang_dxil_mode || slang_spirv_mode || slang_msl_mode) {
    const char* slang_stage = SlangStageName(stage_key);
    if (!slang_stage) {
      std::fprintf(stderr, "unsupported stage %s for slang modes\n",
                   stage_key.c_str());
      return 1;
    }

    auto tmp_dir = std::filesystem::temp_directory_path();
    auto tag = identifier + "_" +
               std::to_string(
#ifdef _WIN32
                   static_cast<int>(_getpid())
#else
                   static_cast<int>(::getpid())
#endif
               );

    // Per-target output extension and slangc -target flag.
    const char* slang_target =
        slang_dxil_mode ? "dxil" : (slang_spirv_mode ? "spirv" : "metal");
    const char* tmp_ext =
        slang_dxil_mode ? ".dxil" : (slang_spirv_mode ? ".spv" : ".metal");
    auto slang_out = tmp_dir / (tag + tmp_ext);
    auto cleanup_slang = [&] {
      std::error_code ec;
      std::filesystem::remove(slang_out, ec);
    };

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    std::vector<std::string> cmd = {
        GetSlangcPath(),
        input_path.string(),
        "-target",
        slang_target,
        "-stage",
        slang_stage,
        "-entry",
        "main",
        "-I",
        input_path.parent_path().string(),
        "-o",
        slang_out.string(),
    };
    if (slang_dxil_mode) {
      cmd.push_back("-profile");
      cmd.push_back("sm_6_0");
    }
    // Note: we don't pass -fvk-*-shift here — those flags would override the
    // explicit [[vk::binding(...)]] attributes that the xesl macros emit
    // when XE_SLANG is set, defeating the whole point. Shaders consumed by
    // the slang-spirv rule must carry explicit Vulkan bindings, either via
    // the xesli macros or hand-written annotations.
    if (slang_spirv_mode) {
      // 39001 ("explicit binding overlap") fires when an explicit
      // [[vk::binding(0, 0)]] coexists with a [[vk::push_constant]] cbuffer
      // — Slang's layout pass tracks both as if they shared the descriptor
      // table even though the push-constant redirects to PushConstant
      // storage in the actual SPIR-V. Cosmetic; output is correct.
      cmd.insert(cmd.end(), {"-Wno-39001"});
      // Vulkan clip space has Y pointing down, like GLSL and opposite D3D, so
      // flag the SPIR-V target for NDC_DIRECTION_Y_XE selection (the shaders
      // read the HLSL branch, which otherwise picks the D3D Y direction).
      cmd.insert(cmd.end(), {"-DXE_SLANG_SPIRV=1"});
    }
    if (slang_msl_mode) {
      // 40100 ("entry point 'main' renamed to 'main_0'") is expected. Slang
      // renames main for MSL and we rewrite it to entry_xe below, so silence
      // the per-shader noise.
      cmd.insert(cmd.end(), {"-Wno-40100"});
      // Lets the xesli binding macros pin Metal buffer indices for slang. Slang
      // maps the HLSL register index to the Metal buffer slot, so the byte
      // buffers carry their slot as the register index in this mode.
      cmd.insert(cmd.end(), {"-DXE_SLANG_MSL=1"});
      // 39029 ("D3D register without Vulkan binding or shift") fires because
      // the byte buffers in this mode carry only a register, no vk::binding.
      // That register drives the Metal slot. Vulkan bindings are irrelevant
      // for the Metal target, so silence it.
      cmd.insert(cmd.end(), {"-Wno-39029"});
    }
    // 30056 ("non-short-circuiting `?:` is deprecated") fires inside the
    // third-party FidelityFX FSR header (ffx_a.h) which we can't edit. Our
    // own xesli macros already use `select()` for the Slang/HLSL branch.
    cmd.insert(cmd.end(), {"-Wno-30056"});
    // Slang reads the HLSL branch of xesl.xesli for all three targets and
    // does its own per-target lowering. Define the polyglot flags explicitly
    // (slangc warns on undefined identifiers in #if). XE_SLANG gates the
    // [[vk::binding]]/[[vk::push_constant]] surfacing in the HLSL macro
    // branch — fxc/dxc would reject those attributes, but slangc consumes
    // them (and ignores them for non-SPIR-V targets, harmlessly).
    cmd.insert(cmd.end(),
               {"-DSHADING_LANGUAGE_HLSL_XE=1", "-DSHADING_LANGUAGE_GLSL_XE=0",
                "-DSHADING_LANGUAGE_MSL_XE=0",
                "-DCOMBINED_TEXTURE_SAMPLER_XE=0", "-DXE_SLANG=1"});
    if (!depfile_path.empty()) {
      cmd.push_back("-depfile");
      cmd.push_back(depfile_path);
    }
    if (RunCommand(cmd) != 0) {
      std::fprintf(stderr, "slangc failed for %s\n",
                   input_path.string().c_str());
      cleanup_slang();
      return 1;
    }
    if (!depfile_path.empty() && !RetargetDepfile(depfile_path, output_path)) {
      cleanup_slang();
      return 1;
    }

    if (slang_dxil_mode) {
      std::vector<uint8_t> bytes;
      if (!ReadBinaryFile(slang_out, &bytes)) {
        std::fprintf(stderr, "failed to read %s\n", slang_out.string().c_str());
        cleanup_slang();
        return 1;
      }
      cleanup_slang();
      if (!WriteByteArrayHeader(output_path, identifier, bytes)) {
        return 1;
      }
      return 0;
    }

    if (slang_spirv_mode) {
      std::vector<uint8_t> bytes;
      if (!ReadBinaryFile(slang_out, &bytes)) {
        std::fprintf(stderr, "failed to read %s\n", slang_out.string().c_str());
        cleanup_slang();
        return 1;
      }
      cleanup_slang();
      if (bytes.size() % 4 != 0) {
        std::fprintf(stderr, "SPIR-V output size %zu not word-aligned\n",
                     bytes.size());
        return 1;
      }
      std::vector<uint32_t> words(bytes.size() / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      if (!WriteSpirvHeader(output_path, identifier, words)) {
        return 1;
      }
      return 0;
    }

    // slang_msl_mode: slangc emits .metal source, then xcrun metal/metallib
    // produce the metallib bytes — same final stage as the existing --msl.
#ifdef XE_SHADER_CC_METAL
    // Slang renames the source `main` entry point to `main_0` in MSL output
    // (it's reserved territory in C++/MSL). The runtime metal loaders look
    // up the function by the legacy xesl name `entry_xe`, so rewrite the
    // function name in the .metal source before xcrun compiles it. Keeps
    // the slang-msl path drop-in compatible with the existing call sites.
    {
      std::string metal_src;
      if (!ReadFile(slang_out, &metal_src)) {
        cleanup_slang();
        return 1;
      }
      // Match the entry by name only, not return type: compute entries are
      // `void main_0(`, but vertex/fragment entries return a struct
      // (`[[vertex]] main_Result_0 main_0(`), so a `void`-anchored match would
      // miss them and the presenter's newFunctionWithName("entry_xe") fails.
      const std::string from = " main_0(";
      const std::string to = " entry_xe(";
      for (size_t pos = 0;
           (pos = metal_src.find(from, pos)) != std::string::npos;
           pos += to.size()) {
        metal_src.replace(pos, from.size(), to);
      }

      // Slang lowers Texture2DMS.Load to texture2d_ms::read with a signed int2
      // coordinate, which the Metal compiler rejects (read wants uint2). Wrap
      // the read coordinate of multisampled textures in uint2(). Scoped by
      // texture2d_ms member name so texture2d/texture3d reads are untouched.
      std::set<std::string> ms_textures;
      const std::string ms_decl = "texture2d_ms<";
      for (size_t pos = 0;
           (pos = metal_src.find(ms_decl, pos)) != std::string::npos;) {
        size_t gt = metal_src.find('>', pos);
        if (gt == std::string::npos) {
          break;
        }
        size_t name_begin = gt + 1;
        while (
            name_begin < metal_src.size() &&
            std::isspace(static_cast<unsigned char>(metal_src[name_begin]))) {
          ++name_begin;
        }
        size_t name_end = name_begin;
        while (name_end < metal_src.size() &&
               (std::isalnum(static_cast<unsigned char>(metal_src[name_end])) ||
                metal_src[name_end] == '_')) {
          ++name_end;
        }
        if (name_end > name_begin) {
          ms_textures.insert(
              metal_src.substr(name_begin, name_end - name_begin));
        }
        pos = name_end;
      }
      for (const std::string& tex : ms_textures) {
        const std::string read_from = tex + ").read((";
        const std::string read_to = tex + ").read(uint2(";
        for (size_t pos = 0;
             (pos = metal_src.find(read_from, pos)) != std::string::npos;
             pos += read_to.size()) {
          metal_src.replace(pos, read_from.size(), read_to);
        }
      }

      std::ofstream out(slang_out, std::ios::binary | std::ios::trunc);
      if (!out) {
        std::fprintf(stderr, "failed to rewrite %s\n",
                     slang_out.string().c_str());
        cleanup_slang();
        return 1;
      }
      out.write(metal_src.data(), metal_src.size());
    }
    auto air_path = tmp_dir / (tag + ".air");
    auto lib_path = tmp_dir / (tag + ".metallib");
    auto cleanup_metal = [&] {
      std::error_code mec;
      std::filesystem::remove(air_path, mec);
      std::filesystem::remove(lib_path, mec);
    };
    // Pin the deployment target so the AIR (and the metallib linked from it)
    // carries XE_METAL_MIN_OS. Without this, the macOS 26 SDK stamps the
    // SDK-default target into the library (deployment target 0x00020008),
    // which the loader rejects on older OSes. Mirrors the --msl path above.
    if (RunCommand({"xcrun", "-sdk", metal_sdk, "metal", "-x", "metal",
                    metal_std, metal_min_os, "-w", "-c",
                    slang_out.string(), "-o", air_path.string()}) != 0) {
      std::fprintf(stderr, "metal failed for %s\n",
                   input_path.string().c_str());
      cleanup_slang();
      cleanup_metal();
      return 1;
    }
    if (RunCommand({"xcrun", "-sdk", metal_sdk, "metallib", air_path.string(),
                    "-o", lib_path.string()}) != 0) {
      std::fprintf(stderr, "metallib failed for %s\n",
                   input_path.string().c_str());
      cleanup_slang();
      cleanup_metal();
      return 1;
    }
    std::vector<uint8_t> bytes;
    if (!ReadBinaryFile(lib_path, &bytes)) {
      std::fprintf(stderr, "failed to read %s\n", lib_path.string().c_str());
      cleanup_slang();
      cleanup_metal();
      return 1;
    }
    cleanup_slang();
    cleanup_metal();
    if (!WriteMetallibHeader(output_path, identifier, bytes)) {
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr,
                 "--slang-msl not available (built without "
                 "XE_SHADER_CC_METAL)\n");
    return 1;
#endif
  }

  std::string source;
  if (!ReadFile(input_path, &source)) {
    return 1;
  }

  // .xesl files rely on a wrapper prepending #version / extensions and then
  // #include'ing the file itself. glslang needs the include directive to come
  // from an actual source string, not from disk, so we synthesize the wrapper
  // and let the includer fetch the real file.
  const bool is_xesl = input_filename.size() > 5 &&
                       input_filename.compare(input_filename.size() - 5, 5,
                                              ".xesl") == 0;
  std::string wrapper;
  std::string wrapper_name;
  if (is_xesl) {
    wrapper = kXeslWrapperPrefix;
    wrapper += "#include \"";
    wrapper += input_filename;
    wrapper += "\"\n";
  }

  glslang::InitializeProcess();

  glslang::TShader shader(stage);
  const char* source_strings[1];
  const char* source_names[1];
  int source_lengths[1];
  if (is_xesl) {
    source_strings[0] = wrapper.c_str();
    source_lengths[0] = static_cast<int>(wrapper.size());
    wrapper_name = "xesl_wrapper";
    source_names[0] = wrapper_name.c_str();
  } else {
    source_strings[0] = source.c_str();
    source_lengths[0] = static_cast<int>(source.size());
    source_names[0] = input_filename.c_str();
  }
  shader.setStringsWithLengthsAndNames(source_strings, source_lengths,
                                       source_names, 1);
  shader.setPreamble("#define SHADING_LANGUAGE_GLSL_XE 1\n");
  // Match the old `glslangValidator -V` default: Vulkan 1.0 / SPV 1.0, which
  // stays compatible with the Vulkan 1.0 devices the runtime still supports.
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                     100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  DirStackFileIncluder includer;
  std::string input_dir = input_path.parent_path().string();
  if (!input_dir.empty()) {
    includer.pushExternalLocalDirectory(input_dir);
  }

  const int default_version = 460;
  const EShMessages messages = static_cast<EShMessages>(
      EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);

  if (!shader.parse(GetDefaultResources(), default_version, false, messages,
                    includer)) {
    std::fprintf(stderr, "glslang parse failed for %s:\n%s%s",
                 input_path.string().c_str(), shader.getInfoLog(),
                 shader.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    std::fprintf(stderr, "glslang link failed for %s:\n%s%s",
                 input_path.string().c_str(), program.getInfoLog(),
                 program.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  // Emit raw SPIR-V from glslang; optimization is done explicitly below to
  // match the previous `spirv-opt -O -O --canonicalize-ids` behavior.
  glslang::SpvOptions spv_options;
  spv_options.generateDebugInfo = false;
  spv_options.stripDebugInfo = true;
  spv_options.disableOptimizer = true;
  spv_options.validate = false;

  std::vector<uint32_t> spirv;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spv_options);

  // Emit the depfile from the includer's set of resolved paths. Add the
  // primary source too (it's a dep of the output .h but not something the
  // includer tracked since we synthesize the XeSL wrapper).
  if (!depfile_path.empty()) {
    std::vector<std::string> deps;
    deps.push_back(input_path.string());
    for (const auto& p : includer.getIncludedFiles()) {
      deps.push_back(p);
    }
    if (!WriteDepfile(depfile_path, output_path, deps)) {
      glslang::FinalizeProcess();
      return 1;
    }
  }

  glslang::FinalizeProcess();

  if (spirv.empty()) {
    std::fprintf(stderr, "GlslangToSpv produced empty output for %s\n",
                 input_path.string().c_str());
    return 1;
  }

  {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_2);
    optimizer.SetMessageConsumer(
        [](spv_message_level_t, const char*, const spv_position_t&,
           const char* message) { std::fprintf(stderr, "%s\n", message); });
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPass(spvtools::CreateCanonicalizeIdsPass());
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(spirv.data(), spirv.size(), &optimized)) {
      std::fprintf(stderr, "spirv-opt failed for %s\n",
                   input_path.string().c_str());
      return 1;
    }
    spirv = std::move(optimized);
  }

  if (!WriteSpirvHeader(output_path, identifier, spirv)) {
    return 1;
  }
  return 0;
}
