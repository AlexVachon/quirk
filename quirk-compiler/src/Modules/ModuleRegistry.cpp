#pragma once
#include "QuirkModule.hpp"

#include "StdFile.cpp"
#include "StdList.cpp"
#include "StdMap.cpp"
#include "StdMath.cpp"
#include "StdPrimitives.cpp"
#include "StdString.cpp"
#include "StdExceptions.cpp"

class ModuleRegistry {
    std::vector<QuirkModule*> modules;

   public:
    ModuleRegistry() {
        modules.push_back(new StdStringModule());
        // modules.push_back(new StdExceptionsModule());
        modules.push_back(new StdPrimitivesModule());
        modules.push_back(new StdListModule());
        modules.push_back(new StdMapModule());
        modules.push_back(new StdMathModule());
        modules.push_back(new StdFileModule());
    }

    ~ModuleRegistry() {
        for (auto* m : modules)
            delete m;
    }

    void registerAll(LLVMContext& ctx,
                     std::map<std::string, StructType*>& structTypes,
                     StructGen* structGen,
                     Module* module) {

        // 1. Register Structs
        for (auto& mod : modules) {
            mod->registerStructs(ctx, structTypes, structGen);
        }

        // 2. Register Functions (NEW)
        for (auto& mod : modules) {
            mod->registerFunctions(module, ctx, structTypes);
        }
    }
};