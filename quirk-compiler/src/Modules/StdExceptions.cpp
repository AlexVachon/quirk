#include "ApexModule.hpp"

class StdExceptionsModule : public ApexModule {
public:
    void registerStructs(LLVMContext& ctx, 
                         std::map<std::string, StructType*>& structTypes, 
                         StructGen* structGen) override {
        
        // Ensure String is available for the message field
        Type* stringPtr = structTypes.count("String") 
            ? PointerType::getUnqual(structTypes["String"]) 
            : Type::getInt8PtrTy(ctx);

        // 1. Base Exception Layout: { String* }
        if (!structTypes.count("Exception")) {
            StructType* excStruct = StructType::create(ctx, "Exception");
            excStruct->setBody({stringPtr});
            structTypes["Exception"] = excStruct;
            structGen->registerStructLayout("Exception", {"message"});
        }

        // 2. TypeError Layout (Inherits Exception -> identical layout due to field flattening)
        if (!structTypes.count("TypeError")) {
            StructType* typeErrStruct = StructType::create(ctx, "TypeError");
            typeErrStruct->setBody({stringPtr});
            structTypes["TypeError"] = typeErrStruct;
            structGen->registerStructLayout("TypeError", {"message"});
        }
    }

    void registerFunctions(Module* module, 
                           LLVMContext& ctx, 
                           std::map<std::string, StructType*>& structTypes) override {
        Type* voidTy = Type::getVoidTy(ctx);
        
        // Lookup Struct Pointers
        Type* excPtr = PointerType::getUnqual(structTypes["Exception"]);
        Type* typeErrPtr = PointerType::getUnqual(structTypes["TypeError"]);
        Type* stringPtr = PointerType::getUnqual(structTypes["String"]);

        // Exception__init(Exception*, String*)
        module->getOrInsertFunction("Exception__init", 
            FunctionType::get(voidTy, {excPtr, stringPtr}, false));

        // // TypeError__init(TypeError*, String*)
        // module->getOrInsertFunction("TypeError__init", 
        //     FunctionType::get(voidTy, {typeErrPtr, stringPtr}, false));
    }
};