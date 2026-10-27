#pragma once
// Int <-> untyped (i8*) value encoding — the workhorse of T-erased
// generic dispatch (List / Map elements, Any, Callable boundaries).
//
// Int is 64-bit. An Int that fits in 32 bits is stored inline in the
// pointer: the pointer value is its low 32 bits (<= 0xFFFFFFFF, which no
// object ever sits at). Any other Int is a headered heap Any box
// (`Core_Primitives_Any_box_int`). Int 0 is boxed too — inline it would be
// the null pointer, indistinguishable from "no value"; the runtime keeps
// one shared static zero box, so boxing 0 allocates nothing.
//
// Decoding reverses it: a pointer value <= 0xFFFFFFFF is an inline Int
// (sign-extended from 32 bits; null reads as 0), anything else goes to
// `quirk_opaque_to_int`, which reads the box. The runtime's
// quirk_int_to_opaque / quirk_opaque_to_int implement the same rules.

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include <cstdint>

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
    Type* i64 = Type::getInt64Ty(Context);
    Type* i32 = Type::getInt32Ty(Context);
    FunctionCallee boxIntFn = TheModule->getOrInsertFunction(
        "Core_Primitives_Any_box_int", FunctionType::get(i8PtrTy, {i64}, false));
    auto finalize = [&](Value* boxed) -> Value* {
        // Callers ask for an arbitrary pointer-typed slot (i8*, String*,
        // %Any*, ...); bitcast so the call-site IR types line up.
        if (expectedType && expectedType != boxed->getType()
            && expectedType->isPointerTy()) {
            return Builder.CreateBitCast(boxed, expectedType);
        }
        return boxed;
    };
    // Widen to Int's i64 (Bool zero-extends, narrower ints sign-extend).
    Value* v = val;
    if (v->getType()->isIntegerTy(1)) v = Builder.CreateZExt(v, i64);
    else if (v->getType() != i64) v = Builder.CreateSExtOrTrunc(v, i64);
    auto inlineOf = [&](Value* x) {
        return Builder.CreateIntToPtr(Builder.CreateZExt(Builder.CreateTrunc(x, i32), i64), i8PtrTy);
    };
    if (auto* ci = dyn_cast<ConstantInt>(v)) {
        int64_t n = ci->getSExtValue();
        if (n == 0 || n < INT32_MIN || n > INT32_MAX)
            return finalize(Builder.CreateCall(boxIntFn, {v}));
        return finalize(inlineOf(v));
    }
    Function* parentFn = Builder.GetInsertBlock()->getParent();
    BasicBlock* boxBB  = BasicBlock::Create(Context, "boxint_box",    parentFn);
    BasicBlock* tagBB  = BasicBlock::Create(Context, "boxint_inline", parentFn);
    BasicBlock* joinBB = BasicBlock::Create(Context, "boxint_join",   parentFn);
    Value* fits = Builder.CreateICmpEQ(Builder.CreateSExt(Builder.CreateTrunc(v, i32), i64), v);
    Value* nonZero = Builder.CreateICmpNE(v, ConstantInt::get(i64, 0));
    Builder.CreateCondBr(Builder.CreateAnd(fits, nonZero), tagBB, boxBB);
    Builder.SetInsertPoint(boxBB);
    Value* heapBoxed = Builder.CreateCall(boxIntFn, {v});
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(tagBB);
    Value* inlineBoxed = inlineOf(v);
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(joinBB);
    PHINode* phi = Builder.CreatePHI(i8PtrTy, 2, "boxint_phi");
    phi->addIncoming(heapBoxed, boxBB);
    phi->addIncoming(inlineBoxed, tagBB);
    return finalize(phi);
}

// An untyped value (any pointer type) as an Int (i64): the inverse of
// boxIntToOpaque. Inline values decode without a call.
inline llvm::Value* decodeUntypedInt(
    llvm::LLVMContext& Context,
    llvm::Module*      TheModule,
    llvm::IRBuilder<>& Builder,
    llvm::Value*       p
) {
    using namespace llvm;
    Type* i8PtrTy = Type::getInt8PtrTy(Context);
    Type* i64 = Type::getInt64Ty(Context);
    Type* i32 = Type::getInt32Ty(Context);
    if (p->getType() != i8PtrTy) p = Builder.CreateBitCast(p, i8PtrTy);
    if (isa<ConstantPointerNull>(p)) return ConstantInt::get(i64, 0);
    FunctionCallee toInt = TheModule->getOrInsertFunction(
        "quirk_opaque_to_int", FunctionType::get(i64, {i8PtrTy}, false));
    Value* u = Builder.CreatePtrToInt(p, i64);
    Function* parentFn = Builder.GetInsertBlock()->getParent();
    BasicBlock* inlBB  = BasicBlock::Create(Context, "unboxint_inline", parentFn);
    BasicBlock* boxBB  = BasicBlock::Create(Context, "unboxint_box",    parentFn);
    BasicBlock* joinBB = BasicBlock::Create(Context, "unboxint_join",   parentFn);
    Builder.CreateCondBr(Builder.CreateICmpULE(u, ConstantInt::get(i64, 0xFFFFFFFFULL)), inlBB, boxBB);
    Builder.SetInsertPoint(inlBB);
    Value* inl = Builder.CreateSExt(Builder.CreateTrunc(u, i32), i64);
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(boxBB);
    Value* boxed = Builder.CreateCall(toInt, {p});
    Builder.CreateBr(joinBB);
    Builder.SetInsertPoint(joinBB);
    PHINode* phi = Builder.CreatePHI(i64, 2, "unboxint");
    phi->addIncoming(inl, inlBB);
    phi->addIncoming(boxed, boxBB);
    return phi;
}

}  // namespace quirk
