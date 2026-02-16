#include "QuirkModule.hpp"

class StdMapModule : public QuirkModule {
   public:
    void registerFunctions(
        Module* module,
        LLVMContext& ctx,
        std::map<std::string, StructType*>& structTypes) override {
        Type* voidTy = Type::getVoidTy(ctx);
        Type* voidPtr = Type::getInt8PtrTy(ctx);
        Type* intTy = Type::getInt32Ty(ctx);
        Type* boolTy = Type::getInt1Ty(ctx);

        Type* mapPtr = structTypes.count("Map")
                           ? PointerType::getUnqual(structTypes["Map"])
                           : voidPtr;

        // --- FIX: Force String Struct Lookup ---
        StructType* stringStruct = nullptr;

        if (structTypes.count("String")) {
            stringStruct = structTypes["String"];
        } else {
            stringStruct = StructType::getTypeByName(ctx, "String");
            if (!stringStruct) {
                // If missing, create an opaque struct named "String"
                // This forces LLVM to treat arguments as %String*
                stringStruct = StructType::create(ctx, "String");
            }
            structTypes["String"] = stringStruct;
        }

        // Now 'stringPtr' is %String* (not i8*)
        Type* stringPtr = PointerType::getUnqual(stringStruct);
        // ---------------------------------------

        // Lifecycle
        module->getOrInsertFunction("Map__init",
                                    FunctionType::get(voidTy, {mapPtr}, false));
        module->getOrInsertFunction("Map___del",
                                    FunctionType::get(voidTy, {mapPtr}, false));

        // Methods (Updated signatures)
        module->getOrInsertFunction(
            "Map_put",
            FunctionType::get(voidTy, {mapPtr, stringPtr, voidPtr}, false));

        module->getOrInsertFunction(
            "Map_get", FunctionType::get(voidPtr, {mapPtr, stringPtr}, false));

        module->getOrInsertFunction(
            "Map_has", FunctionType::get(boolTy, {mapPtr, stringPtr}, false));

        module->getOrInsertFunction(
            "Map_remove",
            FunctionType::get(voidTy, {mapPtr, stringPtr}, false));

        module->getOrInsertFunction("Map_len",
                                    FunctionType::get(intTy, {mapPtr}, false));
        module->getOrInsertFunction("Map_clear",
                                    FunctionType::get(voidTy, {mapPtr}, false));

        // Operators
        module->getOrInsertFunction(
            "Map___get",
            FunctionType::get(voidPtr, {mapPtr, stringPtr}, false));
        module->getOrInsertFunction(
            "Map___set",
            FunctionType::get(voidTy, {mapPtr, stringPtr, voidPtr}, false));
        module->getOrInsertFunction(
            "Map___str", FunctionType::get(stringPtr, {mapPtr}, false));
    }

    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        if (structTypes.find("Map") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt8PtrTy(ctx),  // entries
                Type::getInt32Ty(ctx),    // capacity
                Type::getInt32Ty(ctx)     // size
            };
            structTypes["Map"] = StructType::create(ctx, elements, "Map");
        }
        structGen->registerStructLayout("Map", {"entries", "capacity", "size"});
    }
};