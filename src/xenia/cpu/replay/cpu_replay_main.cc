/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

DEFINE_transient_path(replay_corpus, "",
                      "JIT corpus recorded with --jit_corpus_out.", "General");
DEFINE_path(replay_csv, "",
            "Write one row per function to this file, to compare two builds.",
            "General");
DEFINE_path(
    replay_baseline, "",
    "Compare against a --replay_csv file from another build and list the "
    "functions that changed the most.",
    "General");
DEFINE_string(replay_dump, "",
              "Print the guest code, optimized HIR and host code of the "
              "function at this guest address (hex).",
              "General");

namespace xe {
namespace cpu {
namespace {

// Replay only compiles, so an extern needs an address, not a body.
void ReplayExternHandler(ppc::PPCContext* ppc_context,
                         kernel::KernelState* kernel_state) {}

// Serves the compile what the capturing modules did: save/restore and extern
// metadata, and the instructions recorded as accessing MMIO.
class CorpusModule : public Module {
 public:
  CorpusModule(Processor* processor, const JitCorpus& corpus)
      : Module(processor) {
    for (const auto& record : corpus.declarations) {
      symbol_flags_[record.address] = record.flags;
    }
    for (const auto& record : corpus.functions) {
      symbol_flags_[record.address] = record.flags;
      for (size_t i = 0; i < record.mmio.size() * 8; ++i) {
        if (record.mmio[i / 8] & (1 << (i % 8))) {
          flags_[record.address + uint32_t(i) * 4].accessed_mmio = 1;
        }
      }
    }
  }

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  // Anything the replay mapped, which is all code the capture compiled.
  bool ContainsAddress(uint32_t address) override {
    auto heap = memory_->LookupHeap(address);
    uint32_t protect;
    return heap && heap->QueryProtect(address, &protect) &&
           (protect & kMemoryProtectRead);
  }

  InfoCacheFlags* GetInstructionAddressFlags(uint32_t address) override {
    auto it = flags_.find(address);
    return it != flags_.end() ? &it->second : nullptr;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    auto function = processor_->backend()->CreateGuestFunction(this, address);
    auto it = symbol_flags_.find(address);
    if (it != symbol_flags_.end()) {
      JitCorpus::UnpackSymbolFlags(it->second, function.get(),
                                   ReplayExternHandler);
    }
    return function;
  }

 private:
  std::string name_ = "corpus";
  std::unordered_map<uint32_t, uint32_t> symbol_flags_;
  std::unordered_map<uint32_t, InfoCacheFlags> flags_;
};

// Maps at least map_size bytes and copies size bytes of data there.
bool MapGuestMemory(Memory* memory, uint32_t address, const uint8_t* data,
                    uint32_t size, uint32_t map_size) {
  BaseHeap* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  const uint32_t page_size = heap->page_size();
  const uint32_t start = address & ~(page_size - 1);
  const uint32_t end = xe::align(address + map_size, page_size);
  if (!heap->AllocFixed(start, end - start, 0,
                        kMemoryAllocationReserve | kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite)) {
    return false;
  }
  std::memcpy(memory->TranslateVirtual(address), data, size);
  return true;
}

bool ProtectPage(Memory* memory, const JitCorpus::Page& page) {
  if (!page.data.empty() &&
      !MapGuestMemory(memory, page.address, page.data.data(), page.size,
                      page.size)) {
    return false;
  }
  return memory->LookupHeap(page.address)
      ->Protect(page.address, page.size,
                page.data.empty() ? kMemoryProtectRead | kMemoryProtectWrite
                                  : kMemoryProtectRead);
}

struct Row {
  uint32_t address;
  uint64_t guest_instructions;
  uint64_t host_bytes;
  uint64_t host_instructions;
  uint64_t stable_instructions;
  uint64_t captured_stable_instructions;
};

bool WriteRows(const std::filesystem::path& path,
               const std::vector<Row>& rows) {
  FILE* file = xe::filesystem::OpenFile(path, "w");
  if (!file) {
    fmt::print(stderr, "Unable to create {}\n", xe::path_to_utf8(path));
    return false;
  }
  fmt::print(file,
             "address,guest_instructions,host_bytes,host_instructions,"
             "stable_instructions,captured_stable_instructions\n");
  for (const Row& row : rows) {
    fmt::print(file, "{:08X},{},{},{},{},{}\n", row.address,
               row.guest_instructions, row.host_bytes, row.host_instructions,
               row.stable_instructions, row.captured_stable_instructions);
  }
  fclose(file);
  return true;
}

// Stable instruction counts against a CSV from another build or cvar set.
bool CompareRows(const std::filesystem::path& path,
                 const std::vector<Row>& rows) {
  FILE* file = xe::filesystem::OpenFile(path, "r");
  if (!file) {
    fmt::print(stderr, "Unable to open {}\n", xe::path_to_utf8(path));
    return false;
  }
  std::unordered_map<uint32_t, uint64_t> baseline;
  char line[256];
  unsigned int address;
  unsigned long long stable;
  while (fgets(line, sizeof(line), file)) {
    if (sscanf(line, "%X,%*u,%*u,%*u,%llu", &address, &stable) == 2) {
      baseline[address] = stable;
    }
  }
  fclose(file);

  struct Change {
    uint32_t address;
    int64_t before;
    int64_t after;
  };
  std::vector<Change> changes;
  uint64_t before = 0, after = 0, common = 0;
  for (const Row& row : rows) {
    auto it = baseline.find(row.address);
    if (it == baseline.end()) {
      continue;
    }
    ++common;
    before += it->second;
    after += row.stable_instructions;
    if (it->second != row.stable_instructions) {
      changes.push_back(
          {row.address, int64_t(it->second), int64_t(row.stable_instructions)});
    }
  }
  std::sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) {
    return a.after - a.before < b.after - b.before;
  });
  const auto worse = std::count_if(changes.begin(), changes.end(),
                                   [](auto& c) { return c.after > c.before; });
  fmt::print(
      "baseline  {}: {} functions in both, stable {} -> {} ({:+.3f}%), "
      "{} better, {} worse\n",
      xe::path_to_utf8(path), common, before, after,
      before ? (double(after) / double(before) - 1.0) * 100.0 : 0.0,
      changes.size() - worse, worse);
  for (size_t i = 0; i < std::min<size_t>(worse, 20); ++i) {
    const Change& c = changes[changes.size() - 1 - i];
    fmt::print("  worse   {:08X} {:+} ({} -> {})\n", c.address,
               c.after - c.before, c.before, c.after);
  }
  for (size_t i = 0; i < std::min<size_t>(changes.size() - worse, 20); ++i) {
    const Change& c = changes[i];
    fmt::print("  better  {:08X} {:+} ({} -> {})\n", c.address,
               c.after - c.before, c.before, c.after);
  }
  return true;
}

int cpu_replay_main(const std::vector<std::string>& args) {
  if (cvars::replay_corpus.empty()) {
    fmt::print(
        stderr,
        "Usage: {} <corpus> [--replay_csv=<file>] [--replay_baseline=<file>] "
        "[--replay_dump=<address>]\n",
        args[0]);
    return 1;
  }
  JitCorpus corpus;
  std::string error;
  if (!JitCorpus::Read(cvars::replay_corpus, &corpus, &error)) {
    fmt::print(stderr, "{}: {}\n", xe::path_to_utf8(cvars::replay_corpus),
               error);
    return 1;
  }

  // Compile under the capture's settings, before anything latches them. The
  // other backend's cannot affect this one's code.
#if XE_ARCH_AMD64
  const char* other_backend = "a64";
#else
  const char* other_backend = "x64";
#endif  // XE_ARCH_AMD64
  for (const auto& [name, category, value] : corpus.cvars) {
    uint64_t current = 0;
    if (!JitCorpus::SetCvar(name, value) ||
        !JitCorpus::GetCvar(name, &current)) {
      if (category == other_backend) {
        continue;
      }
      fmt::print(stderr, "Recorded cvar {} does not exist in this build\n",
                 name);
      return 1;
    }
    if (current != value) {
      fmt::print("cvar      {} = {} from the command line, {} recorded\n", name,
                 current, value);
    }
  }
  const uint32_t dump_address =
      cvars::replay_dump.empty()
          ? 0
          : xe::string_util::from_string<uint32_t>(cvars::replay_dump, true);

  auto memory = std::make_unique<Memory>();
  if (!memory->Initialize()) {
    return 1;
  }
  std::unique_ptr<backend::Backend> backend;
#if XE_ARCH_AMD64
  backend = std::make_unique<backend::x64::X64Backend>();
#elif XE_ARCH_ARM64
  backend = std::make_unique<backend::a64::A64Backend>();
#endif  // XE_ARCH
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  if (!processor->Setup(std::move(backend))) {
    fmt::print(stderr, "Unable to set up the backend\n");
    return 1;
  }

  for (const auto& range : corpus.mmio_ranges) {
    memory->AddVirtualMappedRange(range.address, range.mask, range.size,
                                  nullptr, nullptr, nullptr);
  }
  // With the word after, zero unless recorded, as that is where the scanner
  // may have found the end of the function.
  for (const auto& record : corpus.functions) {
    const uint32_t size = uint32_t(record.code.size());
    if (size && !MapGuestMemory(memory.get(), record.address,
                                record.code.data(), size, size + 4)) {
      fmt::print(stderr, "Unable to map code at {:08X}\n", record.address);
      return 1;
    }
    processor->backend()->CommitExecutableRange(record.address,
                                                record.address + size);
  }
  processor->AddModule(std::make_unique<CorpusModule>(processor.get(), corpus));

  // In capture order, with the page protection of the time.
  std::vector<Row> rows;
  std::unordered_set<uint32_t> compiled;
  uint32_t failed = 0, duplicates = 0, extents_differ = 0, identical = 0;
  Row total = {};
  auto page = corpus.pages.begin();
  for (const auto& record : corpus.functions) {
    for (; page != corpus.pages.end() &&
           page->position <= size_t(&record - corpus.functions.data());
         ++page) {
      if (!ProtectPage(memory.get(), *page)) {
        fmt::print(stderr, "Unable to map page {:08X}\n", page->address);
        return 1;
      }
    }
    if (!compiled.insert(record.address).second) {
      ++duplicates;
      continue;
    }
    cvars::disassemble_functions = record.address == dump_address;
    auto function =
        static_cast<GuestFunction*>(processor->ResolveFunction(record.address));
    if (!function) {
      fmt::print(stderr, "Unable to compile {:08X}\n", record.address);
      ++failed;
      continue;
    }
    extents_differ += function->end_address() != record.end_address;
    uint32_t instructions, stable;
    processor->backend()->CountHostInstructions(function->machine_code(),
                                                function->machine_code_length(),
                                                &instructions, &stable);
    Row row = {record.address,
               record.code.size() / 4,
               function->machine_code_length(),
               instructions,
               stable,
               record.stable_instructions};
    identical += row.stable_instructions == row.captured_stable_instructions;
    total.guest_instructions += row.guest_instructions;
    total.host_bytes += row.host_bytes;
    total.host_instructions += row.host_instructions;
    total.stable_instructions += row.stable_instructions;
    total.captured_stable_instructions += row.captured_stable_instructions;
    rows.push_back(row);
    if (cvars::disassemble_functions) {
      auto text = [](const char* s) { return s ? s : ""; };
      auto debug_info = function->debug_info();
      fmt::print("{}\n{}\n{}\n", text(debug_info->source_disasm()),
                 text(debug_info->hir_disasm()),
                 text(debug_info->machine_code_disasm()));
    }
  }

  fmt::print(
      "corpus    {}: {} functions, {} declarations, {} page protection "
      "changes{}\n",
      xe::path_to_utf8(cvars::replay_corpus), corpus.functions.size(),
      corpus.declarations.size(), corpus.pages.size(),
      corpus.truncated ? ", last record truncated" : "");
  fmt::print("compiled  {} ({} failed, {} duplicates, {} extents differ)\n",
             rows.size(), failed, duplicates, extents_differ);
  fmt::print("guest     {} instructions\n", total.guest_instructions);
  fmt::print("host      {} bytes, {} instructions, {} stable\n",
             total.host_bytes, total.host_instructions,
             total.stable_instructions);
  fmt::print("capture   {} stable, {} of {} functions the same\n",
             total.captured_stable_instructions, identical, rows.size());
  if (!cvars::replay_csv.empty() && !WriteRows(cvars::replay_csv, rows)) {
    return 1;
  }
  if (!cvars::replay_baseline.empty() &&
      !CompareRows(cvars::replay_baseline, rows)) {
    return 1;
  }
  return failed ? 1 : 0;
}

}  // namespace
}  // namespace cpu
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia-cpu-replay", xe::cpu::cpu_replay_main, "<corpus>",
                      "replay_corpus");
