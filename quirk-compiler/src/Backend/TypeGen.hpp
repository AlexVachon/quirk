#pragma once
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
        // Untyped parameter → generic void pointer (i8*)
        if (typeName.empty()) return Type::getInt8PtrTy(Context);
        // Strip Optional marker — "String?" and "String" resolve identically
        if (typeName.back() == '?')
            return getLLVMType(typeName.substr(0, typeName.size() - 1));
        if (typeName == "int" || typeName == "Int") return Type::getInt32Ty(Context);
        if (typeName == "bool" || typeName == "Bool") return Type::getInt1Ty(Context);
        if (typeName == "char" || typeName == "Char") return Type::getInt8Ty(Context);
        if (typeName == "double" || typeName == "Double") return Type::getDoubleTy(Context);
        if (typeName == "void") return Type::getVoidTy(Context);

        if (typeName == "ptr" || typeName == "cstring" || typeName == "string" || typeName == "Any" || typeName == "any")
            return Type::getInt8PtrTy(Context);

        // Strip generic type parameters: "List[T]" → "List", "Map[K,V]" → "Map"
        auto bracketPos = typeName.find('[');
        if (bracketPos != std::string::npos)
            return getLLVMType(typeName.substr(0, bracketPos));

        // Handle Structs (including String!)
        if (StructTypes.count(typeName)) {
            return PointerType::getUnqual(StructTypes[typeName]);
        }

        // Unknown type — could be a generic param (T, U) or an unresolved extern.
        // All user-defined structs are pre-registered in Pass 1, so anything still
        // unknown here is treated as type-erased Any (i8*).
        return Type::getInt8PtrTy(Context);
    }

    Type* getFunctionReturnType(const std::string& retType) {
        if (retType.empty()) return Type::getVoidTy(Context);
        return getLLVMType(retType);
    }
};
