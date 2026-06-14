#pragma once
// Shared helper for boxing an integer Value into an opaque (i8*)
// destination — the workhorse of T-erased generic dispatch.
//
// The fast path inline-tags the int directly into the pointer
// (`IntToPtr`), which works for any non-zero value because LLVM
// preserves the bit pattern. But `IntToPtr(0)` produces a null
// pointer, indistinguishable from "no value" — Quirk's runtime
// then renders the boxed value as "null" instead of "0".
//
// This bit users hard once Option[Int].unwrap_or(0) reached the
// canonical sum-type API in typing v1.2.0 — the fallback default
// of 0 was the natural choice, and it silently became null.
//
// Resolution: for Int 0 (statically or dynamically), divert to the
// runtime helper `Core_Primitives_Any_box_int` which heap-allocates
// a real Any* with tag=ANY_INT, ival=0. Decoders that follow heap
// Any tags (quirk_opaque_to_int, quirk_opaque_to_string, match
// scrutinee bridging) read the value correctly. Decoders that
// PtrToInt directly are already correct for the non-zero path and
// don't see zero here.
//
// Static-zero is a constant-fold; dynamic-zero needs a runtime
// branch. The branch is one icmp + one cond_br on a path that's
// only hit at boxing boundaries (function-call arg coercion,
// lambda env/return), not inner loops — perf impact is negligible
// in measured stdlib runs.

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"

namespace quirk {

inline llvm::Value* boxIntToOpaque(
    llvm::LLVMContext& Context,
    llvm::Module*      TheModule,
    llvm::IRBuilder<>& Builder,
    llvm::Value*       val,
    llvm::Type*        expectedType
) {
    using namespace llvm;
    Type* i8PtrTy = Type::getInt8PtrTy(Context);
    // Look up (or declare) the heap-alloc helper.
    Function* boxIntFn = TheModule->getFunction("Core_Primitives_Any_box_int");
    if (!boxIntFn) {
        FunctionType* ft = FunctionType::get(
            i8PtrTy,
            {Type::getInt32Ty(Context)}, false);
        boxIntFn = Function::Create(ft, Function::ExternalLinkage,
                                    "Core_Primitives_Any_box_int", TheModule);
    }
    auto finalize = [&](Value* boxed) -> Value* {
        // Callers ask for an arbitrary pointer-typed slot (i8*,
        // String*, %Any*, etc.). Heap-alloc returns i8*; the
        // inline-tag fast path also returns i8*. Bitcast to the
        // expected shape so the call-site IR types line up. A
        // shape-mismatched bitcast here is no worse than what the
        // old `IntToPtr(val, expectedType)` was doing — the old
        // code also produced an i8*-typed payload disguised under
        // a String* (or whatever) header.
        if (expectedType && expectedType != boxed->getType()
            && expectedType->isPointerTy()) {
            return Builder.CreateBitCast(boxed, expectedType);
        }
        return boxed;
    };
    Value* asI32 = val->getType()->isIntegerTy(32)
        ? val
        : Builder.CreateIntCast(val, Type::getInt32Ty(Context), true);
    if (auto* ci = dyn_cast<ConstantInt>(asI32)) {
        if (ci->isZero())
            return finalize(Builder.CreateCall(boxIntFn, {asI32}));
        return finalize(Builder.CreateIntToPtr(val, i8PtrTy));
    }
    Function* parentFn = Builder.GetInsertBlock()->getParent();
    BasicBlock* zeroBB = BasicBlock::Create(Context, "boxint_zero", parentFn);
    BasicBlock* tagBB  = BasicBlock::Create(Context, "boxint_tag",  parentFn);
    BasicBlock* joinBB = BasicBlock::Create(Context, "boxint_join", parentFn);
    Value* isZero = Builder.CreateICmpEQ(asI32, ConstantInt::get(asI32->getType(), 0));
    Builder.CreateCondBr(isZero, zeroBB, tagBB);
    Builder.SetInsertPoint(zeroBB);
    Value* heapBoxed = Builder.CreateCall(boxIntFn, {asI32});
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(tagBB);
    Value* inlineBoxed = Builder.CreateIntToPtr(val, i8PtrTy);
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(joinBB);
    PHINode* phi = Builder.CreatePHI(i8PtrTy, 2, "boxint_phi");
    phi->addIncoming(heapBoxed, zeroBB);
    phi->addIncoming(inlineBoxed, tagBB);
    return finalize(phi);
}

}  // namespace quirk
