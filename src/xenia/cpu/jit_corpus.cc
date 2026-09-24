/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/jit_corpus.h"

#include <cstring>
#include <type_traits>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

namespace {

enum : uint32_t {
  kTagCvar = 1,
  kTagMmioRange,
  kTagPage,
  kTagDeclaration,
  kTagFunction,
  kTagProfile,
  kTagMemoryPage,
  kTagInvocation,
};

constexpr uint32_t kFlagRestore = 1u << 10;
constexpr uint32_t kFlagExternHandler = 1u << 11;

// The cvars codegen can read: the CPU ones, and two from elsewhere.
bool IsCodegenCvar(const cvar::IConfigVar* var) {
  const std::string& category = var->category();
  return category == "CPU" || category == "a64" || category == "x64" ||
         var->name() == "debug" || var->name() == "guest_scheduler";
}

// Calls visit with the cvar as its ConfigVar<T>, for the types codegen uses.
template <typename Visit>
bool VisitCvar(const std::string& name, Visit visit) {
  if (!cvar::ConfigVars) {
    return false;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return false;
  }
  auto as = [&](auto* typed) {
    if (typed) {
      visit(typed);
    }
    return typed != nullptr;
  };
  cvar::IConfigVar* var = it->second;
  return as(dynamic_cast<cvar::ConfigVar<bool>*>(var)) ||
         as(dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) ||
         as(dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) ||
         as(dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) ||
         as(dynamic_cast<cvar::ConfigVar<uint64_t>*>(var));
}

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
  bool at_end() const { return offset_ == data_.size(); }
  size_t remaining() const { return data_.size() - offset_; }
  const uint8_t* Skip(size_t size) {
    offset_ += size;
    return data_.data() + offset_ - size;
  }
  bool Read(void* out, size_t size) {
    if (data_.size() - offset_ < size) {
      return false;
    }
    std::memcpy(out, data_.data() + offset_, size);
    offset_ += size;
    return true;
  }
  bool Read(std::vector<uint8_t>* out, size_t size) {
    out->resize(size);
    return Read(out->data(), size);
  }
  template <typename T>
  bool Read(T* out) {
    return Read(out, sizeof(T));
  }

 private:
  const std::vector<uint8_t>& data_;
  size_t offset_ = 0;
};

bool ReadPageList(Reader& reader, JitCorpus::PageList* out) {
  uint32_t count;
  if (!reader.Read(&count) || count > (1u << 20)) {
    return false;
  }
  out->resize(count);
  for (auto& [address, hash] : *out) {
    if (!reader.Read(&address) || !reader.Read(&hash)) {
      return false;
    }
  }
  return true;
}

bool ReadInvocation(Reader& reader, JitCorpus::Invocation* out) {
  uint32_t count;
  if (!reader.Read(&out->function) || !reader.Read(&out->return_address) ||
      !reader.Read(&out->thread) ||
      !reader.Read(&out->entry_registers, JitCorpus::kRegistersSize) ||
      !reader.Read(&out->exit_registers, JitCorpus::kRegistersSize) ||
      !reader.Read(&count) || count > (1u << 16)) {
    return false;
  }
  out->exports.resize(count);
  for (auto& call : out->exports) {
    uint32_t length;
    std::vector<uint8_t> name;
    if (!reader.Read(&call.return_address) || !reader.Read(&call.result) ||
        !reader.Read(&length) || length > 256 || !reader.Read(&name, length)) {
      return false;
    }
    call.name.assign(name.begin(), name.end());
  }
  return ReadPageList(reader, &out->memory) &&
         ReadPageList(reader, &out->writes);
}

void Append(std::vector<uint8_t>* out, const void* data, size_t size) {
  auto bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

void AppendPageList(std::vector<uint8_t>* out,
                    const JitCorpus::PageList& pages) {
  const uint32_t count = uint32_t(pages.size());
  Append(out, &count, sizeof(count));
  for (const auto& [address, hash] : pages) {
    Append(out, &address, sizeof(address));
    Append(out, &hash, sizeof(hash));
  }
}

void AppendInvocation(std::vector<uint8_t>* out,
                      const JitCorpus::Invocation& invocation) {
  const uint32_t header[] = {kTagInvocation, invocation.function,
                             invocation.return_address, invocation.thread};
  Append(out, header, sizeof(header));
  Append(out, invocation.entry_registers.data(), JitCorpus::kRegistersSize);
  Append(out, invocation.exit_registers.data(), JitCorpus::kRegistersSize);
  const uint32_t export_count = uint32_t(invocation.exports.size());
  Append(out, &export_count, sizeof(export_count));
  for (const auto& call : invocation.exports) {
    const uint32_t length = uint32_t(call.name.size());
    Append(out, &call.return_address, sizeof(call.return_address));
    Append(out, &call.result, sizeof(call.result));
    Append(out, &length, sizeof(length));
    Append(out, call.name.data(), length);
  }
  AppendPageList(out, invocation.memory);
  AppendPageList(out, invocation.writes);
}

bool ReadFunction(Reader& reader, JitCorpus::FunctionRecord* out) {
  uint32_t count;
  return reader.Read(&out->address) && reader.Read(&out->end_address) &&
         reader.Read(&out->flags) && reader.Read(&out->host_code_size) &&
         reader.Read(&out->stable_instructions) && reader.Read(&count) &&
         count < (1u << 24) && reader.Read(&out->code, size_t(count) * 4) &&
         reader.Read(&out->mmio, (count + 7) / 8);
}

}  // namespace

uint32_t JitCorpus::PackSymbolFlags(const GuestFunction* function) {
  return uint32_t(function->behavior()) |
         (uint32_t(function->SaverestType()) << 8) |
         (function->IsRestore() ? kFlagRestore : 0) |
         (function->extern_handler() ? kFlagExternHandler : 0) |
         (function->SaverestIndex() << 16);
}

void JitCorpus::UnpackSymbolFlags(uint32_t flags, GuestFunction* function,
                                  GuestFunction::ExternHandler extern_handler) {
  auto behavior = Function::Behavior(flags & 0xFF);
  if (behavior == Function::Behavior::kExtern) {
    function->SetupExtern((flags & kFlagExternHandler) ? extern_handler
                                                       : nullptr);
  } else {
    function->set_behavior(behavior);
  }
  auto saverest_type = SaveRestoreType((flags >> 8) & 0x3);
  if (saverest_type != SaveRestoreType::NONE) {
    function->SetSaverest(saverest_type, (flags & kFlagRestore) != 0,
                          uint8_t(flags >> 16));
  }
}

uint64_t JitCorpus::HashMemoryPage(const void* data) {
  const uint64_t hash = XXH3_64bits(data, kMemoryPageSize);
  return hash ? hash : 1;
}

JitCorpus::PageList JitCorpus::DiffPages(const PageList& from,
                                         const PageList& to) {
  PageList out;
  auto f = from.begin(), t = to.begin();
  while (f != from.end() || t != to.end()) {
    if (t == to.end() || (f != from.end() && f->first < t->first)) {
      out.emplace_back((f++)->first, 0);
    } else if (f == from.end() || t->first < f->first) {
      out.push_back(*t++);
    } else {
      if (f->second != t->second) {
        out.push_back(*t);
      }
      ++f, ++t;
    }
  }
  return out;
}

bool JitCorpus::GetCvar(const std::string& name, uint64_t* value) {
  return VisitCvar(
      name, [&](auto* typed) { *value = uint64_t(*typed->current_value()); });
}

bool JitCorpus::SetCvar(const std::string& name, uint64_t value) {
  return VisitCvar(name, [&](auto* typed) {
    using T = std::remove_pointer_t<decltype(typed->current_value())>;
    typed->SetConfigValue(T(value));
  });
}

bool JitCorpus::Read(const std::filesystem::path& path, JitCorpus* out,
                     std::string* error) {
  std::vector<uint8_t>& data = out->data;
  if (FILE* file = xe::filesystem::OpenFile(path, "rb")) {
    data.resize(std::filesystem::file_size(path));
    if (!data.empty() && fread(data.data(), data.size(), 1, file) != 1) {
      data.clear();
    }
    fclose(file);
  }
  Reader reader(data);
  uint32_t magic = 0, version = 0;
  if (!reader.Read(&magic) || !reader.Read(&version) || magic != kMagic) {
    *error = "not a JIT corpus";
    return false;
  }
  if (version != kVersion) {
    *error = fmt::format("corpus version {}, this build reads version {}",
                         version, kVersion);
    return false;
  }
  while (!reader.at_end()) {
    uint32_t tag = 0;
    bool complete = reader.Read(&tag);
    if (complete && tag == kTagCvar) {
      uint32_t lengths[2];
      std::vector<uint8_t> name, category;
      uint64_t value;
      complete = reader.Read(&lengths) && lengths[0] < 256 &&
                 lengths[1] < 256 && reader.Read(&name, lengths[0]) &&
                 reader.Read(&category, lengths[1]) && reader.Read(&value);
      if (complete) {
        out->cvars.push_back({std::string(name.begin(), name.end()),
                              std::string(category.begin(), category.end()),
                              value});
      }
    } else if (complete && tag == kTagMmioRange) {
      MmioRange range;
      complete = reader.Read(&range);
      if (complete) {
        out->mmio_ranges.push_back(range);
      }
    } else if (complete && tag == kTagPage) {
      Page page;
      uint32_t read_only;
      complete = reader.Read(&page.address) && reader.Read(&page.size) &&
                 reader.Read(&read_only) &&
                 reader.Read(&page.data, read_only ? page.size : 0);
      if (complete) {
        page.position = out->functions.size();
        out->pages.push_back(std::move(page));
      }
    } else if (complete && (tag == kTagDeclaration || tag == kTagFunction)) {
      FunctionRecord record;
      complete = ReadFunction(reader, &record);
      if (complete) {
        (tag == kTagFunction ? out->functions : out->declarations)
            .push_back(std::move(record));
      }
    } else if (complete && tag == kTagProfile) {
      uint32_t count;
      complete = reader.Read(&out->profile_samples) && reader.Read(&count) &&
                 count < (1u << 24);
      out->profile.resize(complete ? count : 0);
      for (auto& [address, samples] : out->profile) {
        complete = complete && reader.Read(&address) && reader.Read(&samples);
      }
    } else if (complete && tag == kTagMemoryPage) {
      uint64_t hash;
      complete = reader.Read(&hash) && reader.remaining() >= kMemoryPageSize;
      if (complete) {
        out->memory_pages.emplace(hash, reader.Skip(kMemoryPageSize));
      }
    } else if (complete && tag == kTagInvocation) {
      Invocation invocation;
      complete = ReadInvocation(reader, &invocation);
      if (complete) {
        out->invocations.push_back(std::move(invocation));
      }
    } else if (complete) {
      *error = fmt::format("unknown record type {}", tag);
      return false;
    }
    if (!complete) {
      out->truncated = true;
      break;
    }
  }
  return true;
}

std::unique_ptr<JitCorpusWriter> JitCorpusWriter::Create(
    Processor* processor, const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    XELOGE("Unable to create JIT corpus {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    return nullptr;
  }
  auto writer =
      std::unique_ptr<JitCorpusWriter>(new JitCorpusWriter(processor, file));
  const uint32_t header[] = {JitCorpus::kMagic, JitCorpus::kVersion};
  writer->Write(header, sizeof(header));
  return writer;
}

JitCorpusWriter::~JitCorpusWriter() {
  if (file_) {
    fclose(file_);
  }
}

void JitCorpusWriter::RecordFunction(GuestFunction* function) {
  auto global_lock = global_critical_region_.Acquire();
  if (!environment_written_) {
    // At the first compile, by which point per-title config is applied and
    // the MMIO ranges are registered.
    WriteEnvironment();
    environment_written_ = true;
  }
  Module* module = function->module();
  const bool new_module = declared_modules_.insert(module).second;
  if (new_module) {
    module->ForEachFunction([this](Function* callee) {
      if (callee->IsSaverest() && callee->is_guest()) {
        WriteFunction(kTagDeclaration, static_cast<GuestFunction*>(callee),
                      false);
      }
    });
  }
  // New read-only pages come with new modules, so only then is it worth
  // looking for them.
  UpdateReadOnlyPages(new_module);
  WriteFunction(kTagFunction, function, true);
  if (file_) {
    fflush(file_);
  }
}

void JitCorpusWriter::WriteEnvironment() {
  for (const auto& [name, var] : *cvar::ConfigVars) {
    uint64_t value;
    if (IsCodegenCvar(var) && JitCorpus::GetCvar(name, &value)) {
      const std::string& category = var->category();
      const uint32_t header[] = {kTagCvar, uint32_t(name.size()),
                                 uint32_t(category.size())};
      Write(header, sizeof(header));
      Write(name.data(), name.size());
      Write(category.data(), category.size());
      Write(&value, sizeof(value));
    }
  }
  if (MMIOHandler* mmio_handler = MMIOHandler::global_handler()) {
    for (const MMIORange& range : mmio_handler->mapped_ranges()) {
      const uint32_t record[] = {kTagMmioRange, range.address, range.mask,
                                 range.size};
      Write(record, sizeof(record));
    }
  }
}

// Constant propagation folds loads from read-only guest memory.
void JitCorpusWriter::UpdateReadOnlyPages(bool scan) {
  Memory* memory = processor_->memory();
  auto is_read_only = [](BaseHeap* heap, uint32_t address) {
    uint32_t protect;
    return heap->QueryProtect(address, &protect) &&
           (protect & kMemoryProtectRead) && !(protect & kMemoryProtectWrite);
  };
  for (auto it = read_only_pages_.begin(); it != read_only_pages_.end();) {
    BaseHeap* heap = memory->LookupHeap(*it);
    if (is_read_only(heap, *it)) {
      ++it;
      continue;
    }
    const uint32_t record[] = {kTagPage, *it, heap->page_size(), 0};
    Write(record, sizeof(record));
    it = read_only_pages_.erase(it);
  }
  for (uint64_t address = 0; scan && address < 0x100000000ull;) {
    BaseHeap* heap = memory->LookupHeap(uint32_t(address));
    if (!heap) {
      address += 0x1000;
      continue;
    }
    const uint64_t heap_end = uint64_t(heap->heap_base()) + heap->heap_size();
    for (; address < heap_end; address += heap->page_size()) {
      if (is_read_only(heap, uint32_t(address)) &&
          read_only_pages_.insert(uint32_t(address)).second) {
        const uint32_t record[] = {kTagPage, uint32_t(address),
                                   heap->page_size(), 1};
        Write(record, sizeof(record));
        Write(memory->TranslateVirtual(record[1]), record[2]);
      }
    }
  }
}

void JitCorpusWriter::WriteFunction(uint32_t tag, const GuestFunction* function,
                                    bool with_code) {
  const uint32_t address = function->address();
  const uint32_t end_address = function->end_address();
  const uint32_t count =
      with_code && end_address >= address ? (end_address - address) / 4 + 1 : 0;
  std::vector<uint8_t> code(count * 4);
  std::vector<uint8_t> mmio((count + 7) / 8);
  Module* module = function->module();
  for (uint32_t i = 0; i < count; ++i) {
    std::memcpy(&code[i * 4], module->TranslateCode(address + i * 4), 4);
    InfoCacheFlags* flags = module->GetInstructionAddressFlags(address + i * 4);
    if (flags && flags->accessed_mmio) {
      mmio[i / 8] |= uint8_t(1 << (i % 8));
    }
  }
  uint32_t host_code_size = 0, instructions = 0, stable = 0;
  if (with_code) {
    host_code_size = uint32_t(function->machine_code_length());
    processor_->backend()->CountHostInstructions(
        function->machine_code(), host_code_size, &instructions, &stable);
  }
  const uint32_t record[] = {tag,
                             address,
                             end_address,
                             JitCorpus::PackSymbolFlags(function),
                             host_code_size,
                             stable,
                             count};
  Write(record, sizeof(record));
  Write(code.data(), code.size());
  Write(mmio.data(), mmio.size());
}

void JitCorpusWriter::WriteProfile(
    uint64_t samples,
    const std::vector<std::pair<uint32_t, uint32_t>>& profile) {
  auto global_lock = global_critical_region_.Acquire();
  const uint32_t tag = kTagProfile, count = uint32_t(profile.size());
  Write(&tag, sizeof(tag));
  Write(&samples, sizeof(samples));
  Write(&count, sizeof(count));
  for (const auto& [address, function_samples] : profile) {
    Write(&address, sizeof(address));
    Write(&function_samples, sizeof(function_samples));
  }
}

void JitCorpusWriter::WriteMemoryPage(uint64_t hash, const void* data) {
  auto global_lock = global_critical_region_.Acquire();
  const uint32_t tag = kTagMemoryPage;
  Write(&tag, sizeof(tag));
  Write(&hash, sizeof(hash));
  Write(data, JitCorpus::kMemoryPageSize);
}

void JitCorpusWriter::WriteInvocation(const JitCorpus::Invocation& invocation) {
  std::vector<uint8_t> record;
  AppendInvocation(&record, invocation);
  auto global_lock = global_critical_region_.Acquire();
  Write(record.data(), record.size());
  if (file_) {
    fflush(file_);
  }
}

void JitCorpusWriter::Write(const void* data, size_t size) {
  if (file_ && size && fwrite(data, size, 1, file_) != 1) {
    XELOGE("JIT corpus write failed, recording stopped");
    fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace cpu
}  // namespace xe
