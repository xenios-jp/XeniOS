/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_loop_promotion.h"

#include <algorithm>
#include <cstddef>
#include <map>

#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/hir/block.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/label.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace xe::cpu::hir;

namespace {

// Opcodes whose sequences call nothing, touch no context field other than
// through LOAD_CONTEXT/STORE_CONTEXT, and use only scratch registers outside
// the allocator's sets. A loop holding anything else is left alone.
bool IsPromotableOpcode(Opcode num) {
  switch (num) {
    case OPCODE_COMMENT:
    case OPCODE_NOP:
    case OPCODE_SOURCE_OFFSET:
    case OPCODE_CONTEXT_BARRIER:
    case OPCODE_BRANCH:
    case OPCODE_BRANCH_TRUE:
    case OPCODE_BRANCH_FALSE:
    case OPCODE_ASSIGN:
    case OPCODE_CAST:
    case OPCODE_ZERO_EXTEND:
    case OPCODE_SIGN_EXTEND:
    case OPCODE_TRUNCATE:
    case OPCODE_LOAD_VECTOR_SHL:
    case OPCODE_LOAD_VECTOR_SHR:
    case OPCODE_LOAD_CONTEXT:
    case OPCODE_STORE_CONTEXT:
    case OPCODE_LOAD_OFFSET:
    case OPCODE_STORE_OFFSET:
    case OPCODE_LOAD:
    case OPCODE_STORE:
    case OPCODE_LVL:
    case OPCODE_LVR:
    case OPCODE_STVL:
    case OPCODE_STVR:
    case OPCODE_MAX:
    case OPCODE_VECTOR_MAX:
    case OPCODE_MIN:
    case OPCODE_VECTOR_MIN:
    case OPCODE_SELECT:
    case OPCODE_COMPARE_EQ:
    case OPCODE_COMPARE_NE:
    case OPCODE_COMPARE_SLT:
    case OPCODE_COMPARE_SLE:
    case OPCODE_COMPARE_SGT:
    case OPCODE_COMPARE_SGE:
    case OPCODE_COMPARE_ULT:
    case OPCODE_COMPARE_ULE:
    case OPCODE_COMPARE_UGT:
    case OPCODE_COMPARE_UGE:
    case OPCODE_VECTOR_COMPARE_EQ:
    case OPCODE_VECTOR_COMPARE_SGT:
    case OPCODE_VECTOR_COMPARE_SGE:
    case OPCODE_VECTOR_COMPARE_UGT:
    case OPCODE_VECTOR_COMPARE_UGE:
    case OPCODE_ADD:
    case OPCODE_ADD_CARRY:
    case OPCODE_VECTOR_ADD:
    case OPCODE_SUB:
    case OPCODE_VECTOR_SUB:
    case OPCODE_MUL:
    case OPCODE_MUL_HI:
    case OPCODE_NEG:
    case OPCODE_AND:
    case OPCODE_AND_NOT:
    case OPCODE_OR:
    case OPCODE_XOR:
    case OPCODE_NOT:
    case OPCODE_SHL:
    case OPCODE_VECTOR_SHL:
    case OPCODE_SHR:
    case OPCODE_VECTOR_SHR:
    case OPCODE_SHA:
    case OPCODE_VECTOR_SHA:
    case OPCODE_ROTATE_LEFT:
    case OPCODE_VECTOR_ROTATE_LEFT:
    case OPCODE_VECTOR_AVERAGE:
    case OPCODE_BYTE_SWAP:
    case OPCODE_CNTLZ:
    case OPCODE_INSERT:
    case OPCODE_EXTRACT:
    case OPCODE_SPLAT:
    case OPCODE_PERMUTE:
    case OPCODE_SWIZZLE:
      return true;
    default:
      return false;
  }
}

bool IsGprType(TypeName type) {
  return type == INT8_TYPE || type == INT16_TYPE || type == INT32_TYPE ||
         type == INT64_TYPE;
}

// Thread-private guest registers, which nothing but this thread's code and
// whoever has it stopped at a safepoint reads or writes.
bool IsPromotableSlot(uint32_t offset, TypeName type) {
  using ppc::PPCContext;
  switch (type) {
    case INT64_TYPE:
      if (offset >= offsetof(PPCContext, r) &&
          offset < offsetof(PPCContext, r) + sizeof(PPCContext::r)) {
        return (offset - offsetof(PPCContext, r)) % 8 == 0;
      }
      return offset == offsetof(PPCContext, ctr) ||
             offset == offsetof(PPCContext, lr);
    case INT8_TYPE:
      if (offset >= offsetof(PPCContext, cr0) &&
          offset < offsetof(PPCContext, cr7) + sizeof(PPCContext::cr7)) {
        return true;
      }
      return offset == offsetof(PPCContext, xer_ca) ||
             offset == offsetof(PPCContext, xer_ov) ||
             offset == offsetof(PPCContext, xer_so);
    case VEC128_TYPE:
      if (offset >= offsetof(PPCContext, v) &&
          offset < offsetof(PPCContext, v) + sizeof(PPCContext::v)) {
        return (offset - offsetof(PPCContext, v)) % 16 == 0;
      }
      return false;
    default:
      return false;
  }
}

const Instr* LastRealInstr(const Block* block) {
  const Instr* instr = block->instr_tail;
  while (instr && instr->IsFake()) {
    instr = instr->prev;
  }
  return instr;
}

}  // namespace

bool BlockFallsThrough(const Block* block) {
  const Instr* last = LastRealInstr(block);
  if (!last) {
    return true;
  }
  switch (last->GetOpcodeNum()) {
    case OPCODE_BRANCH:
    case OPCODE_RETURN:
      return false;
    case OPCODE_CALL:
    case OPCODE_CALL_INDIRECT:
    case OPCODE_CALL_EXTERN:
      return !(last->flags & CALL_TAIL);
    default:
      return true;
  }
}

namespace {

struct Access {
  const Instr* instr;
  bool store;
};

struct SlotInfo {
  TypeName type;
  bool valid = true;
  bool dirty = false;
  uint32_t uses = 0;
  std::vector<Access> accesses;
};

class BlockOrder {
 public:
  explicit BlockOrder(const Block* block) {
    uint32_t n = 0;
    for (const Instr* i = block->instr_head; i; i = i->next) {
      ordinal_[i] = n++;
    }
  }
  uint32_t of(const Instr* instr) const { return ordinal_.at(instr); }
  // Uses are all in the defining block, checked before planning, but a use
  // list can still name an instruction a pass removed from the block. Such a
  // value counts as live to the end, so nothing is colored over it.
  uint32_t LastUse(const Value* value) const {
    auto def = ordinal_.find(value->def);
    if (def == ordinal_.end()) {
      return UINT32_MAX;
    }
    uint32_t last = def->second;
    for (const Value::Use* use = value->use_head; use; use = use->next) {
      auto it = ordinal_.find(use->instr);
      if (it == ordinal_.end()) {
        return UINT32_MAX;
      }
      last = std::max(last, it->second);
    }
    return last;
  }

 private:
  std::unordered_map<const Instr*, uint32_t> ordinal_;
};

bool IsSlotLoad(const Instr* instr, uint32_t offset) {
  return instr->GetOpcodeNum() == OPCODE_LOAD_CONTEXT &&
         instr->src1.offset == offset;
}
bool IsSlotStore(const Instr* instr, uint32_t offset) {
  return instr->GetOpcodeNum() == OPCODE_STORE_CONTEXT &&
         instr->src1.offset == offset;
}

// Whether the slot's register stays exact through the block when the values in
// `colored` live in it. The register must hold the guest value at the block's
// edges and at every branch, a slot load reads it there, and no colored value
// may be overwritten while it is still to be read.
bool ColoringHolds(const Block* block, const BlockOrder& order, uint32_t offset,
                   const std::unordered_set<const Value*>& colored) {
  bool guest_value_in_reg = true;
  const Value* holder = nullptr;  // Colored value the register holds, if any.
  std::vector<const Value*> defined;
  auto clobbers_live = [&](uint32_t ord) {
    for (const Value* v : defined) {
      if (order.LastUse(v) > ord) {
        return true;
      }
    }
    return false;
  };
  for (const Instr* i = block->instr_head; i; i = i->next) {
    const uint32_t ord = order.of(i);
    if (IsSlotLoad(i, offset)) {
      if (!guest_value_in_reg) {
        return false;
      }
      if (colored.count(i->dest)) {
        // Reads the register in place, overwriting nothing.
        holder = i->dest;
        defined.push_back(i->dest);
      }
      continue;
    }
    if (IsSlotStore(i, offset)) {
      const Value* w = i->src2.value;
      if (!w->IsConstant() && colored.count(w)) {
        if (holder != w) {
          return false;
        }
      } else {
        // A move into the register.
        if (clobbers_live(ord)) {
          return false;
        }
        holder = nullptr;
        defined.clear();
      }
      guest_value_in_reg = true;
      continue;
    }
    if (i->dest && colored.count(i->dest)) {
      if (clobbers_live(ord)) {
        return false;
      }
      holder = i->dest;
      defined.push_back(i->dest);
      guest_value_in_reg = false;
    }
    if (i->BranchLabel() && !guest_value_in_reg) {
      return false;
    }
  }
  return guest_value_in_reg;
}

void ColorSlotInBlock(const Block* block, const PromotedSlot& slot,
                      std::unordered_set<const Value*>* colored_anywhere,
                      LoopPromotionPlan* plan) {
  BlockOrder order(block);
  std::unordered_set<const Value*> colored;
  auto try_color = [&](const Value* v) {
    if (!v || v->IsConstant() || v->type != slot.type || !v->reg.set ||
        v->reg.index < 0 || !v->def || v->def->block != block ||
        colored_anywhere->count(v) || colored.count(v)) {
      return;
    }
    colored.insert(v);
    if (!ColoringHolds(block, order, slot.offset, colored)) {
      colored.erase(v);
    }
  };
  for (const Instr* i = block->instr_head; i; i = i->next) {
    if (IsSlotLoad(i, slot.offset)) {
      try_color(i->dest);
    } else if (IsSlotStore(i, slot.offset)) {
      try_color(i->src2.value);
    }
  }
  for (const Value* v : colored) {
    Value* value = const_cast<Value*>(v);
    plan->recolored.emplace_back(value, value->reg.index);
    value->reg.index = slot.reg_index;
    colored_anywhere->insert(v);
  }
}

void PlanLoop(const std::vector<Block*>& blocks, size_t head, size_t tail,
              const std::vector<std::vector<std::pair<size_t, bool>>>& preds,
              LoopPromotionPlan* plan) {
  // Single entry: only the fall-through from the block before the head, where
  // the entry loads go, and no inner loop.
  if (head == 0) {
    return;
  }
  bool entry_fallthrough = false;
  for (size_t b = head; b <= tail; ++b) {
    for (const auto& [src, is_fallthrough] : preds[b]) {
      if (src >= head && src <= tail) {
        if (b != head && src >= b) {
          return;
        }
        continue;
      }
      if (b != head || src != head - 1 || !is_fallthrough) {
        return;
      }
      entry_fallthrough = true;
    }
  }
  if (!entry_fallthrough) {
    return;
  }

  std::map<uint32_t, SlotInfo> slots;
  bool used_gpr[A64Emitter::GPR_COUNT] = {};
  bool used_vec[A64Emitter::VEC_COUNT] = {};
  PromotedLoop loop;
  loop.head = blocks[head];
  loop.tail = blocks[tail];
  for (size_t b = head; b <= tail; ++b) {
    const Block* block = blocks[b];
    const Instr* first_real = block->instr_head;
    while (first_real && first_real->IsFake()) {
      first_real = first_real->next;
    }
    for (const Instr* i = block->instr_head; i; i = i->next) {
      const Opcode num = i->GetOpcodeNum();
      if (num == OPCODE_CHECK_PREEMPT) {
        if (b != head || i != first_real) {
          return;
        }
        continue;
      }
      if (!IsPromotableOpcode(num)) {
        return;
      }
      const uint32_t signature = i->opcode->signature;
      if (GET_OPCODE_SIG_TYPE_DEST(signature) == OPCODE_SIG_TYPE_V) {
        const Value* dest = i->dest;
        if (dest->type == FLOAT32_TYPE || dest->type == FLOAT64_TYPE) {
          return;
        }
        for (const Value::Use* use = dest->use_head; use; use = use->next) {
          if (use->instr->block != block) {
            return;
          }
        }
        if (dest->reg.index >= 0) {
          if (IsGprType(dest->type)) {
            if (dest->reg.index >= A64Emitter::GPR_COUNT) {
              return;
            }
            used_gpr[dest->reg.index] = true;
          } else {
            if (dest->reg.index >= A64Emitter::VEC_COUNT) {
              return;
            }
            used_vec[dest->reg.index] = true;
          }
        }
      }
      const OpcodeSignatureType src_types[3] = {
          GET_OPCODE_SIG_TYPE_SRC1(signature),
          GET_OPCODE_SIG_TYPE_SRC2(signature),
          GET_OPCODE_SIG_TYPE_SRC3(signature)};
      for (int s = 0; s < 3; ++s) {
        if (src_types[s] != OPCODE_SIG_TYPE_V) {
          continue;
        }
        const Value* src = i->srcs[s].value;
        if (src->type == FLOAT32_TYPE || src->type == FLOAT64_TYPE) {
          return;
        }
        // Values crossing blocks would need the allocator's cooperation.
        if (!src->IsConstant() && (!src->def || src->def->block != block)) {
          return;
        }
      }
      if (const Label* label = i->BranchLabel()) {
        size_t target = 0;
        while (target < blocks.size() && blocks[target] != label->block) {
          ++target;
        }
        if (target < head || target > tail) {
          loop.exit_label_ids.insert(label->id);
        }
      }
      if (num == OPCODE_LOAD_CONTEXT || num == OPCODE_STORE_CONTEXT) {
        const bool store = num == OPCODE_STORE_CONTEXT;
        const uint32_t offset = static_cast<uint32_t>(i->src1.offset);
        const TypeName type = store ? i->src2.value->type : i->dest->type;
        auto [it, inserted] = slots.try_emplace(offset);
        SlotInfo& info = it->second;
        if (inserted) {
          info.type = type;
        }
        if (info.type != type || !IsPromotableSlot(offset, type)) {
          info.valid = false;
        }
        info.dirty |= store;
        ++info.uses;
        info.accesses.push_back({i, store});
      }
    }
  }
  // An access overlapping another slot's bytes makes both unpromotable.
  uint32_t prev_end = 0;
  SlotInfo* prev = nullptr;
  for (auto& [offset, info] : slots) {
    if (prev && offset < prev_end) {
      prev->valid = false;
      info.valid = false;
    }
    const uint32_t end = offset + uint32_t(GetTypeSize(info.type));
    if (!prev || end > prev_end) {
      prev_end = end;
      prev = &info;
    }
  }

  // Busiest slots first, into the registers no value in the loop uses.
  std::vector<std::pair<uint32_t, SlotInfo*>> ranked;
  for (auto& [offset, info] : slots) {
    if (info.valid) {
      ranked.emplace_back(offset, &info);
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto& a, const auto& b) {
                     return a.second->uses > b.second->uses;
                   });
  for (const auto& [offset, info] : ranked) {
    const bool gpr = info->type != VEC128_TYPE;
    bool* used = gpr ? used_gpr : used_vec;
    const int count = gpr ? A64Emitter::GPR_COUNT : A64Emitter::VEC_COUNT;
    int reg = 0;
    while (reg < count && used[reg]) {
      ++reg;
    }
    if (reg == count) {
      continue;
    }
    used[reg] = true;
    loop.slots.push_back({offset, info->type, reg, info->dirty});
  }
  if (loop.slots.empty()) {
    return;
  }

  std::unordered_set<const Value*> colored;
  for (const PromotedSlot& slot : loop.slots) {
    for (size_t b = head; b <= tail; ++b) {
      ColorSlotInBlock(blocks[b], slot, &colored, plan);
    }
  }
  plan->loops.push_back(std::move(loop));
}

}  // namespace

void PlanLoopPromotion(HIRBuilder* builder, LoopPromotionPlan* plan) {
  // A plan left over from an aborted compile names values that are gone.
  plan->recolored.clear();
  plan->Clear();
  std::vector<Block*> blocks;
  for (Block* b = builder->first_block(); b; b = b->next) {
    blocks.push_back(b);
  }
  std::unordered_map<const Block*, size_t> index;
  for (size_t b = 0; b < blocks.size(); ++b) {
    index[blocks[b]] = b;
  }
  // Predecessors as (source, by fall-through).
  std::vector<std::vector<std::pair<size_t, bool>>> preds(blocks.size());
  for (size_t b = 0; b < blocks.size(); ++b) {
    for (const Instr* i = blocks[b]->instr_head; i; i = i->next) {
      if (const Label* label = i->BranchLabel()) {
        // Code that never runs (data decoded as instructions) can branch to a
        // block the passes removed.
        auto target = index.find(label->block);
        if (target == index.end()) {
          return;
        }
        preds[target->second].emplace_back(b, false);
      }
    }
    if (b + 1 < blocks.size() && BlockFallsThrough(blocks[b])) {
      preds[b + 1].emplace_back(b, true);
    }
  }
  for (size_t head = 0; head < blocks.size(); ++head) {
    size_t tail = head;
    bool loops = false;
    for (const auto& [src, is_fallthrough] : preds[head]) {
      if (src >= head) {
        loops = true;
        tail = std::max(tail, src);
      }
    }
    if (loops) {
      PlanLoop(blocks, head, tail, preds, plan);
    }
  }
  for (const PromotedLoop& loop : plan->loops) {
    for (const PromotedSlot& slot : loop.slots) {
      for (size_t b = index.at(loop.head); b <= index.at(loop.tail); ++b) {
        for (const Instr* i = blocks[b]->instr_head; i; i = i->next) {
          if ((i->GetOpcodeNum() == OPCODE_LOAD_CONTEXT ||
               i->GetOpcodeNum() == OPCODE_STORE_CONTEXT) &&
              i->src1.offset == slot.offset) {
            plan->accesses[i] = &slot;
          }
        }
      }
    }
  }
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
