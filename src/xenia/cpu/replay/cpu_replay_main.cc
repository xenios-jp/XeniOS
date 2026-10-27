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
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/replay/invocation_replay.h"
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
DEFINE_path(
    replay_coverage, "",
    "Count how often each guest instruction runs in the replays and "
    "write the totals by function and by backend sequence to this "
    "file. Slows the replays, so their times say nothing. With --replay_dump, "
    "the counts of that function are printed with its code.",
    "General");
DEFINE_bool(replay_count_instructions, false,
            "Also count the host instructions each captured invocation "
            "retires, which unlike its time does not vary with the host's "
            "load. macOS only.",
            "General");
DEFINE_bool(replay_efficiency_cores, false,
            "Time captured invocations on the efficiency cores, where a phone "
            "runs most guest threads, rather than the performance cores.",
            "General");

namespace xe {
namespace cpu {
namespace {

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
                                   InvocationReplay::ExternHandler);
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
  // Host time per captured invocation, 0 if none was timed, and the host
  // CPU samples the capture took in the function.
  double ns_per_invocation;
  uint32_t samples;
  // Host instructions retired per captured invocation, 0 if not counted.
  double instructions_per_invocation;
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
             "stable_instructions,captured_stable_instructions,"
             "ns_per_invocation,samples,instructions_per_invocation\n");
  for (const Row& row : rows) {
    fmt::print(file, "{:08X},{},{},{},{},{},{:.1f},{},{:.1f}\n", row.address,
               row.guest_instructions, row.host_bytes, row.host_instructions,
               row.stable_instructions, row.captured_stable_instructions,
               row.ns_per_invocation, row.samples,
               row.instructions_per_invocation);
  }
  fclose(file);
  return true;
}

// Prints how a per-invocation measure of the functions measured in both moved,
// in all weighted by samples, and the functions that moved the total the most.
void CompareMeasure(const char* name, const char* unit, const char* more,
                    const char* less, const std::vector<Row>& rows,
                    const std::unordered_map<uint32_t, double>& baseline,
                    double Row::* measure) {
  struct Change {
    uint32_t address;
    double before;
    double after;
    double impact;
  };
  std::vector<Change> changes;
  double weight = 0.0, weighted = 0.0;
  for (const Row& row : rows) {
    auto it = baseline.find(row.address);
    if (it == baseline.end() || it->second <= 0.0 || row.*measure <= 0.0) {
      continue;
    }
    const double ratio = row.*measure / it->second;
    changes.push_back(
        {row.address, it->second, row.*measure, row.samples * (ratio - 1.0)});
    weight += row.samples;
    weighted += row.samples * ratio;
  }
  if (changes.empty()) {
    return;
  }
  std::sort(changes.begin(), changes.end(),
            [](const auto& a, const auto& b) { return a.impact > b.impact; });
  fmt::print(
      "{:<9} {} functions measured in both: {:+.2f}% weighted by "
      "samples\n",
      name, changes.size(), (weighted / weight - 1.0) * 100.0);
  for (size_t i = 0; i < changes.size(); ++i) {
    const Change& c = changes[i];
    const bool increased = c.after > c.before;
    if (increased ? i < 10 : changes.size() - i <= 10) {
      fmt::print("  {}  {:08X} {:+.2f}% ({:.0f} -> {:.0f} {})\n",
                 increased ? more : less, c.address,
                 (c.after / c.before - 1.0) * 100.0, c.before, c.after, unit);
    }
  }
}

// Stable instruction counts, and host time and instructions per captured
// invocation, against a CSV from another build or cvar set.
bool CompareRows(const std::filesystem::path& path,
                 const std::vector<Row>& rows) {
  FILE* file = xe::filesystem::OpenFile(path, "r");
  if (!file) {
    fmt::print(stderr, "Unable to open {}\n", xe::path_to_utf8(path));
    return false;
  }
  std::unordered_map<uint32_t, uint64_t> baseline;
  std::unordered_map<uint32_t, double> baseline_ns, baseline_instructions;
  char line[256];
  while (fgets(line, sizeof(line), file)) {
    unsigned int address;
    unsigned long long stable;
    double ns = 0.0, instructions = 0.0;
    if (sscanf(line, "%X,%*u,%*u,%*u,%llu,%*u,%lf,%*u,%lf", &address, &stable,
               &ns, &instructions) >= 2) {
      baseline[address] = stable;
      baseline_ns[address] = ns;
      baseline_instructions[address] = instructions;
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
    const uint64_t base_stable = it->second;
    ++common;
    before += base_stable;
    after += row.stable_instructions;
    if (base_stable != row.stable_instructions) {
      changes.push_back({row.address, int64_t(base_stable),
                         int64_t(row.stable_instructions)});
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
  CompareMeasure("time", "ns", "slower", "faster", rows, baseline_ns,
                 &Row::ns_per_invocation);
  CompareMeasure("retired", "instructions", "more", "fewer", rows,
                 baseline_instructions, &Row::instructions_per_invocation);
  return true;
}

// Runs every captured invocation once, checking it against the capture, then
// times each function's in the order they were captured, over several runs.
// The first replay of a capture keeps only the invocations it reproduces, and
// rewrites the corpus with just the memory they touch.
bool ReplayInvocations(Processor* processor, const JitCorpus& corpus,
                       std::vector<Row>* rows) {
  InvocationReplay replay(processor, corpus);
  if (!replay.Initialize()) {
    fmt::print(stderr, "Unable to map guest memory\n");
    return false;
  }
  const bool captured = !corpus.invocations.front().checked;
  std::vector<JitCorpus::Invocation> checked;
  std::unordered_set<uint32_t> mismatched;
  uint32_t others = 0, reading_others = 0, unrecorded = 0;
  for (size_t i = 0; i < corpus.invocations.size(); ++i) {
    const auto& invocation = corpus.invocations[i];
    JitCorpus::Invocation result;
    uint32_t pages = 0;
    const std::string mismatch =
        replay.Check(invocation, &result, &pages, &unrecorded);
    if (mismatch.empty()) {
      checked.push_back(std::move(result));
      others += pages;
      reading_others += pages != 0;
      continue;
    }
    if (!captured) {
      mismatched.insert(invocation.function);
    }
    fmt::print("{}  {:08X} invocation {}: {}\n",
               captured ? "dropped " : "MISMATCH", invocation.function, i,
               mismatch);
  }
  if (captured) {
    const uint64_t size = std::filesystem::file_size(cvars::replay_corpus);
    if (!corpus.Rewrite(cvars::replay_corpus, checked)) {
      fmt::print(stderr, "Unable to rewrite the corpus\n");
      return false;
    }
    fmt::print(
        "checked   {} of {} captured invocations reproduce the capture, {} of "
        "them touching {} pages something else changed meanwhile; corpus "
        "rewritten with the pages they touch, {} MB -> {} MB\n",
        checked.size(), corpus.invocations.size(), reading_others, others,
        size >> 20, std::filesystem::file_size(cvars::replay_corpus) >> 20);
  } else {
    fmt::print("exact     {} of {} invocations{}\n", checked.size(),
               corpus.invocations.size(),
               unrecorded ? fmt::format(", touching {} pages the first replay "
                                        "did not",
                                        unrecorded)
                          : std::string());
  }

  // Each function's invocations in capture order, hottest function first.
  std::unordered_map<uint32_t, std::vector<const JitCorpus::Invocation*>>
      by_function;
  for (const auto& invocation : checked) {
    by_function[invocation.function].push_back(&invocation);
  }
  uint64_t guest_samples = 0, covered_samples = 0;
  std::vector<std::pair<uint32_t, uint32_t>> timed;
  for (const auto& [address, samples] : corpus.profile) {
    guest_samples += samples;
    if (by_function.count(address) && !mismatched.count(address)) {
      covered_samples += samples;
      timed.emplace_back(address, samples);
    }
  }
  fmt::print(
      "coverage  {:.1f}% of the samples in guest code are in the {} "
      "functions reproduced ({} of {}, {} samples in all)\n",
      guest_samples ? 100.0 * covered_samples / guest_samples : 0.0,
      timed.size(), covered_samples, guest_samples, corpus.profile_samples);

  if (cvars::replay_efficiency_cores &&
      !xe::threading::PreferEfficiencyCores()) {
    fmt::print("--replay_efficiency_cores has no effect on this platform\n");
  }
  std::unordered_map<uint32_t, Row*> row_of;
  for (Row& row : *rows) {
    row_of[row.address] = &row;
  }
  // The host clock may tick only every 42 ns, which some invocations take
  // less than, so each is run alone many times over, in capture order, and
  // the mean taken: of many runs it resolves far below a tick. Runs over twice
  // the 90th percentile, which were interrupted, are left out. The functions
  // take turns over several rounds, and the fastest round counts, as other
  // work on the host can only slow a round down.
  const uint64_t frequency = Clock::host_tick_frequency_platform();
  constexpr size_t kRounds = 8;
  const uint64_t budget = frequency / 40;
  std::vector<std::vector<double>> rounds(timed.size());
  size_t runs = 0;
  for (size_t round = 0; round < kRounds; ++round) {
    for (size_t f = 0; f < timed.size(); ++f) {
      const auto& invocations = by_function[timed[f].first];
      std::vector<std::vector<uint64_t>> times(invocations.size());
      const uint64_t start = Clock::host_tick_count_platform();
      // The first run of each warms up and is left out.
      for (size_t run = 0;
           run < 2 || Clock::host_tick_count_platform() - start < budget;
           ++run) {
        for (size_t k = 0; k < invocations.size(); ++k) {
          const uint64_t time = replay.Time(*invocations[k]);
          if (run) {
            times[k].push_back(time);
          }
        }
      }
      double ticks = 0.0;
      for (const auto& invocation_times : times) {
        std::vector<uint64_t> sorted = invocation_times;
        auto p90 = sorted.begin() + sorted.size() * 9 / 10;
        std::nth_element(sorted.begin(), p90, sorted.end());
        uint64_t sum = 0, count = 0;
        for (uint64_t time : invocation_times) {
          if (time <= 2 * *p90 + 1) {
            sum += time;
            ++count;
          }
        }
        ticks += double(sum) / double(count) / double(times.size());
      }
      rounds[f].push_back(ticks * 1e9 / double(frequency));
      runs += times[0].size();
    }
  }
  double weighted = 0.0;
  for (size_t f = 0; f < timed.size(); ++f) {
    const auto [address, samples] = timed[f];
    const double ns = *std::min_element(rounds[f].begin(), rounds[f].end());
    if (auto it = row_of.find(address); it != row_of.end()) {
      it->second->ns_per_invocation = ns;
    }
    weighted += samples * ns;
    fmt::print("  {:08X} {:5.1f}% of samples, {} invocations, {:.1f} ns\n",
               address, 100.0 * samples / guest_samples,
               by_function[address].size(), ns);
  }
  fmt::print(
      "time      {:.0f} ns per invocation weighted by samples, over {} "
      "functions on the {} cores, {} runs of each invocation\n",
      covered_samples ? weighted / covered_samples : 0.0, timed.size(),
      cvars::replay_efficiency_cores ? "efficiency" : "performance",
      timed.empty() ? 0 : runs / timed.size());

  if (cvars::replay_count_instructions) {
    // Reading the counter is a system call, which retires thousands of
    // instructions, a varying few dozen of them, and interrupts only add to a
    // count. So each invocation is restored and run kCountRuns times between
    // two reads, and restored as many times alone, which is the same work
    // every time; the fewest of kCountBatches of each are taken.
    constexpr uint32_t kCountRuns = 16;
    constexpr size_t kCountBatches = 8;
    double weighted_instructions = 0.0;
    for (const auto& [address, samples] : timed) {
      const auto& invocations = by_function[address];
      double instructions = 0.0;
      for (const JitCorpus::Invocation* invocation : invocations) {
        uint64_t with_runs = UINT64_MAX, without = UINT64_MAX;
        for (size_t batch = 0; batch < kCountBatches; ++batch) {
          with_runs = std::min(with_runs, replay.CountInstructions(
                                              *invocation, kCountRuns, true));
          without = std::min(without, replay.CountInstructions(
                                          *invocation, kCountRuns, false));
        }
        instructions +=
            double(with_runs - std::min(with_runs, without)) / kCountRuns;
      }
      instructions /= double(invocations.size());
      if (auto it = row_of.find(address); it != row_of.end()) {
        it->second->instructions_per_invocation = instructions;
      }
      weighted_instructions += samples * instructions;
    }
    fmt::print(
        "retired   {:.0f} host instructions per invocation weighted by "
        "samples\n",
        covered_samples ? weighted_instructions / covered_samples : 0.0);
  }
  return mismatched.empty();
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

  ExceptionHandler::Install(InvocationReplay::HandleException, nullptr);
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
  if (!cvars::replay_coverage.empty()) {
    cvars::trace_function_coverage = true;
  }
  if (!processor->Setup(std::move(backend))) {
    fmt::print(stderr, "Unable to set up the backend\n");
    return 1;
  }

  for (const auto& range : corpus.mmio_ranges) {
    memory->AddVirtualMappedRange(range.address, range.mask, range.size,
                                  nullptr, InvocationReplay::MmioRead,
                                  InvocationReplay::MmioWrite);
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
  std::unordered_map<uint32_t, uint32_t> samples_of(corpus.profile.begin(),
                                                    corpus.profile.end());
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
               record.stable_instructions,
               0.0,
               0,
               0.0};
    if (auto it = samples_of.find(record.address); it != samples_of.end()) {
      row.samples = it->second;
    }
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
  if (!corpus.invocations.empty() &&
      !ReplayInvocations(processor.get(), corpus, &rows)) {
    return 1;
  }
  if (!cvars::replay_coverage.empty()) {
    FILE* file = xe::filesystem::OpenFile(cvars::replay_coverage, "w");
    if (!file) {
      fmt::print(stderr, "Unable to write {}\n",
                 xe::path_to_utf8(cvars::replay_coverage));
      return 1;
    }
    processor->DumpTraceCounts(file);
    std::fclose(file);
    if (dump_address) {
      const std::vector<uint64_t> counts =
          processor->GetTraceCounts(dump_address);
      fmt::print("counts    {:08X}\n", dump_address);
      for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i]) {
          fmt::print("  {:08X} {}\n", dump_address + uint32_t(i * 4),
                     counts[i]);
        }
      }
    }
  }
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
