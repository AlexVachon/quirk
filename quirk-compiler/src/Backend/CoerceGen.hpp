#pragma once
//
// CoerceGen — the single canonical `Value* → Type*` coercion helper.
//
// Before v5.3.4 the same 8-case switch was open-coded in ~6 different
// Codegen sites:
//
//   StructGen.hpp: __init arg coercion       (lines 214-283)
//   StructGen.hpp: no-__init field GEP path  (lines 345-393)
//   Codegen.cpp:   call-arg coercion         (~2410, ~2541-2583)
//   Codegen.cpp:   retval coercion           (~2815-2920)
//   Codegen.cpp:   magic-method dispatch RHS (~4248-4283)
//
// Each covered slightly different subsets and drifted over time — the
// `String * Any` fuzz crash (v5.3.0), the `Post(stem, ...)` garbage-
// byte bug (v5.3.3), and the `Any.trim()` → void classification
// (v5.3.2) all traced back to one branch missing that another had.
//
// This helper collapses everything into one entry point. Any future
// Codegen site that needs to bridge a type mismatch imports this
// header and calls `coerceToType(val, expected, ctx)`. Bug fixes to
// coercion land in ONE place.
//
// Cases handled, in order:
//
//   ty == expected                     →  val (no-op)
//   int → int (width mismatch)         →  CreateIntCast(signed)
//   int → double                       →  CreateSIToFP
//   double → int                       →  CreateFPToSI
//   double → i8*                       →  Core_Primitives_Any_box_double
//   int  → any ptr                     →  boxIntToOpaque
//   i8*  → int                         →  quirk_opaque_to_int
//   i8*  → String*                     →  quirk_opaque_to_string
//   struct-ptr → struct-ptr            →  CreateBitCast  (same struct)
//   ptr → ptr (compatible)             →  CreateBitCast
//
// Unhandled shapes return `val` unchanged (the caller is responsible
// for verifying — the emitted IR fails verifier if the caller was
// wrong). This is deliberate: the helper is a best-effort bridge, not
// a Sema replacement.

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "BoxInt.hpp"

using namespace llvm;

namespace quirk {

inline Value* coerceToType(Value* val, Type* expected,
                           LLVMContext& ctx, IRBuilder<>& b,
                           Module* mod)
{
    if (!val || !expected) return val;
    Type* got = val->getType();
    if (got == expected) return val;

    Type* i8p = Type::getInt8PtrTy(ctx);

    // int → int (different widths).
    if (got->isIntegerTy() && expected->isIntegerTy()) {
        return b.CreateIntCast(val, expected, /*isSigned=*/true, "coerce_i2i");
    }

    // int → double.
    if (got->isIntegerTy() && expected->isDoubleTy()) {
        return b.CreateSIToFP(val, expected, "coerce_i2d");
    }

    // double → int.
    if (got->isDoubleTy() && expected->isIntegerTy()) {
        return b.CreateFPToSI(val, expected, "coerce_d2i");
    }

    // double → i8* (Any-typed param). Box through the runtime helper
    // so the value carries a real ANY_DOUBLE tag; a raw bitcast would
    // stuff the double's bit pattern into a pseudo-pointer.
    if (got->isDoubleTy() && expected == i8p) {
        FunctionCallee box = mod->getOrInsertFunction(
            "Core_Primitives_Any_box_double",
            Type::getInt8PtrTy(ctx), Type::getDoubleTy(ctx));
        return b.CreateCall(box, {val}, "coerce_d2any");
    }

    // int → any pointer (Any / struct*). Legacy tagged-int shape.
    if (got->isIntegerTy() && expected->isPointerTy()) {
        return boxIntToOpaque(ctx, mod, b, val, expected);
    }

    // Pointer → pointer: handle several sub-cases.
    if (got->isPointerTy() && expected->isPointerTy()) {
        bool gotIsI8p = got == i8p;
        bool expIsI8p = expected == i8p;

        // i8* → String*: unbox via the runtime helper. Handles all
        // three sources cleanly:
        //   - raw c-string → wraps into a fresh String
        //   - Any-boxed String* → returns the underlying String*
        //   - Any-boxed Int/Double → freshly stringified
        //
        // (The old inline "wrap i8* as a new String's _buffer field"
        // pattern produced garbage bytes when the i8* was actually a
        // boxed struct pointer — that's the v5.3.3 fuzz path.)
        if (gotIsI8p && expected->getPointerElementType()->isStructTy()) {
            Type* elt = expected->getPointerElementType();
            if (auto* st = dyn_cast<StructType>(elt)) {
                StringRef nm = st->getName();
                if (nm.contains("String") && !nm.contains("Iterator")) {
                    FunctionCallee toStr = mod->getOrInsertFunction(
                        "quirk_opaque_to_string", expected, i8p);
                    return b.CreateCall(toStr, {val}, "coerce_any2str");
                }
            }
        }
        // Otherwise a plain bitcast covers the remaining pointer-
        // shape mismatches — same-struct-different-name, opaque
        // Any↔specific-struct, etc.
        (void)gotIsI8p; (void)expIsI8p;
        return b.CreateBitCast(val, expected, "coerce_ptr");
    }

    // i8* → int (also matches pointer→int when types weren't caught
    // above). Route through quirk_opaque_to_int so both the tagged-
    // int encoding and the heap-Any encoding resolve correctly.
    if (got->isPointerTy() && expected->isIntegerTy()) {
        FunctionCallee toInt = mod->getOrInsertFunction(
            "quirk_opaque_to_int", Type::getInt32Ty(ctx), i8p);
        Value* casted = (got == i8p) ? val : b.CreateBitCast(val, i8p);
        Value* asI32 = b.CreateCall(toInt, {casted}, "coerce_any2i");
        if (expected->isIntegerTy(32)) return asI32;
        return b.CreateIntCast(asI32, expected, /*isSigned=*/true, "coerce_i2i_narrow");
    }

    // i8* → double. Same Any-vs-tagged-int concern as the int
    // path — a lambda that captures a nonlocal Double reads back
    // as an Any-heap-box pointer, not a tagged double bit pattern.
    // The runtime helper handles both encodings.
    if (got->isPointerTy() && expected->isDoubleTy()) {
        FunctionCallee toDbl = mod->getOrInsertFunction(
            "quirk_opaque_to_double", Type::getDoubleTy(ctx), i8p);
        Value* casted = (got == i8p) ? val : b.CreateBitCast(val, i8p);
        return b.CreateCall(toDbl, {casted}, "coerce_any2dbl");
    }

    // Fall through — no known coercion. Return val as-is; the LLVM
    // verifier will yell if this actually reaches the emitted IR.
    return val;
}

} // namespace quirk
