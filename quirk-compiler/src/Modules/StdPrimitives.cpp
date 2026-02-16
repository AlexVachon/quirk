#include "QuirkModule.hpp"

class StdPrimitivesModule : public QuirkModule {
public:
    void registerFunctions(Module* module, 
                           LLVMContext& ctx, 
                           std::map<std::string, StructType*>& structTypes) override {
        
        Type* intTy = Type::getInt32Ty(ctx);
        Type* doubleTy = Type::getDoubleTy(ctx);
        Type* boolTy = Type::getInt1Ty(ctx);
        Type* charTy = Type::getInt8Ty(ctx);
        
        // Ensure String struct is available
        Type* stringPtr = structTypes.count("String") 
            ? PointerType::getUnqual(structTypes["String"]) 
            : Type::getInt8PtrTy(ctx);

        // --- INT METHODS ---
        // Int_str(int) -> String*
        module->getOrInsertFunction("Int_str", FunctionType::get(stringPtr, {intTy}, false));
        
        // Int_abs(int) -> int
        module->getOrInsertFunction("Int_abs", FunctionType::get(intTy, {intTy}, false));
        
        // Int_pow(int, int) -> int
        module->getOrInsertFunction("Int_pow", FunctionType::get(intTy, {intTy, intTy}, false));
        
        // Int_is_even(int) -> bool
        module->getOrInsertFunction("Int_is_even", FunctionType::get(boolTy, {intTy}, false));
        module->getOrInsertFunction("Int_is_odd", FunctionType::get(boolTy, {intTy}, false));
        
        // Int_to_float(int) -> double
        module->getOrInsertFunction("Int_to_float", FunctionType::get(doubleTy, {intTy}, false));


        // --- DOUBLE METHODS ---
        // Double_str(double) -> String*
        module->getOrInsertFunction("Double_str", FunctionType::get(stringPtr, {doubleTy}, false));
        
        // Double_abs(double) -> double
        module->getOrInsertFunction("Double_abs", FunctionType::get(doubleTy, {doubleTy}, false));
        module->getOrInsertFunction("Double_ceil", FunctionType::get(doubleTy, {doubleTy}, false));
        module->getOrInsertFunction("Double_floor", FunctionType::get(doubleTy, {doubleTy}, false));
        module->getOrInsertFunction("Double_round", FunctionType::get(doubleTy, {doubleTy}, false));
        module->getOrInsertFunction("Double_sqrt", FunctionType::get(doubleTy, {doubleTy}, false));
        
        // Double_to_int(double) -> int
        module->getOrInsertFunction("Double_to_int", FunctionType::get(intTy, {doubleTy}, false));

        module->getOrInsertFunction("Bool_str", FunctionType::get(stringPtr, {boolTy}, false));

        
        module->getOrInsertFunction("Char__init", FunctionType::get(charTy, {}, false));
        module->getOrInsertFunction("Char_str", FunctionType::get(stringPtr, {charTy}, false));
        
        module->getOrInsertFunction("Char_is_upper", FunctionType::get(boolTy, {charTy}, false));
        module->getOrInsertFunction("Char_is_lower", FunctionType::get(boolTy, {charTy}, false));
        module->getOrInsertFunction("Char_is_digit", FunctionType::get(boolTy, {charTy}, false));
        module->getOrInsertFunction("Char_is_alpha", FunctionType::get(boolTy, {charTy}, false));
        module->getOrInsertFunction("Char_is_space", FunctionType::get(boolTy, {charTy}, false));
        
        module->getOrInsertFunction("Char_to_upper", FunctionType::get(charTy, {charTy}, false));
        module->getOrInsertFunction("Char_to_lower", FunctionType::get(charTy, {charTy}, false));
    }
};