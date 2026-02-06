#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>
#include <string>
#include <iostream>

// Forward declaration
class StructGen;

using namespace llvm;

class BuiltinGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    StructGen* structGen;

   public:
    BuiltinGen(LLVMContext& ctx, Module* mod, IRBuilder<>& build, StructGen* sg)
        : Context(ctx), TheModule(mod), Builder(build), structGen(sg) {}

    void Initialize() {
        // printf
        FunctionType* printfType = FunctionType::get(
            Type::getInt32Ty(Context), {Type::getInt8PtrTy(Context)}, true);
        if (!TheModule->getFunction("printf")) {
            Function::Create(printfType, Function::ExternalLinkage, "printf",
                             TheModule);
        }

        // malloc
        if (!TheModule->getFunction("malloc")) {
            Function::Create(
                FunctionType::get(Type::getInt8PtrTy(Context),
                                  {Type::getInt64Ty(Context)}, false),
                Function::ExternalLinkage, "malloc", TheModule);
        }

        // free
        if (!TheModule->getFunction("free")) {
            Function::Create(
                FunctionType::get(Type::getVoidTy(Context),
                                  {Type::getInt8PtrTy(Context)}, false),
                Function::ExternalLinkage, "free", TheModule);
        }

        // sprintf
        if (!TheModule->getFunction("sprintf")) {
            FunctionType* sprintfTy = FunctionType::get(
                Type::getInt32Ty(Context),
                {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)},
                true);
            Function::Create(sprintfTy, Function::ExternalLinkage, "sprintf",
                             TheModule);
        }

        // Common String Utils
        if (!TheModule->getFunction("strlen")) {
            Function::Create(
                FunctionType::get(Type::getInt64Ty(Context),
                                  {Type::getInt8PtrTy(Context)}, false),
                Function::ExternalLinkage, "strlen", TheModule);
        }
        if (!TheModule->getFunction("strcpy")) {
            Function::Create(FunctionType::get(Type::getInt8PtrTy(Context),
                                               {Type::getInt8PtrTy(Context),
                                                Type::getInt8PtrTy(Context)},
                                               false),
                             Function::ExternalLinkage, "strcpy", TheModule);
        }
        if (!TheModule->getFunction("strcat")) {
            Function::Create(FunctionType::get(Type::getInt8PtrTy(Context),
                                               {Type::getInt8PtrTy(Context),
                                                Type::getInt8PtrTy(Context)},
                                               false),
                             Function::ExternalLinkage, "strcat", TheModule);
        }
    }

    bool isBuiltin(const std::string& name) {
        return name == "print" || name == "char_at" || name == "set_char_at";
    }

    Value* handleBuiltin(const std::string& name,
                         CallNode* call,
                         std::function<Value*(Node*)> exprHandler) {
        if (name == "print")
            return generatePrint(call, exprHandler);
        if (name == "char_at")
            return generateCharAt(call, exprHandler);
        if (name == "set_char_at")
            return generateSetCharAt(call, exprHandler);
        return nullptr;
    }

    void addMemoryGuard(Value* ptr, const std::string& errorMsg) {
        if (!ptr)
            return;

        Function* parentFunc = Builder.GetInsertBlock()->getParent();
        BasicBlock* panicBB =
            BasicBlock::Create(Context, "mem_panic", parentFunc);
        BasicBlock* contBB = BasicBlock::Create(Context, "mem_ok", parentFunc);

        Builder.CreateCondBr(Builder.CreateIsNull(ptr), panicBB, contBB);
        Builder.SetInsertPoint(panicBB);

        Function* printfFunc = TheModule->getFunction("printf");
        if (printfFunc) {
            Value* msg = Builder.CreateGlobalStringPtr(
                "FATAL ERROR: " + errorMsg + "\n");
            Builder.CreateCall(printfFunc, {msg});
        }

        FunctionCallee exitFunc = TheModule->getOrInsertFunction(
            "exit", FunctionType::get(Type::getVoidTy(Context),
                                      {Type::getInt32Ty(Context)}, false));
        Builder.CreateCall(exitFunc,
                           {ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(contBB);
    }

    // Public helpers for String Ops
    Value* generateStringConcat(Value* lhs, Value* rhs) {
        Function* strlenFunc = TheModule->getFunction("strlen");
        Function* mallocFunc = TheModule->getFunction("malloc");
        Function* strcpyFunc = TheModule->getFunction("strcpy");
        Function* strcatFunc = TheModule->getFunction("strcat");

        Value* len1 = Builder.CreateCall(strlenFunc, {lhs}, "len1");
        Value* len2 = Builder.CreateCall(strlenFunc, {rhs}, "len2");
        Value* totalLen = Builder.CreateAdd(len1, len2, "totalLen");
        Value* one = ConstantInt::get(Type::getInt64Ty(Context), 1);

        Value* allocSize = Builder.CreateAdd(totalLen, one);
        Value* newStrRaw =
            Builder.CreateCall(mallocFunc, {allocSize}, "newStrRaw");
        Value* newStr = Builder.CreateBitCast(
            newStrRaw, Type::getInt8PtrTy(Context), "newStr");

        Builder.CreateCall(strcpyFunc, {newStr, lhs});
        Builder.CreateCall(strcatFunc, {newStr, rhs});
        return newStr;
    }

    Value* generateStringCompare(Value* lhs, Value* rhs, std::string op) {
        Function* strcmpFunc = TheModule->getFunction("strcmp");
        if (!strcmpFunc) {
            strcmpFunc = Function::Create(
                FunctionType::get(
                    Type::getInt32Ty(Context),
                    {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)},
                    false),
                Function::ExternalLinkage, "strcmp", TheModule);
        }

        Value* diff = Builder.CreateCall(strcmpFunc, {lhs, rhs}, "diff");
        Value* zero = ConstantInt::get(Type::getInt32Ty(Context), 0);
        if (op == "==")
            return Builder.CreateICmpEQ(diff, zero, "streq");
        if (op == "!=")
            return Builder.CreateICmpNE(diff, zero, "strneq");
        return ConstantInt::getFalse(Context);
    }

    Value* generateIntToString(Value* intVal) {
        Function* mallocFunc = TheModule->getFunction("malloc");
        Function* sprintfFunc = TheModule->getFunction("sprintf");

        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context), 32);
        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* formatStr = Builder.CreateGlobalStringPtr("%d");

        Builder.CreateCall(sprintfFunc, {rawPtr, formatStr, intVal});
        return rawPtr;
    }

    Value* generateDoubleToString(Value* doubleVal) {
        Function* mallocFunc = TheModule->getFunction("malloc");
        Function* sprintfFunc = TheModule->getFunction("sprintf");

        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context), 64);
        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* formatStr = Builder.CreateGlobalStringPtr("%f");

        Builder.CreateCall(sprintfFunc, {rawPtr, formatStr, doubleVal});
        return rawPtr;
    }

   private:
    Value* generatePrint(CallNode* node,
                         std::function<Value*(Node*)> exprHandler) {
        // ---- [DEBUG LOG START] ----
        std::cerr << "[CG] Generating Print for " << node->args.size()
                  << " arguments." << std::endl;
        // ---- [DEBUG LOG END] ----

        Function* printfFunc = TheModule->getFunction("printf");
        if (!printfFunc) {
            std::cerr << "[CG-ERROR] printf function not found!" << std::endl;
            return nullptr;
        }

        Value* formatInt = Builder.CreateGlobalStringPtr("%d\n");
        Value* formatStr = Builder.CreateGlobalStringPtr("%s\n");
        Value* formatFloat = Builder.CreateGlobalStringPtr("%f\n");
        Value* formatChar = Builder.CreateGlobalStringPtr("%c\n");

        for (auto& argStruct : node->args) {
            Value* val = exprHandler(argStruct.value.get());
            if (!val) {
                std::cerr << "  [CG] Skipping null arg" << std::endl;
                continue;
            }

            Type* ty = val->getType();
            // ---- [DEBUG LOG START] ----
            std::cerr << "  [CG] Value Type ID: " << ty->getTypeID()
                      << std::endl;
            if (ty->isPointerTy()) {
                std::cerr << "  [CG] Is Pointer. Element Type: "
                          << ty->getPointerElementType()->getTypeID()
                          << std::endl;
            }
            // ---- [DEBUG LOG END] ----

            if (ty->isIntegerTy(8)) {
                Value* charAsInt =
                    Builder.CreateZExt(val, Type::getInt32Ty(Context));
                Builder.CreateCall(printfFunc, {formatChar, charAsInt});
            } else if (ty->isIntegerTy(1)) {
                Value* trueStr = Builder.CreateGlobalStringPtr("true");
                Value* falseStr = Builder.CreateGlobalStringPtr("false");
                Value* strPtr = Builder.CreateSelect(val, trueStr, falseStr);
                Builder.CreateCall(printfFunc, {formatStr, strPtr});
            } else if (ty->isPointerTy() &&
                       ty->getPointerElementType()->isIntegerTy(8)) {
                // cstring
                Builder.CreateCall(printfFunc, {formatStr, val});
            } else if (ty->isDoubleTy()) {
                Builder.CreateCall(printfFunc, {formatFloat, val});
            } else if (ty->isPointerTy() &&
                       ty->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(ty->getPointerElementType());
                std::string structName = st->getName().str();

                std::cerr << "  [CG] Handling Struct: " << structName
                          << std::endl;

                Value* strVal = nullptr;
                if (structGen) {
                    strVal = structGen->generateStrCall(val, structName);
                    if (!strVal) {
                        strVal = structGen->generateReprCall(val, structName);
                    }
                }

                if (strVal) {
                    // --- FIX: Unwrap String struct if __str/__repr returns an
                    // object ---
                    if (strVal->getType()->isPointerTy() &&
                        strVal->getType()
                            ->getPointerElementType()
                            ->isStructTy()) {
                        StructType* retSt = cast<StructType>(
                            strVal->getType()->getPointerElementType());
                        if (retSt->getName().str() == "String") {
                            // Extract buffer (index 0) from the String struct
                            Value* bufferPtr =
                                Builder.CreateStructGEP(retSt, strVal, 0);
                            strVal = Builder.CreateLoad(
                                Type::getInt8PtrTy(Context), bufferPtr);
                        }
                    }
                    // -------------------------------------------------------------------

                    Builder.CreateCall(printfFunc, {formatStr, strVal});
                } else {
                    Value* addr =
                        Builder.CreatePtrToInt(val, Type::getInt64Ty(Context));
                    Value* fmt = Builder.CreateGlobalStringPtr(
                        "<" + structName + " instance at %p>\n");
                    Builder.CreateCall(printfFunc, {fmt, addr});
                }
            } else {
                Builder.CreateCall(printfFunc, {formatInt, val});
            }
        }
        return nullptr;
    }

    Value* generateCharAt(CallNode* call,
                          std::function<Value*(Node*)> exprHandler) {
        if (call->args.size() < 2)
            return nullptr;

        Value* ptr = exprHandler(call->args[0].value.get());
        Value* idx = exprHandler(call->args[1].value.get());

        if (!ptr || !idx)
            return nullptr;

        if (ptr->getType()->isIntegerTy()) {
            ptr = Builder.CreateIntToPtr(ptr, Type::getInt8PtrTy(Context));
        } else {
            ptr = Builder.CreateBitCast(ptr, Type::getInt8PtrTy(Context));
        }

        Value* bytePtr = Builder.CreateGEP(Type::getInt8Ty(Context), ptr, idx);
        Value* byteVal = Builder.CreateLoad(Type::getInt8Ty(Context), bytePtr);
        return Builder.CreateZExt(byteVal, Type::getInt32Ty(Context));
    }

    Value* generateSetCharAt(CallNode* call,
                             std::function<Value*(Node*)> exprHandler) {
        if (call->args.size() < 3)
            return nullptr;

        Value* ptr = exprHandler(call->args[0].value.get());
        Value* idx = exprHandler(call->args[1].value.get());
        Value* val = exprHandler(call->args[2].value.get());

        if (!ptr || !idx || !val)
            return nullptr;

        ptr = Builder.CreateBitCast(ptr, Type::getInt8PtrTy(Context));

        if (val->getType()->isIntegerTy(32) ||
            val->getType()->isIntegerTy(64)) {
            val = Builder.CreateTrunc(val, Type::getInt8Ty(Context));
        }

        Value* bytePtr = Builder.CreateGEP(Type::getInt8Ty(Context), ptr, idx);
        Builder.CreateStore(val, bytePtr);

        return nullptr;
    }
};