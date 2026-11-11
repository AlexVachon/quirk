#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "BoxInt.hpp"
#include "ast.hpp"
#include <functional>

using namespace llvm;

class MathGen {
    LLVMContext& Context;
    IRBuilder<>& Builder;
    Module* TheModule = nullptr;

   public:
    // Int (i64) `+` / `-` / `*` with overflow detection: the
    // llvm.s*.with.overflow intrinsics give the wrapped result plus a
    // flag; on overflow branch to a cold block that calls the runtime's
    // quirk_int_overflow (throws RuntimeError). Silent wrap-around used
    // to produce wrong answers (sum of 0..10M came out negative-ish).
    Value* emitCheckedInt(const std::string& op, Value* L, Value* R) {
        Module* mod = Builder.GetInsertBlock()->getModule();
        Intrinsic::ID id = op == "+" ? Intrinsic::sadd_with_overflow
                         : op == "-" ? Intrinsic::ssub_with_overflow
                                     : Intrinsic::smul_with_overflow;
        Function* intr = Intrinsic::getDeclaration(mod, id, {L->getType()});
        Value* pair = Builder.CreateCall(intr, {L, R}, "i_chk");
        Value* result = Builder.CreateExtractValue(pair, 0, op == "+" ? "i_add" : op == "-" ? "i_sub" : "i_mul");
        Value* overflow = Builder.CreateExtractValue(pair, 1, "i_ovf");

        Function* parent = Builder.GetInsertBlock()->getParent();
        BasicBlock* ovfBB = BasicBlock::Create(Context, "int_overflow", parent);
        BasicBlock* okBB  = BasicBlock::Create(Context, "int_ok", parent);
        MDBuilder md(Context);
        Builder.CreateCondBr(overflow, ovfBB, okBB, md.createBranchWeights(1, 1 << 20));

        Builder.SetInsertPoint(ovfBB);
        Type* i64 = Type::getInt64Ty(Context);
        FunctionCallee fail = mod->getOrInsertFunction("quirk_int_overflow",
            Type::getVoidTy(Context), Type::getInt8PtrTy(Context), i64, i64);
        Builder.CreateCall(fail, {Builder.CreateGlobalStringPtr(op),
                                  Builder.CreateSExtOrTrunc(L, i64), Builder.CreateSExtOrTrunc(R, i64)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(okBB);
        return result;
    }

    MathGen(LLVMContext& ctx, IRBuilder<>& build)
        : Context(ctx), Builder(build) {}

    void setModule(Module* m) { TheModule = m; }

    Value* toBool(Value* v) {
        if (!v) return nullptr;
        if (v->getType()->isIntegerTy(1)) return v;
        if (v->getType()->isPointerTy())
            return Builder.CreateICmpNE(v, Constant::getNullValue(v->getType()), "ptr_is_true");
        if (v->getType()->isDoubleTy())
            return Builder.CreateFCmpONE(v, ConstantFP::get(Context, APFloat(0.0)), "dbl_is_true");
        if (v->getType()->isIntegerTy())
            return Builder.CreateICmpNE(v, ConstantInt::get(v->getType(), 0), "int_is_true");
        return ConstantInt::getFalse(Context);
    }

    Value* generateNot(Value* val) {
        return Builder.CreateNot(toBool(val), "not_result");
    }

    // `a and b` / `a or b`, short-circuiting: `b` runs only when `a`
    // doesn't decide the result. Both sides used to be evaluated (an i1
    // `and`), so `x != null and x.f()` dereferenced the null.
    Value* generateLogicOp(std::string op, Value* leftVal, Node* rightNode,
                           std::function<Value*(Node*)> exprHandler) {
        if (op != "and" && op != "or") return nullptr;
        if (!leftVal) return nullptr;
        Value* L = toBool(leftVal);
        bool isAnd = op == "and";
        Function* fn = Builder.GetInsertBlock()->getParent();
        BasicBlock* lhsEnd = Builder.GetInsertBlock();
        BasicBlock* rhsBB = BasicBlock::Create(Context, isAnd ? "and_rhs" : "or_rhs", fn);
        BasicBlock* endBB = BasicBlock::Create(Context, isAnd ? "and_end" : "or_end", fn);
        if (isAnd) Builder.CreateCondBr(L, rhsBB, endBB);
        else       Builder.CreateCondBr(L, endBB, rhsBB);

        Builder.SetInsertPoint(rhsBB);
        Value* rightVal = exprHandler(rightNode);
        Value* R = rightVal ? toBool(rightVal) : ConstantInt::getFalse(Context);
        BasicBlock* rhsEnd = Builder.GetInsertBlock();
        bool rhsFallsThrough = !rhsEnd->getTerminator();
        if (rhsFallsThrough) Builder.CreateBr(endBB);

        Builder.SetInsertPoint(endBB);
        PHINode* phi = Builder.CreatePHI(Type::getInt1Ty(Context), 2, isAnd ? "and_result" : "or_result");
        phi->addIncoming(isAnd ? ConstantInt::getFalse(Context) : ConstantInt::getTrue(Context), lhsEnd);
        if (rhsFallsThrough) phi->addIncoming(R, rhsEnd);
        return phi;
    }

    Value* generateBinaryOp(std::string op, Value* L, Value* R) {
        if (!L || !R) return nullptr;

        // Unbox i8* for arithmetic / numeric comparisons. Two encodings
        // can show up:
        //   - Legacy tagged int (low 32 bits = value) — e.g. `for x in
        //     list` elements, list[i] reads of Int.
        //   - Any* heap-box — used by nonlocal cells so Int 0 / Bool
        //     false aren't NULL.
        // `quirk_opaque_to_int` handles both, so we route through it
        // when we have a Module pointer; the raw PtrToInt fallback
        // covers the early-init path where setModule hasn't fired.
        bool isNumericOp = (op=="+"||op=="-"||op=="*"||op=="/"||op=="%"||op==">"||op=="<"||op==">="||op=="<=");
        bool isEqOp     = (op == "==" || op == "!=");
        // `==` / `!=` on two collections (List / Map / Set / Tuple) or two
        // untyped values compares contents (quirk_opaque_eq — strings and
        // numbers by value, collections recursively, other objects by
        // identity). It compared pointers, so `[1, 2] == [1, 2]` was false.
        if (TheModule && isEqOp && L->getType()->isPointerTy() && L->getType() == R->getType()
            && !isa<ConstantPointerNull>(L) && !isa<ConstantPointerNull>(R)) {
            Type* el = L->getType()->getPointerElementType();
            bool untyped = el->isIntegerTy(8);
            bool coll = false;
            if (auto* st = dyn_cast<StructType>(el); st && st->hasName()) {
                std::string n = st->getName().str();
                coll = (n == "List" || n == "Map" || n == "Set" || n == "Tuple");
            }
            if (untyped || coll) {
                Type* i8p = Type::getInt8PtrTy(Context);
                FunctionCallee eq = TheModule->getOrInsertFunction("quirk_opaque_eq",
                    FunctionType::get(Type::getInt32Ty(Context), {i8p, i8p}, false));
                Value* r = Builder.CreateCall(eq, {Builder.CreateBitCast(L, i8p), Builder.CreateBitCast(R, i8p)}, "val_eq");
                Value* zero = ConstantInt::get(Type::getInt32Ty(Context), 0);
                return op == "==" ? Builder.CreateICmpNE(r, zero, "eq") : Builder.CreateICmpEQ(r, zero, "ne");
            }
        }
        // Both sides untyped (List elements, Map values, a reduce
        // accumulator): what to do depends on the values, so the runtime
        // decides — String concat, Double math or Int math (see
        // quirk_opaque_binop). Decoding both as Ints made
        // [1.5, 2.5].sum() 3 and "a" + "b" from a List 0.
        {
            Type* i8p = Type::getInt8PtrTy(Context);
            if (TheModule && L->getType() == i8p && R->getType() == i8p && isNumericOp) {
                static const std::map<std::string, int> arith = {{"+", 0}, {"-", 1}, {"*", 2}, {"/", 3}, {"%", 4}};
                if (auto it = arith.find(op); it != arith.end()) {
                    FunctionCallee binop = TheModule->getOrInsertFunction("quirk_opaque_binop",
                        FunctionType::get(i8p, {Type::getInt32Ty(Context), i8p, i8p}, false));
                    return Builder.CreateCall(binop, {ConstantInt::get(Type::getInt32Ty(Context), it->second), L, R}, "dyn_op");
                }
                FunctionCallee cmp = TheModule->getOrInsertFunction("quirk_opaque_cmp",
                    FunctionType::get(Type::getInt32Ty(Context), {i8p, i8p}, false));
                Value* c = Builder.CreateCall(cmp, {L, R}, "dyn_cmp");
                Value* zero = ConstantInt::get(Type::getInt32Ty(Context), 0);
                if (op == ">")  return Builder.CreateICmpSGT(c, zero, "dyn_gt");
                if (op == "<")  return Builder.CreateICmpSLT(c, zero, "dyn_lt");
                if (op == ">=") return Builder.CreateICmpSGE(c, zero, "dyn_ge");
                return Builder.CreateICmpSLE(c, zero, "dyn_le");
            }
        }
        {
            Type* i8p = Type::getInt8PtrTy(Context);
            bool Lptr = L->getType() == i8p;
            bool Rptr = R->getType() == i8p;
            bool Lnum = L->getType()->isIntegerTy() || L->getType()->isDoubleTy();
            bool Rnum = R->getType()->isIntegerTy() || R->getType()->isDoubleTy();
            // Paired with a Double, the untyped side is decoded as a Double:
            // `total = total + x` (total: Double, x a List element) read x
            // as an Int, so 1.5 + 2.25 summed to 3.0.
            auto unbox = [&](Value* v, const char* label, Value* other) -> Value* {
                if (!TheModule) return Builder.CreatePtrToInt(v, Type::getInt64Ty(Context), label);
                if (other->getType()->isDoubleTy()) {
                    FunctionCallee toD = TheModule->getOrInsertFunction("quirk_opaque_to_double",
                        FunctionType::get(Type::getDoubleTy(Context), {i8p}, false));
                    return Builder.CreateCall(toD, {v}, label);
                }
                return quirk::decodeUntypedInt(Context, TheModule, Builder, v);
            };
            if (isNumericOp) {
                // Arithmetic / ordered comparison: unbox any i8* side that
                // pairs with a number-or-pointer.
                if (Lptr && (Rnum || Rptr)) L = unbox(L, "unbox_l", R);
                if (Rptr && (Lnum || Lptr)) R = unbox(R, "unbox_r", L);
            } else if (isEqOp) {
                // `==` / `!=` against a literal int (or a known Int local)
                // must unbox the i8* side or LLVM ICmp asserts on type
                // mismatch. Don't touch i8*-vs-i8* — that's a legit
                // pointer-equality compare (struct identity, etc.).
                if (Lptr && Rnum) L = unbox(L, "unbox_eq_l", R);
                if (Rptr && Lnum) R = unbox(R, "unbox_eq_r", L);
            }
        }

        bool isDouble = L->getType()->isDoubleTy() || R->getType()->isDoubleTy();

        if (isDouble) {
            if (L->getType()->isIntegerTy()) L = Builder.CreateSIToFP(L, Type::getDoubleTy(Context), "int_to_dbl");
            if (R->getType()->isIntegerTy()) R = Builder.CreateSIToFP(R, Type::getDoubleTy(Context), "int_to_dbl");
            if (op == "+")  return Builder.CreateFAdd(L, R, "f_add");
            if (op == "-")  return Builder.CreateFSub(L, R, "f_sub");
            if (op == "*")  return Builder.CreateFMul(L, R, "f_mul");
            if (op == "/")  return Builder.CreateFDiv(L, R, "f_div");
            if (op == "%")  return Builder.CreateFRem(L, R, "f_mod");
            if (op == ">")  return Builder.CreateFCmpOGT(L, R, "f_gt");
            if (op == "<")  return Builder.CreateFCmpOLT(L, R, "f_lt");
            if (op == ">=") return Builder.CreateFCmpOGE(L, R, "f_ge");
            if (op == "<=") return Builder.CreateFCmpOLE(L, R, "f_le");
            if (op == "==") return Builder.CreateFCmpOEQ(L, R, "f_eq");
            if (op == "!=") return Builder.CreateFCmpONE(L, R, "f_ne");
        } else {
            if (L->getType() != R->getType() &&
                L->getType()->isIntegerTy() && R->getType()->isIntegerTy()) {
                if (L->getType()->getIntegerBitWidth() < R->getType()->getIntegerBitWidth())
                    L = Builder.CreateIntCast(L, R->getType(), true);
                else
                    R = Builder.CreateIntCast(R, L->getType(), true);
            }
            if (op == "+" || op == "-" || op == "*") {
                if (L->getType()->isIntegerTy(64) || L->getType()->isIntegerTy(32)) return emitCheckedInt(op, L, R);
                if (op == "+")  return Builder.CreateAdd(L, R, "i_add");
                if (op == "-")  return Builder.CreateSub(L, R, "i_sub");
                return Builder.CreateMul(L, R, "i_mul");
            }
            if (op == "/" || op == "%") {
                // LLVM's `sdiv` / `srem` against 0 are *undefined
                // behavior* — at -O2 the optimizer assumes the divisor
                // is non-zero and the result is arbitrary bits. Emit
                // a runtime guard before the divide: if R == 0, throw
                // ZeroDivisionError; otherwise proceed.
                Function* parent = Builder.GetInsertBlock()->getParent();
                Module* mod      = Builder.GetInsertBlock()->getModule();
                Value* isZero = Builder.CreateICmpEQ(R, ConstantInt::get(R->getType(), 0), "div_zero_chk");
                BasicBlock* throwBB = BasicBlock::Create(Context, "div_by_zero", parent);
                BasicBlock* okBB    = BasicBlock::Create(Context, "div_ok",      parent);
                Builder.CreateCondBr(isZero, throwBB, okBB);

                Builder.SetInsertPoint(throwBB);
                FunctionCallee thrower = mod->getOrInsertFunction(
                    "quirk_throw_exception",
                    Type::getVoidTy(Context),
                    Type::getInt8PtrTy(Context),
                    Type::getInt8PtrTy(Context));
                const char* msg = (op == "/") ? "integer division by zero"
                                              : "integer modulo by zero";
                Builder.CreateCall(thrower, {
                    Builder.CreateGlobalStringPtr("ZeroDivisionError"),
                    Builder.CreateGlobalStringPtr(msg)
                });
                Builder.CreateUnreachable();

                Builder.SetInsertPoint(okBB);
                if (op == "/") return Builder.CreateSDiv(L, R, "i_div");
                return Builder.CreateSRem(L, R, "i_mod");
            }
            if (op == ">")  return Builder.CreateICmpSGT(L, R, "i_gt");
            if (op == "<")  return Builder.CreateICmpSLT(L, R, "i_lt");
            if (op == ">=") return Builder.CreateICmpSGE(L, R, "i_ge");
            if (op == "<=") return Builder.CreateICmpSLE(L, R, "i_le");
            if (op == "==") return Builder.CreateICmpEQ(L, R, "i_eq");
            if (op == "!=") return Builder.CreateICmpNE(L, R, "i_ne");
        }
        return nullptr;
    }
};
