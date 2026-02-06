#include "ApexModule.hpp"

class StdStringModule : public ApexModule {
   public:
    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        // String Layout: { int magic, int length, char* buffer }
        if (structTypes.find("String") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt32Ty(ctx),   // length (Index 0)
                Type::getInt8PtrTy(ctx)  // buffer (Index 1)
            };
            structTypes["String"] = StructType::create(ctx, elements, "String");
        }
        // Update layout map
        structGen->registerStructLayout("String", {"length", "buffer"});

        // StringIterator
        if (structTypes.find("StringIterator") == structTypes.end()) {
            std::vector<Type*> elements = {
                PointerType::getUnqual(structTypes["String"]),
                Type::getInt32Ty(ctx)};
            structTypes["StringIterator"] =
                StructType::create(ctx, elements, "StringIterator");
        }
        structGen->registerStructLayout("StringIterator", {"str_ref", "idx"});
    }
};