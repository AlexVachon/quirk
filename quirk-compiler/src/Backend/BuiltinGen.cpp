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
        // --- libc functions ---

        // printf(i8*, ...) -> i32
        std::vector<Type*> printfArgs = {Type::getInt8PtrTy(Context)};
        FunctionType* printfType =
            FunctionType::get(Type::getInt32Ty(Context), printfArgs, true);
        Function::Create(printfType, Function::ExternalLinkage, "printf",
                         TheModule);

        // sprintf(i8*, i8*, ...) -> i32
        // FIX: We declare this explicitly so 'sys.qk' doesn't override it with
        // wrong types
        std::vector<Type*> sprintfArgs = {Type::getInt8PtrTy(Context),
                                          Type::getInt8PtrTy(Context)};
        FunctionType* sprintfType =
            FunctionType::get(Type::getInt32Ty(Context), sprintfArgs, true);
        Function::Create(sprintfType, Function::ExternalLinkage, "sprintf",
                         TheModule);

        // malloc(i64) -> i8*
        FunctionType* mallocType = FunctionType::get(
            Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false);
        Function::Create(mallocType, Function::ExternalLinkage, "malloc",
                         TheModule);

        // free(i8*) -> void
        FunctionType* freeType = FunctionType::get(
            Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false);
        Function::Create(freeType, Function::ExternalLinkage, "free",
                         TheModule);

        // exit(i32) -> void
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
        if (!func)
            return nullptr;

        std::vector<Value*> args;
        for (auto& a : call->args) {
            args.push_back(exprHandler(a.value.get()));
        }
        return Builder.CreateCall(func, args);
    }

    // --- HELPER: Int to String ---
    // --- HELPER: Int to String ---
    Value* generateIntToString(Value* val) {
        // 1. Handle Boolean (i1) -> Call Bool_str
        if (val->getType()->isIntegerTy(1)) {
            Function* f = TheModule->getFunction("Bool_str");
            if (f)
                return Builder.CreateCall(f, {val});
        }

        // 2. Handle Int (Ensure i32)
        // Int_str expects i32. If we have i8, i16, or i64, cast it.
        if (val->getType()->getIntegerBitWidth() != 32) {
            val = Builder.CreateIntCast(val, Type::getInt32Ty(Context), true);
        }

        // 3. Delegate to C Runtime 'Int_str'
        Function* f = TheModule->getFunction("Int_str");
        if (f) {
            return Builder.CreateCall(f, {val});
        }

        // Fallback (Should not happen if primitives.c is linked)
        std::cerr << "[Warning] Int_str not found, falling back to manual "
                     "sprintf generation."
                  << std::endl;

        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(
            mallocFunc, {ConstantInt::get(Type::getInt64Ty(Context), 32)});

        Function* sprintfFunc = TheModule->getFunction("sprintf");
        Value* fmt = Builder.CreateGlobalStringPtr("%d");
        Builder.CreateCall(sprintfFunc, {buffer, fmt, val});

        std::vector<Value*> ctorArgs = {buffer};
        return structGen->allocateAndInit("String", ctorArgs);
    }

    // --- HELPER: Double to String ---
    Value* generateDoubleToString(Value* val) {
        // Optimized: Delegate to C Runtime 'Double_str'
        Function* f = TheModule->getFunction("Double_str");
        if (f) {
            return Builder.CreateCall(f, {val});
        }

        // Fallback to old '_float_to_str'
        Function* f2s = TheModule->getFunction("_float_to_str");
        if (f2s) {
            Value* rawStr = Builder.CreateCall(f2s, {val});
            std::vector<Value*> ctorArgs = {rawStr};
            return structGen->allocateAndInit("String", ctorArgs);
        }
        return nullptr;
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
        if (!val)
            return nullptr;

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
                        // We want index 1 (buffer)
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