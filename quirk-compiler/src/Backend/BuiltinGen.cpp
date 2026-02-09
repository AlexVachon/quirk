#include <iostream>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

// We assume StructGen is ALREADY DEFINED because Codegen.cpp includes it first.
class StructGen;

class BuiltinGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    StructGen* structGen;

   public:
    BuiltinGen(LLVMContext& ctx,
               Module* mod,
               IRBuilder<>& builder,
               StructGen* sg)
        : Context(ctx), TheModule(mod), Builder(builder), structGen(sg) {}

    void Initialize() {
        // Declare libc functions
        std::vector<Type*> printfArgs = {Type::getInt8PtrTy(Context)};
        FunctionType* printfType =
            FunctionType::get(Type::getInt32Ty(Context), printfArgs, true);
        Function::Create(printfType, Function::ExternalLinkage, "printf",
                         TheModule);

        FunctionType* mallocType = FunctionType::get(
            Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false);
        Function::Create(mallocType, Function::ExternalLinkage, "malloc",
                         TheModule);

        FunctionType* freeType = FunctionType::get(
            Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false);
        Function::Create(freeType, Function::ExternalLinkage, "free",
                         TheModule);

        FunctionType* exitType = FunctionType::get(
            Type::getVoidTy(Context), {Type::getInt32Ty(Context)}, false);
        Function::Create(exitType, Function::ExternalLinkage, "exit",
                         TheModule);
    }

    bool isBuiltin(const std::string& name) {
        return name == "print" || name == "printf" || name == "malloc" ||
               name == "free";
    }

    Value* handleBuiltin(const std::string& name,
                         CallNode* call,
                         std::function<Value*(Node*)> exprHandler) {
        if (name == "print")
            return generatePrint(call, exprHandler);
        if (name == "printf")
            return generatePrintf(call, exprHandler);
        return nullptr;
    }

    Value* generatePrintf(CallNode* call,
                          std::function<Value*(Node*)> exprHandler) {
        Function* func = TheModule->getFunction("printf");
        std::vector<Value*> args;
        for (auto& a : call->args) {
            args.push_back(exprHandler(a.value.get()));
        }
        return Builder.CreateCall(func, args);
    }

    // --- HELPER: Int to String ---
    Value* generateIntToString(Value* val) {
        // Use sprintf to convert int to char* buffer
        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(
            mallocFunc, {ConstantInt::get(Type::getInt64Ty(Context), 32)});

        Function* sprintfFunc = TheModule->getFunction("sprintf");
        if (!sprintfFunc) {
            std::vector<Type*> args = {Type::getInt8PtrTy(Context),
                                       Type::getInt8PtrTy(Context)};
            FunctionType* ft =
                FunctionType::get(Type::getInt32Ty(Context), args, true);
            sprintfFunc = Function::Create(ft, Function::ExternalLinkage,
                                           "sprintf", TheModule);
        }

        Value* fmt = Builder.CreateGlobalStringPtr("%d");
        Builder.CreateCall(sprintfFunc, {buffer, fmt, val});

        // Box into String struct
        std::vector<Value*> ctorArgs = {buffer};
        return structGen->allocateAndInit("String", ctorArgs);
    }

    // --- HELPER: Double to String ---
    Value* generateDoubleToString(Value* val) {
        Function* f2s = TheModule->getFunction("_float_to_str");
        if (!f2s) {
            FunctionType* ft =
                FunctionType::get(Type::getInt8PtrTy(Context),
                                  {Type::getDoubleTy(Context)}, false);
            f2s = Function::Create(ft, Function::ExternalLinkage,
                                   "_float_to_str", TheModule);
        }
        Value* rawStr = Builder.CreateCall(f2s, {val});
        std::vector<Value*> ctorArgs = {rawStr};
        return structGen->allocateAndInit("String", ctorArgs);
    }

    Value* generateStringConcat(Value* L, Value* R) {
        // Expecting L and R to be String* (Structs)
        Function* addFunc = TheModule->getFunction("String___add");
        if (addFunc)
            return Builder.CreateCall(addFunc, {L, R});
        return L;  // Fail gracefully
    }

    Value* generateStringCompare(Value* L, Value* R, std::string op) {
        Function* eqFunc = TheModule->getFunction("String___eq");
        if (!eqFunc)
            return ConstantInt::getFalse(Context);
        Value* res = Builder.CreateCall(eqFunc, {L, R});
        if (op == "!=")
            return Builder.CreateNot(res);
        return res;
    }

    Value* generatePrint(CallNode* call,
                         std::function<Value*(Node*)> exprHandler) {
        Function* printfFunc = TheModule->getFunction("printf");
        Value* val = exprHandler(call->args[0].value.get());

        // 1. Handle Strings (Struct String)
        if (val->getType()->isPointerTy() &&
            val->getType()->getPointerElementType()->isStructTy()) {
            StructType* st =
                cast<StructType>(val->getType()->getPointerElementType());
            std::string structName = st->getName().str();

            if (structName == "String") {
                // Call .__str() (which returns cstring)
                Value* strVal = structGen->generateStrCall(val, structName);
                if (strVal) {
                    Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                    return Builder.CreateCall(printfFunc, {fmt, strVal});
                }
            } else {
                // Generic Struct: Call __str if exists
                Value* strObj = structGen->generateStrCall(val, structName);
                if (strObj) {
                    // Unwrap if it returns a String object
                    if (strObj->getType()->isPointerTy() &&
                        strObj->getType()
                            ->getPointerElementType()
                            ->isStructTy()) {
                        // String struct: [len, buffer]
                        Value* bufferPtr = Builder.CreateStructGEP(
                            cast<StructType>(
                                strObj->getType()->getPointerElementType()),
                            strObj, 1);
                        Value* cStr = Builder.CreateLoad(
                            Type::getInt8PtrTy(Context), bufferPtr);
                        Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                        return Builder.CreateCall(printfFunc, {fmt, cStr});
                    }
                }
            }
        }

        // 2. Handle Int
        if (val->getType()->isIntegerTy()) {
            Value* fmt = Builder.CreateGlobalStringPtr("%d\n");
            return Builder.CreateCall(printfFunc, {fmt, val});
        }

        // 3. Handle Double
        if (val->getType()->isDoubleTy()) {
            Value* fmt = Builder.CreateGlobalStringPtr("%f\n");
            return Builder.CreateCall(printfFunc, {fmt, val});
        }

        // 4. Handle C-String (i8*)
        if (val->getType()->isPointerTy() &&
            val->getType()->getPointerElementType()->isIntegerTy(8)) {
            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
            return Builder.CreateCall(printfFunc, {fmt, val});
        }

        return nullptr;
    }
};