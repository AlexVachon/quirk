#include "QuirkModule.hpp"

class StdStringModule : public QuirkModule {
   public:
    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        // String Layout: { int magic, int length, char* buffer }
        if (structTypes.find("String") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt8PtrTy(ctx), // Index 0: buffer (char*)
                Type::getInt32Ty(ctx)    // Index 1: length (int)
            };
            structTypes["String"] = StructType::create(ctx, elements, "String");
        }
        structGen->registerStructLayout("String", {"buffer", "length"});

        // StringIterator
        if (structTypes.find("StringIterator") == structTypes.end()) {
            std::vector<Type*> elements = {
                PointerType::getUnqual(structTypes["String"]),
                Type::getInt32Ty(ctx)};
            structTypes["StringIterator"] =
                StructType::create(ctx, elements, "StringIterator");
        }
        structGen->registerStructLayout("StringIterator", {"str_ref", "idx"});

        // Any — Tagged union for dynamic typing
        // Layout must match C struct: { i32 tag, i32 ival, double dval, i8* ptr }
        if (structTypes.find("Any") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt32Ty(ctx),   // tag  (AnyTag enum)
                Type::getInt32Ty(ctx),   // ival (Int / Bool / Char)
                Type::getDoubleTy(ctx),  // dval (Double)
                Type::getInt8PtrTy(ctx), // ptr  (String* / List* / Map* / other)
            };
            structTypes["Any"] = StructType::create(ctx, elements, "Any");
        }
        structGen->registerStructLayout("Any", {"tag", "ival", "dval", "ptr"});
    }
};