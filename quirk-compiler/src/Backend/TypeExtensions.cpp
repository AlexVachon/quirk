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
                           const std::vector<Arg>& args,  // <--- Using 'Arg'
                           std::function<Value*(Node*)> exprHandler) {
        if (typeName == "String" && methodName == "format") {
            return handleStringFormat(objPtr, args, exprHandler);
        }
        return nullptr;
    }

   private:
    Value* convertToString(Value* v) {
        if (!v)
            return nullptr;
        if (v->getType()->isIntegerTy())
            return builtinGen->generateIntToString(v);
        if (v->getType()->isDoubleTy())
            return builtinGen->generateDoubleToString(v);
        if (v->getType()->isPointerTy() &&
            v->getType()->getPointerElementType()->isStructTy()) {
            StructType* st =
                cast<StructType>(v->getType()->getPointerElementType());
            if (st->getName() == "String") {
                Value* p = Builder.CreateStructGEP(st, v, 0);
                return Builder.CreateLoad(Type::getInt8PtrTy(Context), p);
            }
        }
        return v;
    }

    Value* handleStringFormat(Value* objPtr,
                              const std::vector<Arg>& args,  // <--- Using 'Arg'
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
        std::vector<Value*> stringifiedArgs;

        for (const auto& arg : args) {
            Value* val = exprHandler(arg.value.get());
            stringifiedArgs.push_back(convertToString(val));
        }

        Value* argsList = structGen->createListFromValues(stringifiedArgs);

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
            vals.push_back(convertToString(v));
        }

        Value* keysList = structGen->createListFromValues(keys);
        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("String_format_map");

        // --- NEW: Safety Check ---
        if (!func) {
            std::cerr << "Error: 'String_format_map' not found. Ensure "
                         "std/string.apx is loaded."
                      << std::endl;
            // Return null or create a trap to stop compilation safely
            return Constant::getNullValue(objPtr->getType());
        }
        // -------------------------
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }
};