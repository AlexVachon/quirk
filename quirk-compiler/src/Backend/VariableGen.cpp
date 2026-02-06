#pragma once
#include "llvm/IR/IRBuilder.h"
#include <map>
#include <iostream>

using namespace llvm;

class VariableGen {
    LLVMContext& Context;
    IRBuilder<>& Builder;

    // The Symbol Table: Maps variable name -> Stack Slot (AllocaInst*)
    std::map<std::string, Value*> NamedValues;

   public:
    VariableGen(LLVMContext& ctx, IRBuilder<>& build)
        : Context(ctx), Builder(build) {}

    // Clear variables when entering a new function
    void clear() { NamedValues.clear(); }

    // Register a function argument (e.g., 'self', 'x')
    void defineArgument(const std::string& name, Value* val) {
        // Create Alloca in the Entry Block
        Function* TheFunction = Builder.GetInsertBlock()->getParent();
        IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
        
        Value* slot = TmpB.CreateAlloca(val->getType(), nullptr, name);
        Builder.CreateStore(val, slot);
        NamedValues[name] = slot;
    }

    // Handle 'x := 10'
    void defineLocalVariable(const std::string& name, Value* val) {
        // 1. Optimization: Reuse existing slot if available
        if (NamedValues.count(name)) {
            Value* existingSlot = NamedValues[name];
            if (existingSlot->getType()->getPointerElementType() == val->getType()) {
                Builder.CreateStore(val, existingSlot);
                return;
            }
        }

        // 2. Create new Alloca in Entry Block
        Function* TheFunction = Builder.GetInsertBlock()->getParent();
        IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());
        
        Value* slot = TmpB.CreateAlloca(val->getType(), nullptr, name);
        
        // Store the value at the CURRENT position
        Builder.CreateStore(val, slot);
        NamedValues[name] = slot;
    }

    // Handle 'x = 20' (Reassignment)
    void updateLocalVariable(const std::string& name, Value* val) {
        if (NamedValues.find(name) == NamedValues.end()) {
            defineLocalVariable(name, val);
            return;
        }

        Value* slot = NamedValues[name];
        Builder.CreateStore(val, slot);
    }

    Value* resolveVariable(const std::string& name) {
        if (NamedValues.find(name) == NamedValues.end())
            return nullptr;

        Value* slot = NamedValues[name];

        if (slot->getType()->isPointerTy()) {
            return Builder.CreateLoad(slot->getType()->getPointerElementType(),
                                      slot, name.c_str());
        }

        return slot;
    }

    bool exists(const std::string& name) { return NamedValues.count(name); }
};