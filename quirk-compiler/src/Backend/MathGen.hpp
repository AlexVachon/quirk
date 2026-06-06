#pragma once
#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>

using namespace llvm;

class MathGen {
    LLVMContext& Context;
    IRBuilder<>& Builder;

   public:
    MathGen(LLVMContext& ctx, IRBuilder<>& build)
        : Context(ctx), Builder(build) {}

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

    Value* generateLogicOp(std::string op, Value* leftVal, Node* rightNode,
                           std::function<Value*(Node*)> exprHandler) {
        Value* L = toBool(leftVal);
        Value* rightVal = exprHandler(rightNode);
        if (!rightVal) return nullptr;
        Value* R = toBool(rightVal);
        if (op == "and") return Builder.CreateAnd(L, R, "and_result");
        if (op == "or")  return Builder.CreateOr(L, R, "or_result");
        return nullptr;
    }

    Value* generateBinaryOp(std::string op, Value* L, Value* R) {
        if (!L || !R) return nullptr;

        // Unbox i8* (Any-boxed integer/double) for arithmetic and numeric comparisons.
        // This handles `for x in list` / list comprehension variables that arrive as void*.
        bool isNumericOp = (op=="+"||op=="-"||op=="*"||op=="/"||op==">"||op=="<"||op==">="||op=="<=");
        if (isNumericOp) {
            Type* i8p = Type::getInt8PtrTy(Context);
            bool Lptr = L->getType() == i8p;
            bool Rptr = R->getType() == i8p;
            bool Lnum = L->getType()->isIntegerTy() || L->getType()->isDoubleTy();
            bool Rnum = R->getType()->isIntegerTy() || R->getType()->isDoubleTy();
            if (Lptr && (Rnum || Rptr)) L = Builder.CreatePtrToInt(L, Type::getInt32Ty(Context), "unbox_l");
            if (Rptr && (Lnum || Lptr)) R = Builder.CreatePtrToInt(R, Type::getInt32Ty(Context), "unbox_r");
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
            if (op == "+")  return Builder.CreateAdd(L, R, "i_add");
            if (op == "-")  return Builder.CreateSub(L, R, "i_sub");
            if (op == "*")  return Builder.CreateMul(L, R, "i_mul");
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
