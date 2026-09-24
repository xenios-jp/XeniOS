/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_REPLAY_INVOCATION_REPLAY_H_
#define XENIA_CPU_REPLAY_INVOCATION_REPLAY_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/exception_handler.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

// Runs the invocations a capture recorded on the functions compiled from its
// corpus, each from the registers and memory it started with.
class InvocationReplay {
 public:
  InvocationReplay(Processor* processor, const JitCorpus& corpus);
  ~InvocationReplay();

  // Page faults must reach this before the handlers a processor installs, so
  // it is installed before one is set up.
  static bool HandleException(Exception* ex, void* data);

  // Makes guest memory accessible.
  bool Initialize();

  // Runs an invocation once, watching which pages it touches, and compares
  // the registers and pages it ends with to the capture. Returns the first
  // difference, or nothing and |checked|, the invocation as later replays run
  // it: with only the pages it touched, and the ones it changed.
  //
  // Captured invocations must come in the order they were recorded, as each
  // one's memory is a difference from the last one's. A page such a run left
  // alone but the capture saw change was written by something else, such as
  // the GPU: those it touched are counted in |others|, and cannot have
  // affected it if the rest matches. A checked invocation run by code built
  // differently may touch pages the first run did not, such as constants the
  // first one folded; |unrecorded| counts those, holding what the corpus
  // mapped there.
  std::string Check(const JitCorpus::Invocation& invocation,
                    JitCorpus::Invocation* checked, uint32_t* others,
                    uint32_t* unrecorded);
  // Runs a checked invocation from its entry state, returning the host clock
  // ticks the function took.
  uint64_t Time(const JitCorpus::Invocation& invocation);

  // An extern handler, reproducing the next export call the capture
  // recorded.
  static void ExternHandler(ppc::PPCContext* context,
                            kernel::KernelState* kernel_state);
  // MMIO callbacks. The capture did not record MMIO, so an invocation reading
  // it does not replay.
  static uint32_t MmioRead(void* context, void* callback_context,
                           uint32_t address);
  static void MmioWrite(void* context, void* callback_context, uint32_t address,
                        uint32_t value);

 private:
  uint8_t* HostPage(uint32_t address) const;
  void WritePage(uint32_t address, uint64_t hash);
  bool SetAccess(xe::memory::PageAccess access);
  // Sets the entry registers and runs, watching the pages touched or not.
  Function* Run(const JitCorpus::Invocation& invocation, bool watch);
  // The guest pages in the host pages the last watched run touched.
  std::vector<uint32_t> TouchedPages() const;

  Processor* processor_;
  Memory* memory_;
  const JitCorpus& corpus_;
  std::unique_ptr<ThreadState> thread_state_;
  // Host views of the guest heaps, and the host pages among them a watched run
  // touched, by index from the virtual membase.
  std::vector<std::pair<uint8_t*, size_t>> views_;
  size_t host_page_size_ = 0;
  std::vector<uint8_t> touched_;
  bool watching_ = false;
  // Captured invocations: the entry memory of the last one checked, and what
  // guest memory holds.
  JitCorpus::PageList entry_;
  JitCorpus::PageList loaded_;
  // The invocation running, its next export call and whether it read MMIO.
  const JitCorpus::Invocation* invocation_ = nullptr;
  size_t next_export_ = 0;
  bool diverged_ = false;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_REPLAY_INVOCATION_REPLAY_H_
