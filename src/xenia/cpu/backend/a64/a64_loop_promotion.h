/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_LOOP_PROMOTION_H_
#define XENIA_CPU_BACKEND_A64_A64_LOOP_PROMOTION_H_

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/cpu/hir/hir_builder.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

// A guest register that a loop keeps in a host register the allocator left
// unused in the whole loop. Loaded on entry, written back on every exit and
// around the preempt safepoint's yield, so memory holds it wherever anything
// but the loop's own code can look.
struct PromotedSlot {
  uint32_t offset;
  hir::TypeName type;  // INT8_TYPE, INT64_TYPE or VEC128_TYPE.
  int32_t reg_index;   // In the allocator's set for the type.
  bool dirty;          // Stored in the loop.
  // Only stored, last in the tail with a value another slot's register holds
  // from there to the tail's end. It shares that register and its stores emit
  // nothing: the loop leaves only through the tail's end, and the yield at the
  // head comes only after a pass through it.
  bool deferred = false;
};

struct PromotedLoop {
  const hir::Block* head = nullptr;
  const hir::Block* tail = nullptr;
  std::vector<PromotedSlot> slots;
  // Labels outside the loop that its branches target.
  std::unordered_set<uint32_t> exit_label_ids;
  // The head's preempt check, which the entry jumps past while a deferred
  // slot's register still holds another slot's value.
  const hir::Instr* entry_skips = nullptr;
};

struct LoopPromotionPlan {
  std::vector<PromotedLoop> loops;
  // Every load_context and store_context of a promoted slot.
  std::unordered_map<const hir::Instr*, const PromotedSlot*> accesses;
  // Values recolored into a slot's register, with the allocator's register.
  std::vector<std::pair<hir::Value*, int32_t>> recolored;

  // Gives recolored values their allocated registers back, leaving the HIR as
  // it was.
  void Clear() {
    // Last first, as a value may have been recolored more than once.
    for (auto it = recolored.rbegin(); it != recolored.rend(); ++it) {
      it->first->reg.index = it->second;
    }
    recolored.clear();
    loops.clear();
    accesses.clear();
  }
  const PromotedLoop* LoopAt(const hir::Block* head) const {
    for (const PromotedLoop& loop : loops) {
      if (loop.head == head) {
        return &loop;
      }
    }
    return nullptr;
  }
};

// Plans the promotion after register allocation. Values loaded from or stored
// to a promoted slot are recolored into its register where that is exact, so
// those accesses emit no code; the rest become register moves.
void PlanLoopPromotion(hir::HIRBuilder* builder, LoopPromotionPlan* plan);

// Whether code at the end of the block runs on into the next one.
bool BlockFallsThrough(const hir::Block* block);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_LOOP_PROMOTION_H_
