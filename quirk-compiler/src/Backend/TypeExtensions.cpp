#include <functional>
#include <iostream>
#include <vector>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

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
        if (!v) return Constant::getNullValue(Type::getInt8PtrTy(Context));

        Type* type = v->getType();

        // 1. Integers (Box to void*)
        if (type->isIntegerTy()) {
            return Builder.CreateIntToPtr(v, Type::getInt8PtrTy(Context));
        }

        // 2. Doubles (Convert to String)
        if (type->isDoubleTy()) {
            Function* f2s = TheModule->getFunction("_float_to_str");
            if (!f2s) { 
                // Fallback / Auto-gen signature if missing
                FunctionType* ft = FunctionType::get(Type::getInt8PtrTy(Context), {Type::getDoubleTy(Context)}, false);
                f2s = Function::Create(ft, Function::ExternalLinkage, "_float_to_str", TheModule);
            }
            return Builder.CreateCall(f2s, {v});
        }

        // 3. Pointers (Strings, Structs)
        if (type->isPointerTy()) {
            Type* elType = type->getPointerElementType();
            
            if (elType->isStructTy()) {
                StructType* st = cast<StructType>(elType);
                std::string name = st->getName().str();
                if (name.find("struct.") == 0) name = name.substr(7);

                // A. Already a String Object? Unwrap .buffer
                if (name.find("String") == 0) { 
                    Value* bufPtr = structGen->getMemberPtr(v, "buffer");
                    if (bufPtr) return Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                }

                // B. Other Struct? Call .__str()
                std::string strMethod = name + "___str";
                Function* strFunc = TheModule->getFunction(strMethod);
                if (!strFunc) {
                    strMethod = name + "__str";
                    strFunc = TheModule->getFunction(strMethod);
                }

                if (strFunc) {
                    Value* retVal = Builder.CreateCall(strFunc, {v});

                    // --- FIX: Check Return Type ---
                    // Case 1: Returns raw cstring (i8*) -> Use directly
                    if (retVal->getType()->isPointerTy() && 
                        retVal->getType()->getPointerElementType()->isIntegerTy(8)) {
                        return retVal;
                    }

                    // Case 2: Returns String Object -> Unwrap .buffer
                    if (retVal->getType()->isPointerTy() && 
                        retVal->getType()->getPointerElementType()->isStructTy()) {
                        Value* bufPtr = structGen->getMemberPtr(retVal, "buffer");
                        if (bufPtr) return Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                    }
                }
            }
            
            // Fallback: Cast to i8* (e.g. raw string literal)
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