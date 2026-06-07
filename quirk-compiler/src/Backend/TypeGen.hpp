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
    // Enum name → variant list, populated by Codegen's Pass 1. Used so a
    // user-declared `enum Level { ... }` resolves to i32 (matching what
    // enum-literal codegen emits at `Level.TRACE`). Without this, enum-
    // typed parameters fall through to the i8* default and any comparison
    // against an enum literal crashes LLVM's ICmp type assertion.
    const std::map<std::string, std::vector<std::string>>* enumVariants = nullptr;

public:
    TypeGen(LLVMContext& ctx, std::map<std::string, StructType*>& structs)
        : Context(ctx), StructTypes(structs) {}

    void setEnumRegistry(const std::map<std::string, std::vector<std::string>>* ev) {
        enumVariants = ev;
    }

    Type* getLLVMType(const std::string& typeName) {
        // Untyped parameter → generic void pointer (i8*)
        if (typeName.empty()) return Type::getInt8PtrTy(Context);
        // Nullable handling.
        //   - String? / List? / struct?  →  the base type is already a
        //     pointer (struct*), nullable comes for free.
        //   - Int? / Bool? / Double? / Char? / <Enum>?  →  the base
        //     lowers to a value type that can't hold null. Lower the
        //     nullable form to i8* instead; Codegen boxes on assign
        //     and unboxes on read. This is the v2.3.0 / v3-phase-1
        //     change that makes `case null` work on primitives.
        if (typeName.back() == '?') {
            std::string base = typeName.substr(0, typeName.size() - 1);
            if (base == "int" || base == "Int" ||
                base == "bool" || base == "Bool" ||
                base == "double" || base == "Double" ||
                base == "char" || base == "Char") {
                return Type::getInt8PtrTy(Context);
            }
            if (enumVariants && enumVariants->count(base)) {
                return Type::getInt8PtrTy(Context);
            }
            return getLLVMType(base);   // struct? / String? / etc. unchanged
        }
        if (typeName == "int" || typeName == "Int") return Type::getInt32Ty(Context);
        if (typeName == "bool" || typeName == "Bool") return Type::getInt1Ty(Context);
        if (typeName == "double" || typeName == "Double") return Type::getDoubleTy(Context);
        if (typeName == "void") return Type::getVoidTy(Context);

        if (typeName == "ptr" || typeName == "cstring" || typeName == "string" || typeName == "Any" || typeName == "any")
            return Type::getInt8PtrTy(Context);

        // Strip generic type parameters: "List[T]" → "List", "Map[K,V]" → "Map"
        auto bracketPos = typeName.find('[');
        if (bracketPos != std::string::npos)
            return getLLVMType(typeName.substr(0, bracketPos));

        // User-declared enums are i32 (the same type the enum-literal
        // codegen emits). Without this the param alloca is i8* and any
        // comparison against an enum value blows up ICmp.
        if (enumVariants && enumVariants->count(typeName)) {
            return Type::getInt32Ty(Context);
        }

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
