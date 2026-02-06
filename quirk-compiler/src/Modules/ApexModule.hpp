#pragma once
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/DerivedTypes.h"
#include <map>
#include <string>
#include <vector>

class StructGen;

using namespace llvm;

class ApexModule {
public:
    virtual ~ApexModule() = default;
    
    virtual void registerStructs(LLVMContext& ctx, 
                                 std::map<std::string, StructType*>& structTypes, 
                                 StructGen* structGen) = 0;
};