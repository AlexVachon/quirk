#include "ApexModule.hpp"

class StdListModule : public ApexModule {
   public:
    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        // 1. List
        if (structTypes.find("List") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt8PtrTy(ctx),  // data
                Type::getInt32Ty(ctx),    // length
                Type::getInt32Ty(ctx)     // capacity
            };
            structTypes["List"] = StructType::create(ctx, elements, "List");
        }
        structGen->registerStructLayout("List", {"data", "length", "capacity"});

        // 2. ListIterator
        if (structTypes.find("ListIterator") == structTypes.end()) {
            std::vector<Type*> elements = {
                PointerType::getUnqual(structTypes["List"]),  // list_ref
                Type::getInt32Ty(ctx)                         // idx
            };
            structTypes["ListIterator"] =
                StructType::create(ctx, elements, "ListIterator");
        }
        structGen->registerStructLayout("ListIterator", {"list_ref", "idx"});
    }
};