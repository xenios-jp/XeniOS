/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_JIT_CORPUS_H_
#define XENIA_CPU_JIT_CORPUS_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/cpu/function.h"

namespace xe {
namespace cpu {

class Module;
class Processor;

// Every guest function the JIT compiled in a run, with everything else the
// compile read that another process would not have: the instructions known to
// access MMIO, save/restore helper declarations, the read-only guest pages
// constant loads fold from, MMIO ranges and codegen cvars. xenia-cpu-replay
// compiles it again offline.
//
// Records are in the order functions finished compiling, which the replay
// keeps: a call to a callee that is already compiled is emitted differently,
// and the guest changes page protection as it runs. A capture normally ends
// with the process being killed, so reading stops quietly at a truncated last
// record. The file holds guest code and is not redistributable.
struct JitCorpus {
  static constexpr uint32_t kMagic = 0x52434A58;  // "XJCR"
  static constexpr uint32_t kVersion = 1;

  struct MmioRange {
    uint32_t address;
    uint32_t mask;
    uint32_t size;
  };
  // A guest page that became read-only, or writable again.
  struct Page {
    uint32_t address;
    uint32_t size;
    // Its contents if it became read-only.
    std::vector<uint8_t> data;
    // Number of functions recorded before it.
    size_t position;
  };
  struct FunctionRecord {
    uint32_t address;
    // Address of the last instruction.
    uint32_t end_address;
    // PackSymbolFlags of the function.
    uint32_t flags;
    // What the capture emitted, 0 for a declaration.
    uint32_t host_code_size;
    uint32_t stable_instructions;
    // The instructions as stored in guest memory, and a bit per instruction
    // recorded as accessing MMIO. Both empty for a declaration.
    std::vector<uint8_t> code;
    std::vector<uint8_t> mmio;
  };

  // Function metadata the backend reads from callees and the scanner from
  // restore helpers.
  static uint32_t PackSymbolFlags(const GuestFunction* function);
  static void UnpackSymbolFlags(uint32_t flags, GuestFunction* function,
                                GuestFunction::ExternHandler extern_handler);

  // Codegen cvars of any integer or bool type, by name. Setting one sets its
  // config value, so a value given on the command line still takes priority.
  static bool GetCvar(const std::string& name, uint64_t* value);
  static bool SetCvar(const std::string& name, uint64_t value);

  // Fails on a file that is not a corpus of this version.
  static bool Read(const std::filesystem::path& path, JitCorpus* out,
                   std::string* error);

  struct Cvar {
    std::string name;
    std::string category;
    uint64_t value;
  };
  std::vector<Cvar> cvars;
  std::vector<MmioRange> mmio_ranges;
  std::vector<Page> pages;
  std::vector<FunctionRecord> declarations;
  std::vector<FunctionRecord> functions;
  bool truncated = false;
};

class JitCorpusWriter {
 public:
  static std::unique_ptr<JitCorpusWriter> Create(
      Processor* processor, const std::filesystem::path& path);
  ~JitCorpusWriter();

  void RecordFunction(GuestFunction* function);

 private:
  JitCorpusWriter(Processor* processor, FILE* file)
      : processor_(processor), file_(file) {}
  void WriteEnvironment();
  void UpdateReadOnlyPages(bool scan);
  void WriteFunction(uint32_t tag, const GuestFunction* function,
                     bool with_code);
  void Write(const void* data, size_t size);

  Processor* processor_;
  FILE* file_;
  // Guest threads compile concurrently. The global lock, rather than a mutex
  // of our own, as the module and heap queries made under it take it too.
  xe::global_critical_region global_critical_region_;
  bool environment_written_ = false;
  std::unordered_set<Module*> declared_modules_;
  std::set<uint32_t> read_only_pages_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_JIT_CORPUS_H_
