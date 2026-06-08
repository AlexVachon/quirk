#pragma once
// EnumGen — owns all enum-related Codegen state and the "pure"
// emission steps (per-enum LLVM globals + the variant-to-name
// `__<Enum>_str` function). Lives alongside StructGen / TypeGen /
// MathGen / etc. as part of the v2.3.2 locality refactor.
//
// Codegen.cpp still handles the dispatch at the AST node level
// (handleCall member-access, handleExpression MemberAccess, etc.)
// because that path needs too many Codegen internals (varGen,
// structFieldTypes, currentCodegenClass). It calls *into* EnumGen
// via the accessors below: `isEnum`, `getBacking`, `track*`,
// `findOrEmit*`, etc.
//
// The data EnumGen owns:
//   enumVariants       enum name → ordered variant identifiers
//   varEnumTypes       local-var name → enum type name (for
//                      carrier resolution when `g.value()` is called)
//   backedEnums        enum name → BackedEnumInfo {backingType,
//                      resolved values per variant}
//
// LLVM globals named per-enum:
//   __<Enum>_packed    backing values blob (String null-separated /
//                      int32 array / double array depending on backing)
//   __<Enum>_names     variant identifiers blob (always Strings)
//   __<Enum>_name      enum name as a C string (used in lookup-miss
//                      error messages from the runtime helpers)
#include <map>
#include <string>
#include <vector>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class EnumGen {
    LLVMContext& Context;
    Module*      TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;

   public:
    struct BackedEnumInfo {
        std::string backingType;             // "" / "String" / "Int" / "Double"
        std::vector<std::string> values;     // one per variant (defaults resolved)
    };

   private:
    std::map<std::string, std::vector<std::string>> enumVariants;
    std::map<std::string, std::string>             varEnumTypes;
    std::map<std::string, BackedEnumInfo>          backedEnums;

   public:
    EnumGen(LLVMContext& ctx, Module* mod, IRBuilder<>& builder,
            std::map<std::string, StructType*>& structs)
        : Context(ctx), TheModule(mod), Builder(builder), StructTypes(structs) {}

    void setModule(Module* mod) { TheModule = mod; }

    // -- Phase 1b: register enum declarations from the AST. ------
    // Called once during Codegen setup for every EnumNode.
    void registerEnum(EnumNode* e) {
        enumVariants[e->name] = e->variants;
        BackedEnumInfo info;
        // Treat unbacked enums as String-backed-with-name-defaults
        // for uniform global emission. The user-facing distinction
        // (whether `EnumName(v)` is allowed, what `.value()` returns)
        // lives at Sema; Codegen just needs the values blob.
        info.backingType = e->backingType.empty() ? "String" : e->backingType;
        info.values.reserve(e->variants.size());
        for (size_t i = 0; i < e->variants.size(); i++) {
            std::string v = (i < e->variantValues.size()) ? e->variantValues[i] : std::string();
            if (v.empty()) {
                v = (info.backingType == "Int")
                    ? std::to_string(i)
                    : e->variants[i];
            }
            info.values.push_back(std::move(v));
        }
        backedEnums[e->name] = std::move(info);
    }

    // -- Accessors used by Codegen during dispatch. -------------
    const std::vector<std::string>* variantsOf(const std::string& enumName) const {
        auto it = enumVariants.find(enumName);
        return it == enumVariants.end() ? nullptr : &it->second;
    }
    const BackedEnumInfo* infoOf(const std::string& enumName) const {
        auto it = backedEnums.find(enumName);
        return it == backedEnums.end() ? nullptr : &it->second;
    }
    bool isEnum(const std::string& name) const {
        return enumVariants.count(name) > 0;
    }
    const std::map<std::string, std::vector<std::string>>& allVariants() const {
        return enumVariants;
    }
    std::map<std::string, std::vector<std::string>>* variantsRegistryPtr() {
        // TypeGen wants a raw pointer to the map (for its
        // `setEnumRegistry`); expose it but keep ownership here.
        return &enumVariants;
    }
    // Raw-map handles for Codegen's reference aliases — let the
    // existing inline `backedEnums[X]` / `varEnumTypes.find(X)`
    // dispatch keep working against EnumGen-owned storage.
    std::map<std::string, std::string>*           varEnumTypesPtr() { return &varEnumTypes; }
    std::map<std::string, BackedEnumInfo>*        backedEnumsPtr()  { return &backedEnums; }

    // varEnumTypes — tracks which local bindings are enum-typed so
    // `g.value()` etc. can resolve the carrier enum.
    void trackVarEnum(const std::string& varName, const std::string& enumName) {
        varEnumTypes[varName] = enumName;
    }
    std::string lookupVarEnum(const std::string& varName) const {
        auto it = varEnumTypes.find(varName);
        return it == varEnumTypes.end() ? std::string() : it->second;
    }

    // -- Phase 3: emit per-enum LLVM globals + the __<Enum>_str fn.
    // Called from Codegen after StructTypes is populated. `emitStrFn`
    // is split so it can run after Pass-3 (when the global str
    // helper layout is finalised); `emitGlobals` runs alongside.
    void emitGlobals() {
        for (const auto& [name, info] : backedEnums) {
            // Names blob — variant identifiers as packed null-separated
            // C strings. Used by `EnumName.names()` and the lookup-
            // miss error messages.
            {
                std::string namesPacked;
                auto vIt = enumVariants.find(name);
                if (vIt != enumVariants.end()) {
                    for (const auto& v : vIt->second) {
                        namesPacked += v;
                        namesPacked.push_back('\0');
                    }
                }
                Constant* blob = ConstantDataArray::getString(Context, namesPacked, /*AddNull=*/false);
                auto* gv = new GlobalVariable(
                    *TheModule, blob->getType(), /*isConstant=*/true,
                    GlobalValue::PrivateLinkage, blob, "__" + name + "_names");
                gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                (void)gv;
            }
            // Values blob — shape depends on backing type.
            if (info.backingType == "String") {
                std::string packed;
                for (const auto& v : info.values) { packed += v; packed.push_back('\0'); }
                Constant* blob = ConstantDataArray::getString(Context, packed, /*AddNull=*/false);
                auto* gv = new GlobalVariable(
                    *TheModule, blob->getType(), /*isConstant=*/true,
                    GlobalValue::PrivateLinkage, blob, "__" + name + "_packed");
                gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                (void)gv;
            } else if (info.backingType == "Int") {
                std::vector<Constant*> vals;
                vals.reserve(info.values.size());
                for (const auto& v : info.values) {
                    int32_t n = 0;
                    try { n = (int32_t)std::stol(v); } catch (...) { n = 0; }
                    vals.push_back(ConstantInt::get(Type::getInt32Ty(Context), n));
                }
                auto* arrTy = ArrayType::get(Type::getInt32Ty(Context), vals.size());
                Constant* blob = ConstantArray::get(arrTy, vals);
                auto* gv = new GlobalVariable(
                    *TheModule, arrTy, /*isConstant=*/true,
                    GlobalValue::PrivateLinkage, blob, "__" + name + "_packed");
                gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                (void)gv;
            } else if (info.backingType == "Double") {
                std::vector<Constant*> vals;
                vals.reserve(info.values.size());
                for (const auto& v : info.values) {
                    double d = 0.0;
                    try { d = std::stod(v); } catch (...) { d = 0.0; }
                    vals.push_back(ConstantFP::get(Type::getDoubleTy(Context), d));
                }
                auto* arrTy = ArrayType::get(Type::getDoubleTy(Context), vals.size());
                Constant* blob = ConstantArray::get(arrTy, vals);
                auto* gv = new GlobalVariable(
                    *TheModule, arrTy, /*isConstant=*/true,
                    GlobalValue::PrivateLinkage, blob, "__" + name + "_packed");
                gv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
                (void)gv;
            }
            // Enum-name global — used by lookup helpers in their
            // "X is not a valid <EnumName>" error message.
            Constant* nameBlob = ConstantDataArray::getString(Context, name, /*AddNull=*/true);
            auto* nameGv = new GlobalVariable(
                *TheModule, nameBlob->getType(), /*isConstant=*/true,
                GlobalValue::PrivateLinkage, nameBlob, "__" + name + "_name");
            nameGv->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
            (void)nameGv;
        }
    }

    // Emit the `__<Enum>_str(i32 ordinal) -> String*` helper for each
    // enum. Codegen kicks this in Pass 3 alongside other late-pass
    // function emissions; the `makeStrFn` callable is the runtime's
    // `make_String` (or whichever helper produces a fresh String*).
    void emitStrFns(const std::vector<std::unique_ptr<Node>>& nodes,
                    FunctionCallee makeStrFn, Type* strPtrTy) {
        for (const auto& node : nodes) {
            auto* e = dynamic_cast<EnumNode*>(node.get());
            if (!e) continue;
            std::string fnName = "__" + e->name + "_str";
            FunctionType* ft = FunctionType::get(strPtrTy, {Type::getInt32Ty(Context)}, false);
            Function* strFn = Function::Create(ft, Function::InternalLinkage, fnName, TheModule);

            BasicBlock* entry = BasicBlock::Create(Context, "entry", strFn);
            BasicBlock* dflt  = BasicBlock::Create(Context, "default", strFn);
            Builder.SetInsertPoint(entry);
            Value* arg = strFn->arg_begin();
            SwitchInst* sw = Builder.CreateSwitch(arg, dflt, (unsigned)e->variants.size());

            auto makeStr = [&](const std::string& text) -> Value* {
                Value* rawPtr = Builder.CreateGlobalStringPtr(text);
                Value* strVal = Builder.CreateCall(makeStrFn, {rawPtr});
                return strPtrTy->isPointerTy() && strPtrTy != Type::getInt8PtrTy(Context)
                    ? Builder.CreateBitCast(strVal, strPtrTy)
                    : strVal;
            };

            for (int i = 0; i < (int)e->variants.size(); i++) {
                BasicBlock* bb = BasicBlock::Create(Context, "case_" + e->variants[i], strFn);
                sw->addCase(ConstantInt::get(Type::getInt32Ty(Context), i), bb);
                Builder.SetInsertPoint(bb);
                Builder.CreateRet(makeStr(e->variants[i]));
            }
            Builder.SetInsertPoint(dflt);
            Builder.CreateRet(makeStr("<unknown>"));
        }
    }
};
