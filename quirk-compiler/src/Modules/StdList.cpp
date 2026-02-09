#include "ApexModule.hpp"

class StdListModule : public ApexModule {
   public:
    void registerFunctions(
        Module* module,
        LLVMContext& ctx,
        std::map<std::string, StructType*>& structTypes) override {
        // Define common types
        Type* voidTy = Type::getVoidTy(ctx);
        Type* voidPtr = Type::getInt8PtrTy(ctx);  // 'any' type (void*)
        Type* intTy = Type::getInt32Ty(ctx);
        Type* boolTy = Type::getInt1Ty(ctx);

        // Get Struct Pointers (default to i8* if not found, though they should
        // exist)
        Type* listPtr = structTypes.count("List")
                            ? PointerType::getUnqual(structTypes["List"])
                            : voidPtr;

        Type* iterPtr =
            structTypes.count("ListIterator")
                ? PointerType::getUnqual(structTypes["ListIterator"])
                : voidPtr;

        Type* stringPtr = structTypes.count("String")
                              ? PointerType::getUnqual(structTypes["String"])
                              : voidPtr;

        // ==========================================
        //  List Lifecycle
        // ==========================================

        // List__init(self: List*, capacity: int) -> void
        module->getOrInsertFunction(
            "List__init", FunctionType::get(voidTy, {listPtr, intTy}, false));

        // List___del(self: List*) -> void
        module->getOrInsertFunction(
            "List___del", FunctionType::get(voidTy, {listPtr}, false));

        // ==========================================
        //  Core Methods
        // ==========================================

        // List_append(self: List*, item: any) -> void
        module->getOrInsertFunction(
            "List_append",
            FunctionType::get(voidTy, {listPtr, voidPtr}, false));

        // List_pop(self: List*) -> any
        module->getOrInsertFunction(
            "List_pop", FunctionType::get(voidPtr, {listPtr}, false));

        // List_len(self: List*) -> int
        module->getOrInsertFunction("List_len",
                                    FunctionType::get(intTy, {listPtr}, false));

        // List_clear(self: List*) -> void
        module->getOrInsertFunction(
            "List_clear", FunctionType::get(voidTy, {listPtr}, false));

        // List_is_empty(self: List*) -> bool
        module->getOrInsertFunction(
            "List_is_empty", FunctionType::get(boolTy, {listPtr}, false));

        // ==========================================
        //  Operators
        // ==========================================

        // List___get(self: List*, index: int) -> any
        module->getOrInsertFunction(
            "List___get", FunctionType::get(voidPtr, {listPtr, intTy}, false));

        // List___set(self: List*, index: int, item: any) -> void
        module->getOrInsertFunction(
            "List___set",
            FunctionType::get(voidTy, {listPtr, intTy, voidPtr}, false));

        // List___iter(self: List*) -> ListIterator*
        module->getOrInsertFunction(
            "List___iter", FunctionType::get(iterPtr, {listPtr}, false));

        // List___str(self: List*) -> String*
        module->getOrInsertFunction(
            "List___str", FunctionType::get(stringPtr, {listPtr}, false));

        // List___repr(self: List*) -> String*
        module->getOrInsertFunction(
            "List___repr", FunctionType::get(stringPtr, {listPtr}, false));

        // ==========================================
        //  ListIterator Methods
        // ==========================================

        // ListIterator__init(self: Iterator*, list: List*) -> void
        module->getOrInsertFunction(
            "ListIterator__init",
            FunctionType::get(voidTy, {iterPtr, listPtr}, false));

        // ListIterator___has_next(self: Iterator*) -> bool
        module->getOrInsertFunction(
            "ListIterator___has_next",
            FunctionType::get(boolTy, {iterPtr}, false));

        // ListIterator___next(self: Iterator*) -> any
        module->getOrInsertFunction(
            "ListIterator___next",
            FunctionType::get(voidPtr, {iterPtr}, false));
    }

    void registerStructs(LLVMContext& ctx,
                         std::map<std::string, StructType*>& structTypes,
                         StructGen* structGen) override {
        // 1. List
        if (structTypes.find("List") == structTypes.end()) {
            std::vector<Type*> elements = {
                Type::getInt8PtrTy(ctx),  // data (void**)
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