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

        Type* type = v->getType();

        // 1. Handle Integers (Keep as is -> Tagged Int)
        if (type->isIntegerTy()) {
            return Builder.CreateIntToPtr(v, Type::getInt8PtrTy(Context));
        }

        // 2. Handle Doubles (Convert to String!)
        if (type->isDoubleTy()) {
            Function* f2s = TheModule->getFunction("_float_to_str");
            if (!f2s) {
                // Fallback declaration if missing
                FunctionType* ft =
                    FunctionType::get(Type::getInt8PtrTy(Context),
                                      {Type::getDoubleTy(Context)}, false);
                f2s = Function::Create(ft, Function::ExternalLinkage,
                                       "_float_to_str", TheModule);
            }
            return Builder.CreateCall(f2s, {v});
        }

        // 3. Handle Pointers (Strings, Structs, Lists)
        if (type->isPointerTy()) {
            // Check if it's a Quirk Struct (e.g., %List*, %Vector*)
            Type* elType = type->getPointerElementType();
            if (elType->isStructTy()) {
                StructType* st = cast<StructType>(elType);
                std::string name = st->getName().str();

                // If it's already a String, unwrap buffer
                if (name == "String") {
                    Value* p = Builder.CreateStructGEP(st, v, 1);
                    return Builder.CreateLoad(Type::getInt8PtrTy(Context), p);
                }

                // If it's another Struct (List, etc.), call .__str()
                // We assume any struct passed to format has __str
                std::string strMethod = name + "___str";
                Function* strFunc = TheModule->getFunction(strMethod);
                if (strFunc) {
                    Value* strObj = Builder.CreateCall(strFunc, {v});
                    // Unwrap the result String object -> buffer
                    StructType* strType = TheModule->getTypeByName("String");
                    Value* p = Builder.CreateStructGEP(strType, strObj, 1);
                    return Builder.CreateLoad(Type::getInt8PtrTy(Context), p);
                }
            }

            // Fallback: simple cast
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