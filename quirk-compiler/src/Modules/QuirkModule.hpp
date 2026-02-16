#pragma once
#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include <map>

class StructGen; // Forward decl

using namespace llvm;

class QuirkModule {
public:
    virtual ~QuirkModule() = default;

    virtual void registerStructs(LLVMContext& ctx, 
                                 std::map<std::string, StructType*>& structTypes, 
                                 StructGen* structGen) {}

    virtual void registerFunctions(Module* module, 
                                   LLVMContext& ctx, 
                                   std::map<std::string, StructType*>& structTypes) {}
};