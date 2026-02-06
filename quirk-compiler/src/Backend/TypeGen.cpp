#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include <map>
#include <string>

using namespace llvm;

class TypeGen {
    LLVMContext& Context;
    std::map<std::string, StructType*>& StructTypes;

public:
    TypeGen(LLVMContext& ctx, std::map<std::string, StructType*>& structs) 
        : Context(ctx), StructTypes(structs) {}

    Type* getLLVMType(const std::string& typeName) {
        if (typeName == "int") return Type::getInt32Ty(Context);
        if (typeName == "bool") return Type::getInt1Ty(Context);
        if (typeName == "char") return Type::getInt8Ty(Context);
        if (typeName == "double") return Type::getDoubleTy(Context);
        if (typeName == "void") return Type::getVoidTy(Context);
        
        // FIX: Add "cstring" to pointer types
        // "string" is kept for legacy compatibility if needed, but cstring is preferred
        if (typeName == "ptr" || typeName == "cstring" || typeName == "string" || typeName == "any") {
            return Type::getInt8PtrTy(Context);
        }

        // Structs are passed by reference (Pointer to Struct)
        if (StructTypes.count(typeName)) {
            return PointerType::getUnqual(StructTypes[typeName]);
        }

        return Type::getInt32Ty(Context);
    }

    Type* getFunctionReturnType(const std::string& retType) {
        if (retType.empty()) return Type::getVoidTy(Context);
        return getLLVMType(retType);
    }
};