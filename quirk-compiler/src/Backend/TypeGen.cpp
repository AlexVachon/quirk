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
        // FIX: Map uppercase primitive names to LLVM primitives
        if (typeName == "int" || typeName == "Int") return Type::getInt32Ty(Context);
        if (typeName == "bool" || typeName == "Bool") return Type::getInt1Ty(Context);
        if (typeName == "char" || typeName == "Char") return Type::getInt8Ty(Context);
        if (typeName == "double" || typeName == "Double") return Type::getDoubleTy(Context);
        if (typeName == "void") return Type::getVoidTy(Context);
        
        if (typeName == "ptr" || typeName == "cstring" || typeName == "string" || typeName == "any") {
            return Type::getInt8PtrTy(Context);
        }

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