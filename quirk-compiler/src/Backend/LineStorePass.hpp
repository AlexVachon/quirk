#pragma once
// Line-store sinking (compiler 7.14.0).
//
// Every statement stores its line into the function's shadow-frame slot
// (the pointer quirk_push_frame_l returns) so tracebacks can name the
// running line (7.9.2). The slot is only *read* by something the code
// calls — a callee's traceback, a runtime error, the debugger hook — so a
// store matters only if a call can run before the next store. This pass:
//
//   - deletes a store that the next statement's store overwrites before
//     any call (plain arithmetic, assignments, comparisons), and
//   - moves a store that is needed only on a cold path — the overflow /
//     division-by-zero trap blocks, which have a single predecessor — into
//     that path, so the hot path pays nothing.
//
// Calls that can't read the slot (frame push / pop, allocation, boxing,
// LLVM intrinsics) don't count. A crash with no call involved (a null
// field read) reports the last line that called something; exceptions and
// runtime errors keep their exact line.

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include <map>
#include <set>
#include <vector>

namespace quirk {

inline bool lineSlotObserver(const llvm::Instruction& I) {
    auto* call = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!call) return false;
    const llvm::Function* f = call->getCalledFunction();
    if (!f) return true;                       // indirect: a Callable, anything
    if (f->isIntrinsic()) return false;
    llvm::StringRef n = f->getName();
    return !(n == "quirk_pop_frame" || n == "quirk_push_frame_l" || n == "quirk_push_frame" ||
             n == "quirk_alloc" || n.startswith("GC_malloc") ||
             n.startswith("Core_Primitives_Any_box_"));
}

inline void sinkLineStores(llvm::Function& F) {
    using namespace llvm;
    std::vector<Value*> slots;
    for (auto& BB : F)
        for (auto& I : BB)
            if (auto* c = dyn_cast<CallInst>(&I))
                if (Function* cf = c->getCalledFunction(); cf && cf->getName() == "quirk_push_frame_l")
                    slots.push_back(c);
    for (Value* slot : slots) {
        auto isStore = [&](const Instruction& I) {
            auto* st = dyn_cast<StoreInst>(&I);
            return st && st->getPointerOperand() == slot;
        };
        // Backward dataflow: is the slot's current value read (by a call)
        // before it's overwritten, starting from the top of each block?
        std::map<const BasicBlock*, bool> needIn;
        for (auto& BB : F) needIn[&BB] = false;
        auto needOut = [&](const BasicBlock& BB) {
            for (const BasicBlock* s : successors(&BB)) if (needIn[s]) return true;
            return false;
        };
        for (bool changed = true; changed; ) {
            changed = false;
            for (const BasicBlock& BB : F) {   // order only affects how many rounds
                bool need = needOut(BB);
                for (auto i = BB.rbegin(); i != BB.rend(); ++i) {
                    if (isStore(*i)) need = false;
                    else if (lineSlotObserver(*i)) need = true;
                }
                if (need != needIn[&BB]) { needIn[&BB] = need; changed = true; }
            }
        }
        std::vector<StoreInst*> stores;
        for (auto& BB : F)
            for (auto& I : BB)
                if (isStore(I)) stores.push_back(cast<StoreInst>(&I));
        for (StoreInst* st : stores) {
            BasicBlock* BB = st->getParent();
            // What follows the store in its block: another store (dead), a
            // call (needed here), or the end of the block.
            bool decided = false, keep = false;
            for (Instruction* i = st->getNextNode(); i; i = i->getNextNode()) {
                if (isStore(*i)) { decided = true; keep = false; break; }
                if (lineSlotObserver(*i)) { decided = true; keep = true; break; }
            }
            if (decided) {
                if (!keep) st->eraseFromParent();
                continue;
            }
            std::vector<BasicBlock*> needing;
            bool sinkable = true;
            for (BasicBlock* s : successors(BB)) {
                if (!needIn[s]) continue;
                needing.push_back(s);
                if (s == BB || !s->getSinglePredecessor()) sinkable = false;
            }
            if (needing.empty()) { st->eraseFromParent(); continue; }
            if (!sinkable) continue;
            for (BasicBlock* s : needing)
                new StoreInst(st->getValueOperand(), slot, &*s->getFirstInsertionPt());
            st->eraseFromParent();
        }
    }
}

inline void sinkLineStores(llvm::Module& M) {
    for (auto& F : M)
        if (!F.isDeclaration()) sinkLineStores(F);
}

} // namespace quirk
