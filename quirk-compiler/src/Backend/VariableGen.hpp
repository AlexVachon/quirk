#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <map>
#include <set>
#include <iostream>

using namespace llvm;

class VariableGen {
    LLVMContext& Context;
    IRBuilder<>& Builder;
    // The active LLVM Module. Used by emitDebugRegister and by the
    // nonlocal-cell box helper (which calls into Core_Primitives_Any_*
    // box functions). Set via setDebugMode (legacy name; sets the
    // module unconditionally, regardless of `debugMode`).
    bool debugMode = false;
    Module* TheModule = nullptr;

    // Emit Debug_register_local(name, &slot, type_tag). type_tag matches
    // the enum in libs/debug.c — small int, see DBG_TAG_* there.
    void emitDebugRegister(const std::string& name, Value* slot) {
        if (!debugMode || !TheModule) return;
        if (!slot || !slot->getType()->isPointerTy()) return;
        Type* elTy = slot->getType()->getPointerElementType();
        int tag = 5;  // 5 = OPAQUE / unknown — printed via quirk_opaque_to_string
        if (elTy->isIntegerTy(1))        tag = 4;  // BOOL
        else if (elTy->isIntegerTy(32))  tag = 0;  // INT
        else if (elTy->isIntegerTy(64))  tag = 1;  // INT64
        else if (elTy->isDoubleTy())     tag = 2;  // DOUBLE
        else if (elTy->isPointerTy())    tag = 3;  // POINTER (String*/Any*/etc.)
        FunctionCallee reg = TheModule->getOrInsertFunction(
            "Debug_register_local",
            Type::getVoidTy(Context),
            Type::getInt8PtrTy(Context),  // name
            Type::getInt8PtrTy(Context),  // address
            Type::getInt32Ty(Context));   // type_tag
        Value* nameStr = Builder.CreateGlobalStringPtr(name);
        Value* slotI8  = Builder.CreateBitCast(slot, Type::getInt8PtrTy(Context));
        Builder.CreateCall(reg,
            {nameStr, slotI8, ConstantInt::get(Type::getInt32Ty(Context), tag)});
    }

    // The Symbol Table: Maps variable name -> Stack Slot (AllocaInst*)
    std::map<std::string, Value*> NamedValues;

    // Variables captured as mutable heap cells for nonlocal/closure sharing.
    // Maps name -> the alloca slot that stores the i8* GC cell pointer.
    // The cell itself is a GC_malloc(8) block holding one i8* value.
    std::set<std::string> nonlocalVars;

    // Module-level mutable state. Survives `clear()` between functions so
    // every function in the module can see the same storage. Maps name →
    // the LLVM GlobalVariable, which is treated like a pointer-to-slot
    // analogous to the per-function allocas in NamedValues.
    std::map<std::string, GlobalVariable*> globalVars;

   public:
    VariableGen(LLVMContext& ctx, IRBuilder<>& build)
        : Context(ctx), Builder(build) {}

    void setDebugMode(bool d, Module* m) { debugMode = d; TheModule = m; }

    // Box a value for storage in a nonlocal heap cell. Pointers go in
    // as-is (after a bitcast to i8*). Integers/Doubles/Bools allocate
    // a `Core_Primitives_Any_box_*` wrapper so the cell pointer is
    // never NULL — without this, `Int 0` and `Bool false` would round-
    // trip through the cell as the null pointer and `quirk_opaque_to_
    // string` would stringify them as "null". The companion unboxers
    // are MathGen's arithmetic auto-unbox and Codegen's call-arg
    // auto-unbox; both now route through `quirk_opaque_to_int`/
    // `quirk_opaque_to_double` which transparently handle both the
    // Any* and the legacy tagged-int encoding.
    Value* boxForNonlocalCell(Value* val) {
        Type* i8p = Type::getInt8PtrTy(Context);
        if (!val || val->getType()->isVoidTy())
            return ConstantPointerNull::get(cast<PointerType>(i8p));
        if (val->getType()->isPointerTy())
            return Builder.CreateBitCast(val, i8p);
        if (!TheModule) {
            // Module not yet bound — fall back to the lossy encoding.
            // Should not happen in practice; setDebugMode is called
            // immediately after construction.
            if (val->getType()->isIntegerTy())
                return Builder.CreateIntToPtr(val, i8p);
            if (val->getType()->isDoubleTy()) {
                Value* asInt = Builder.CreateBitCast(val, Type::getInt64Ty(Context));
                return Builder.CreateIntToPtr(asInt, i8p);
            }
            return Builder.CreateBitCast(val, i8p);
        }
        Type* i32 = Type::getInt32Ty(Context);
        if (val->getType()->isIntegerTy(1)) {
            FunctionCallee boxBool = TheModule->getOrInsertFunction(
                "Core_Primitives_Any_box_bool", i8p, i32);
            Value* ext = Builder.CreateZExt(val, i32);
            return Builder.CreateCall(boxBool, {ext});
        }
        if (val->getType()->isIntegerTy()) {
            FunctionCallee boxInt = TheModule->getOrInsertFunction(
                "Core_Primitives_Any_box_int", i8p, i32);
            Value* w = val->getType()->getIntegerBitWidth() == 32
                ? val
                : Builder.CreateIntCast(val, i32, /*isSigned=*/true);
            return Builder.CreateCall(boxInt, {w});
        }
        if (val->getType()->isDoubleTy()) {
            FunctionCallee boxDbl = TheModule->getOrInsertFunction(
                "Core_Primitives_Any_box_double", i8p, Type::getDoubleTy(Context));
            return Builder.CreateCall(boxDbl, {val});
        }
        return Builder.CreateBitCast(val, i8p);
    }

    void clear() {
        NamedValues.clear();
        nonlocalVars.clear();
        // globalVars intentionally preserved — it's module-scoped state.
    }

    // Register a top-level variable backed by an LLVM GlobalVariable.
    // Reads/writes from any function will route through this storage.
    void defineGlobal(const std::string& name, GlobalVariable* gv) {
        globalVars[name] = gv;
    }
    bool isGlobal(const std::string& name) const {
        return globalVars.count(name) > 0;
    }
    GlobalVariable* getGlobal(const std::string& name) const {
        auto it = globalVars.find(name);
        return it == globalVars.end() ? nullptr : it->second;
    }

    // Register a function argument (e.g., 'self', 'x')
    void defineArgument(const std::string& name, Value* val) {
        Function* TheFunction = Builder.GetInsertBlock()->getParent();
        IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
        Value* slot = TmpB.CreateAlloca(val->getType(), nullptr, name);
        Builder.CreateStore(val, slot);
        NamedValues[name] = slot;
        emitDebugRegister(name, slot);
    }

    // Handle 'x := 10'
    void defineLocalVariable(const std::string& name, Value* val) {
        if (NamedValues.count(name)) {
            Value* existingSlot = NamedValues[name];
            if (existingSlot->getType()->getPointerElementType() == val->getType()) {
                Builder.CreateStore(val, existingSlot);
                return;
            }
        }

        Function* TheFunction = Builder.GetInsertBlock()->getParent();
        IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());

        Value* slot = TmpB.CreateAlloca(val->getType(), nullptr, name);
        Builder.CreateStore(val, slot);
        NamedValues[name] = slot;
        emitDebugRegister(name, slot);
    }

    // Handle 'x = 20' (Reassignment)
    void updateLocalVariable(const std::string& name, Value* val) {
        // Module-level global: write through the GlobalVariable. Check
        // before locals because a top-level name may have been re-bound in
        // an inner scope as a Callable (lambdas in main do this); we want
        // assignments to flow back to the canonical module slot.
        if (globalVars.count(name)) {
            GlobalVariable* gv = globalVars[name];
            Type* gvTy = gv->getValueType();
            if (val->getType() != gvTy) {
                if (val->getType()->isPointerTy() && gvTy->isPointerTy()) {
                    val = Builder.CreateBitCast(val, gvTy, name + "_gcast");
                }
            }
            Builder.CreateStore(val, gv);
            return;
        }

        // Nonlocal: store through the heap cell
        if (nonlocalVars.count(name)) {
            Value* slot = NamedValues[name];
            // slot stores an i8* (the cell address)
            Value* cellPtr = Builder.CreateLoad(Type::getInt8PtrTy(Context), slot, name + "_cell_ptr");
            // cast cell pointer to i8** so we can store an i8* through it
            Value* cellI8pp = Builder.CreateBitCast(cellPtr,
                PointerType::getUnqual(Type::getInt8PtrTy(Context)), name + "_cell_i8pp");
            Value* boxed = boxForNonlocalCell(val);
            Builder.CreateStore(boxed, cellI8pp);
            return;
        }

        if (NamedValues.find(name) == NamedValues.end()) {
            defineLocalVariable(name, val);
            return;
        }
        Value* slot = NamedValues[name];
        Builder.CreateStore(val, slot);
    }

    Value* resolveVariable(const std::string& name) {
        // Locals win over globals — a function parameter named `x` shadows
        // a module global `x`. Same precedence as most languages.
        if (NamedValues.find(name) == NamedValues.end()) {
            // Fall back to the module-level slot if there's one.
            auto git = globalVars.find(name);
            if (git != globalVars.end()) {
                return Builder.CreateLoad(git->second->getValueType(), git->second, name.c_str());
            }
            return nullptr;
        }

        Value* slot = NamedValues[name];

        // Nonlocal: double-load through cell pointer
        if (nonlocalVars.count(name)) {
            // slot stores i8* (the cell address)
            Value* cellPtr = Builder.CreateLoad(Type::getInt8PtrTy(Context), slot, name + "_cell_ptr");
            // cell is an i8* block; reinterpret as i8** to load the stored i8*
            Value* cellI8pp = Builder.CreateBitCast(cellPtr,
                PointerType::getUnqual(Type::getInt8PtrTy(Context)));
            return Builder.CreateLoad(Type::getInt8PtrTy(Context), cellI8pp, name);
        }

        if (slot->getType()->isPointerTy()) {
            return Builder.CreateLoad(slot->getType()->getPointerElementType(),
                                      slot, name.c_str());
        }

        return slot;
    }

    bool exists(const std::string& name) {
        return NamedValues.count(name) || globalVars.count(name);
    }
    bool isNonlocal(const std::string& name) const { return nonlocalVars.count(name) > 0; }

    // Register a nonlocal variable backed by a GC heap cell.
    // cellPtr is the i8* result of GC_malloc(8) — the cell address.
    // The current value (i8* boxed) must already be stored at *cellPtr by the caller.
    void defineNonlocalCell(const std::string& name, Value* cellPtr) {
        // Store the cell address in a dedicated i8* alloca slot
        Function* TheFunction = Builder.GetInsertBlock()->getParent();
        IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
        Value* slot = TmpB.CreateAlloca(cellPtr->getType(), nullptr, name + "__cell_slot");
        Builder.CreateStore(cellPtr, slot);
        NamedValues[name] = slot;
        nonlocalVars.insert(name);
    }

    // Get the raw i8* cell pointer for a nonlocal variable (used at lambda-capture time).
    Value* getCellPtr(const std::string& name) {
        if (!nonlocalVars.count(name)) return nullptr;
        Value* slot = NamedValues[name];
        return Builder.CreateLoad(Type::getInt8PtrTy(Context), slot, name + "_cell");
    }

    std::map<std::string, Value*> snapshot() const { return NamedValues; }
    void restore(const std::map<std::string, Value*>& saved) { NamedValues = saved; }

    // Restore both values and nonlocal metadata (used when returning from lambda compilation)
    void restoreWithNonlocal(const std::map<std::string, Value*>& savedVals,
                              const std::set<std::string>& savedNonlocal) {
        NamedValues  = savedVals;
        nonlocalVars = savedNonlocal;
    }

    std::set<std::string> snapshotNonlocal() const { return nonlocalVars; }
};
