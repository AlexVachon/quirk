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

    // ==========================================================
    // Helper: Convert Any Type to Boolean (i1)
    // ==========================================================
    Value* toBool(Value* v) {
        if (!v)
            return nullptr;
        // Already a boolean?
        if (v->getType()->isIntegerTy(1))
            return v;

        // Pointer -> Is Not Null?
        if (v->getType()->isPointerTy()) {
            return Builder.CreateICmpNE(v, Constant::getNullValue(v->getType()),
                                        "ptr_is_true");
        }
        // Double -> Is Not 0.0?
        if (v->getType()->isDoubleTy()) {
            return Builder.CreateFCmpONE(
                v, ConstantFP::get(Context, APFloat(0.0)), "dbl_is_true");
        }
        // Int -> Is Not 0?
        if (v->getType()->isIntegerTy()) {
            return Builder.CreateICmpNE(v, ConstantInt::get(v->getType(), 0),
                                        "int_is_true");
        }
        // Fallback
        return ConstantInt::getFalse(Context);
    }

    // ==========================================================
    // Logical Operators (not, and, or)
    // ==========================================================
    Value* generateNot(Value* val) {
        // 1. Normalize to boolean (i1)
        Value* boolVal = toBool(val);
        // 2. Invert
        return Builder.CreateNot(boolVal, "not_result");
    }

    Value* generateLogicOp(std::string op,
                           Value* leftVal,
                           Node* rightNode,
                           std::function<Value*(Node*)> exprHandler) {
        // 1. Normalize Left to Bool
        Value* L = toBool(leftVal);

        // 2. Evaluate Right
        // Note: For full short-circuiting, this needs BasicBlock management.
        // For now, we evaluate both but strictly strictly cast to bool.
        Value* rightVal = exprHandler(rightNode);
        if (!rightVal)
            return nullptr;

        // 3. Normalize Right to Bool
        Value* R = toBool(rightVal);

        if (op == "and")
            return Builder.CreateAnd(L, R, "and_result");
        if (op == "or")
            return Builder.CreateOr(L, R, "or_result");
        return nullptr;
    }

    // ==========================================================
    // Binary Operators (+, -, *, /, <, >, ==)
    // ==========================================================
    Value* generateBinaryOp(std::string op, Value* L, Value* R) {
        if (!L || !R)
            return nullptr;

        // Check if either side is a Double
        bool isDouble =
            L->getType()->isDoubleTy() || R->getType()->isDoubleTy();

        if (isDouble) {
            // FIX: Auto-Cast Int to Double if needed
            if (L->getType()->isIntegerTy())
                L = Builder.CreateSIToFP(L, Type::getDoubleTy(Context),
                                         "int_to_dbl");
            if (R->getType()->isIntegerTy())
                R = Builder.CreateSIToFP(R, Type::getDoubleTy(Context),
                                         "int_to_dbl");

            // Math
            if (op == "+")
                return Builder.CreateFAdd(L, R, "f_add");
            if (op == "-")
                return Builder.CreateFSub(L, R, "f_sub");
            if (op == "*")
                return Builder.CreateFMul(L, R, "f_mul");
            if (op == "/")
                return Builder.CreateFDiv(L, R, "f_div");

            // Comparisons (Returns i1)
            if (op == ">")
                return Builder.CreateFCmpOGT(L, R, "f_gt");
            if (op == "<")
                return Builder.CreateFCmpOLT(L, R, "f_lt");
            if (op == ">=")
                return Builder.CreateFCmpOGE(L, R, "f_ge");
            if (op == "<=")
                return Builder.CreateFCmpOLE(L, R, "f_le");
            if (op == "==")
                return Builder.CreateFCmpOEQ(L, R, "f_eq");
            if (op == "!=")
                return Builder.CreateFCmpONE(L, R, "f_ne");
        } else {
            // Integer Math
            // FIX: Ensure Bit Widths Match (e.g. i32 vs i8)
            if (L->getType() != R->getType()) {
                if (L->getType()->getIntegerBitWidth() <
                    R->getType()->getIntegerBitWidth())
                    L = Builder.CreateIntCast(L, R->getType(), true);
                else
                    R = Builder.CreateIntCast(R, L->getType(), true);
            }

            if (op == "+")
                return Builder.CreateAdd(L, R, "i_add");
            if (op == "-")
                return Builder.CreateSub(L, R, "i_sub");
            if (op == "*")
                return Builder.CreateMul(L, R, "i_mul");
            if (op == "/")
                return Builder.CreateSDiv(L, R, "i_div");

            // Comparisons
            if (op == ">")
                return Builder.CreateICmpSGT(L, R, "i_gt");
            if (op == "<")
                return Builder.CreateICmpSLT(L, R, "i_lt");
            if (op == ">=")
                return Builder.CreateICmpSGE(L, R, "i_ge");
            if (op == "<=")
                return Builder.CreateICmpSLE(L, R, "i_le");
            if (op == "==")
                return Builder.CreateICmpEQ(L, R, "i_eq");
            if (op == "!=")
                return Builder.CreateICmpNE(L, R, "i_ne");
        }
        return nullptr;
    }
};