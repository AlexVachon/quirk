#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>
#include <iostream>
#include <vector>

using namespace llvm;

class StructGen;
class BuiltinGen;

class TypeExtensions {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    StructGen* structGen;
    BuiltinGen* builtinGen;

   public:
    TypeExtensions(LLVMContext& ctx,
                   Module* mod,
                   IRBuilder<>& build,
                   StructGen* sg,
                   BuiltinGen* bg)
        : Context(ctx),
          TheModule(mod),
          Builder(build),
          structGen(sg),
          builtinGen(bg) {}

    Value* tryHandleMethod(const std::string& typeName,
                           const std::string& methodName,
                           Value* objPtr,
                           const std::vector<Arg>& args,
                           std::function<Value*(Node*)> exprHandler) {
        if (typeName == "String" && methodName == "format") {
            return handleStringFormat(objPtr, args, exprHandler);
        }
        return nullptr;
    }

   private:
    // --- UPDATED: Box values instead of converting to String ---
    Value* prepareValueForRuntime(Value* v) {
        if (!v)
            return Constant::getNullValue(Type::getInt8PtrTy(Context));

        // 1. Unwrap Quirk 'String' objects to raw cstrings (char*)
        if (v->getType()->isPointerTy() &&
            v->getType()->getPointerElementType()->isStructTy()) {
            StructType* st =
                cast<StructType>(v->getType()->getPointerElementType());
            if (st->getName() == "String") {
                // Buffer is at Index 1 (Length=0, Buffer=1)
                Value* p = Builder.CreateStructGEP(st, v, 1);
                return Builder.CreateLoad(Type::getInt8PtrTy(Context), p);
            }
        }

        // 2. Handle Doubles: Bitcast to i64 -> IntToPtr
        if (v->getType()->isDoubleTy()) {
            Value* asInt = Builder.CreateBitCast(v, Type::getInt64Ty(Context));
            return Builder.CreateIntToPtr(asInt, Type::getInt8PtrTy(Context));
        }

        // 3. Handle Integers: IntToPtr
        if (v->getType()->isIntegerTy()) {
            return Builder.CreateIntToPtr(v, Type::getInt8PtrTy(Context));
        }

        // 4. Handle Pointers (cstring, etc): Bitcast to i8*
        if (v->getType()->isPointerTy()) {
            return Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
        }

        return v;
    }

    Value* handleStringFormat(Value* objPtr,
                              const std::vector<Arg>& args,
                              std::function<Value*(Node*)> exprHandler) {
        if (args.empty())
            return objPtr;
        bool isPositional = args[0].name.empty();
        return isPositional ? handlePositionalFormat(objPtr, args, exprHandler)
                            : handleMapFormat(objPtr, args, exprHandler);
    }

    Value* handlePositionalFormat(Value* objPtr,
                                  const std::vector<Arg>& args,
                                  std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> runtimeArgs;

        for (const auto& arg : args) {
            Value* val = exprHandler(arg.value.get());
            // --- FIX: Box raw value instead of converting to string ---
            runtimeArgs.push_back(prepareValueForRuntime(val));
        }

        Value* argsList = structGen->createListFromValues(runtimeArgs);

        Function* func = TheModule->getFunction("String_format_list");
        if (!func) {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }

        return Builder.CreateCall(func, {objPtr, argsList});
    }

    Value* handleMapFormat(Value* objPtr,
                           const std::vector<Arg>& args,
                           std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> keys;
        std::vector<Value*> vals;

        for (const auto& arg : args) {
            keys.push_back(Builder.CreateGlobalStringPtr(arg.name));
            Value* v = exprHandler(arg.value.get());
            // --- FIX: Box raw value instead of converting to string ---
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* keysList = structGen->createListFromValues(keys);
        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("String_format_map");
        if (!func) {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }
};