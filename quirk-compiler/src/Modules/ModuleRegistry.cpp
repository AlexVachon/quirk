#pragma once
#include "ApexModule.hpp"

#include "StdString.cpp"
#include "StdList.cpp"
#include "StdMath.cpp"
#include "StdFile.cpp"

class ModuleRegistry {
    std::vector<ApexModule*> modules;

   public:
    ModuleRegistry() {
        modules.push_back(new StdStringModule());
        modules.push_back(new StdListModule());
        modules.push_back(new StdMathModule());
        modules.push_back(new StdFileModule());
    }

    ~ModuleRegistry() {
        for (auto* m : modules)
            delete m;
    }

    void registerAll(LLVMContext& ctx,
                     std::map<std::string, StructType*>& structTypes,
                     StructGen* structGen) {
        for (auto* m : modules) {
            m->registerStructs(ctx, structTypes, structGen);
        }
    }
};