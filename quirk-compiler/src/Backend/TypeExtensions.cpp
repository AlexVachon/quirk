#include <functional>
#include <iostream>
#include <vector>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class StructGen;
class BuiltinGen;

class TypeExtensions
{
    LLVMContext &Context;
    Module *TheModule;
    IRBuilder<> &Builder;
    StructGen *structGen;
    BuiltinGen *builtinGen;

public:
    TypeExtensions(LLVMContext &ctx,
                   Module *mod,
                   IRBuilder<> &build,
                   StructGen *sg,
                   BuiltinGen *bg)
        : Context(ctx),
          TheModule(mod),
          Builder(build),
          structGen(sg),
          builtinGen(bg) {}

    Value *tryHandleMethod(const std::string &typeName,
                           const std::string &methodName,
                           Value *objPtr,
                           const std::vector<Arg> &args,
                           std::function<Value *(Node *)> exprHandler)
    {
        if (typeName == "String" && methodName == "format")
        {
            return handleStringFormat(objPtr, args, exprHandler);
        }
        return nullptr;
    }

private:
    // --- UPDATED: Call __str() dynamically, then unwrap to raw C-Strings (i8*) ---
    Value *prepareValueForRuntime(Value *v)
    {
        if (!v) return Constant::getNullValue(Type::getInt8PtrTy(Context));

        Type *type = v->getType();

        // 1. Booleans (i1 -> Call Bool_str -> unwrap)
        if (type->isIntegerTy(1)) {
            Function *f = TheModule->getFunction("Bool_str");
            if (f) v = Builder.CreateCall(f, {v});
        }
        
        // 2. Characters (i8 -> Call Char_str -> unwrap)
        else if (type->isIntegerTy(8)) {
            Function *f = TheModule->getFunction("Char_str");
            if (f) v = Builder.CreateCall(f, {v});
        }

        // 3. Integers (i32 -> Call Int_str -> unwrap)
        else if (type->isIntegerTy()) {
            Function *f = TheModule->getFunction("Int_str");
            if (f) v = Builder.CreateCall(f, {v});
        }

        // 4. Doubles (Double -> Call Double_str -> unwrap)
        else if (type->isDoubleTy()) {
            Function *f = TheModule->getFunction("Double_str");
            if (f) {
                v = Builder.CreateCall(f, {v});
            } else {
                Function *f2s = TheModule->getFunction("_float_to_str");
                if (f2s) v = Builder.CreateCall(f2s, {v}); 
            }
        }

        // 5. Pointers (Structs, Strings, Raw C-Strings)
        if (v->getType()->isPointerTy()) {
            Type *elType = v->getType()->getPointerElementType();

            // If it's a Struct, check if we need to call __str
            if (elType->isStructTy()) {
                StructType *st = cast<StructType>(elType);
                std::string name = st->getName().str();
                if (name.find("struct.") == 0) name = name.substr(7);

                // If it is NOT already a String, call __str
                if (name.find("String") != 0) {
                    std::string strMethod = name + "___str";
                    Function *strFunc = TheModule->getFunction(strMethod);
                    if (!strFunc) strFunc = TheModule->getFunction(name + "__str");

                    if (strFunc) {
                        v = Builder.CreateCall(strFunc, {v}); // Call __str
                    } else {
                        // Fallback if no __str exists
                        return Constant::getNullValue(Type::getInt8PtrTy(Context));
                    }
                }
            }

            // --- THE CRITICAL FIX: Unwrap ALL String Objects to i8* ---
            // Whether it was originally a String, or we just called __str to get one,
            // we MUST unwrap it to a raw C-string for the C-runtime format list!
            if (v->getType()->isPointerTy() && v->getType()->getPointerElementType()->isStructTy()) {
                StructType *st = cast<StructType>(v->getType()->getPointerElementType());
                if (st->getName().str().find("String") != std::string::npos) {
                    Value *bufPtr = structGen->getMemberPtr(v, "buffer");
                    if (bufPtr) {
                        return Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                    }
                }
            }
            
            // If it's already a raw C-string (i8*), return as-is
            if (v->getType()->isPointerTy() && v->getType()->getPointerElementType()->isIntegerTy(8)) {
                return v;
            }
        }

        return Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
    }

    Value *handleStringFormat(Value *objPtr,
                              const std::vector<Arg> &args,
                              std::function<Value *(Node *)> exprHandler)
    {
        if (args.empty())
            return objPtr;
        bool isPositional = args[0].name.empty();
        return isPositional ? handlePositionalFormat(objPtr, args, exprHandler)
                            : handleMapFormat(objPtr, args, exprHandler);
    }

    Value *handlePositionalFormat(Value *objPtr,
                                  const std::vector<Arg> &args,
                                  std::function<Value *(Node *)> exprHandler)
    {
        std::vector<Value *> runtimeArgs;

        for (const auto &arg : args)
        {
            Value *val = exprHandler(arg.value.get());
            // --- FIX: Box raw value instead of converting to string ---
            runtimeArgs.push_back(prepareValueForRuntime(val));
        }

        Value *argsList = structGen->createListFromValues(runtimeArgs);

        Function *func = TheModule->getFunction("String_format_list");
        if (!func)
        {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }

        return Builder.CreateCall(func, {objPtr, argsList});
    }

    Value *handleMapFormat(Value *objPtr,
                           const std::vector<Arg> &args,
                           std::function<Value *(Node *)> exprHandler)
    {
        std::vector<Value *> keys;
        std::vector<Value *> vals;

        for (const auto &arg : args)
        {
            keys.push_back(Builder.CreateGlobalStringPtr(arg.name));
            Value *v = exprHandler(arg.value.get());
            // --- FIX: Box raw value instead of converting to string ---
            vals.push_back(prepareValueForRuntime(v));
        }

        Value *keysList = structGen->createListFromValues(keys);
        Value *valsList = structGen->createListFromValues(vals);

        Function *func = TheModule->getFunction("String_format_map");
        if (!func)
        {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }
};