#pragma once
#include <iostream>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class StructGen;

class BuiltinGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    StructGen* structGen;

   public:
    BuiltinGen(LLVMContext& ctx, Module* mod, IRBuilder<>& builder, StructGen* sg)
        : Context(ctx), TheModule(mod), Builder(builder), structGen(sg) {}

    void setStructGen(StructGen* sg) { structGen = sg; }

    void Initialize() {
        std::vector<Type*> printfArgs = {Type::getInt8PtrTy(Context)};
        FunctionType* printfType = FunctionType::get(Type::getInt32Ty(Context), printfArgs, true);
        Function::Create(printfType, Function::ExternalLinkage, "printf", TheModule);

        std::vector<Type*> sprintfArgs = {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)};
        FunctionType* sprintfType = FunctionType::get(Type::getInt32Ty(Context), sprintfArgs, true);
        Function::Create(sprintfType, Function::ExternalLinkage, "sprintf", TheModule);

        FunctionType* mallocType = FunctionType::get(
            Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false);
        Function::Create(mallocType, Function::ExternalLinkage, "malloc", TheModule);

        FunctionType* freeType = FunctionType::get(
            Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false);
        Function::Create(freeType, Function::ExternalLinkage, "free", TheModule);

        FunctionType* exitType = FunctionType::get(
            Type::getVoidTy(Context), {Type::getInt32Ty(Context)}, false);
        Function::Create(exitType, Function::ExternalLinkage, "exit", TheModule);

        Type* voidPtrTy = Type::getInt8PtrTy(Context);
        Type* i32Ty = Type::getInt32Ty(Context);
        Type* voidTy = Type::getVoidTy(Context);

        Function::Create(FunctionType::get(voidPtrTy, false), Function::ExternalLinkage, "quirk_get_jmp_buf", TheModule);
        Function::Create(FunctionType::get(voidTy, false), Function::ExternalLinkage, "quirk_pop_try", TheModule);
        Function::Create(FunctionType::get(voidTy, {voidPtrTy}, false), Function::ExternalLinkage, "quirk_set_exception", TheModule);
        Function::Create(FunctionType::get(voidPtrTy, false), Function::ExternalLinkage, "quirk_get_exception", TheModule);
        Function::Create(FunctionType::get(i32Ty, false), Function::ExternalLinkage, "quirk_get_try_depth", TheModule);
        Function::Create(FunctionType::get(voidPtrTy, false), Function::ExternalLinkage, "quirk_get_current_jmp_buf", TheModule);
        Function::Create(FunctionType::get(voidTy, false), Function::ExternalLinkage, "quirk_unhandled_exception", TheModule);

        Function* sj = Function::Create(FunctionType::get(i32Ty, {voidPtrTy}, false), Function::ExternalLinkage, "_setjmp", TheModule);
        sj->addFnAttr(Attribute::ReturnsTwice);
        Function::Create(FunctionType::get(voidTy, {voidPtrTy, i32Ty}, false), Function::ExternalLinkage, "longjmp", TheModule);

        auto anyPtrTy = Type::getInt8PtrTy(Context);
        Function::Create(FunctionType::get(anyPtrTy, {i32Ty},     false), Function::ExternalLinkage, "Core_Primitives_Any_box_int",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {Type::getDoubleTy(Context)}, false), Function::ExternalLinkage, "Core_Primitives_Any_box_double", TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {i32Ty},     false), Function::ExternalLinkage, "Core_Primitives_Any_box_bool",   TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {i32Ty},     false), Function::ExternalLinkage, "Core_Primitives_Any_box_char",   TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_string", TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_list",   TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_map",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_ptr",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {},           false), Function::ExternalLinkage, "Core_Primitives_Any_box_null",   TheModule);

        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_string", TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_str",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any___str",     TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_get_type",  TheModule);
        Function::Create(FunctionType::get(i32Ty,    {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_int",    TheModule);
        Function::Create(FunctionType::get(Type::getDoubleTy(Context), {anyPtrTy}, false), Function::ExternalLinkage, "Core_Primitives_Any_to_float", TheModule);
        Function::Create(FunctionType::get(i32Ty,    {anyPtrTy, anyPtrTy}, false), Function::ExternalLinkage, "Core_Primitives_Any_isinstance", TheModule);
    }

    bool isBuiltin(const std::string& name) {
        return name == "print" || name == "printf" || name == "malloc" || name == "free";
    }

    Value* handleBuiltin(const std::string& name, CallNode* call,
                         std::function<Value*(Node*)> exprHandler) {
        if (name == "print")   return generatePrint(call, exprHandler);
        if (name == "printf")  return generatePrintf(call, exprHandler);
        return nullptr;
    }

    Value* generatePrintf(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        Function* func = TheModule->getFunction("printf");
        if (!func) return nullptr;
        std::vector<Value*> args;
        for (auto& a : call->args) args.push_back(exprHandler(a.value.get()));
        return Builder.CreateCall(func, args);
    }

    Value* generateIntToString(Value* val) {
        if (val->getType()->isIntegerTy(1)) {
            Value* ext = Builder.CreateZExt(val, Type::getInt32Ty(Context));
            Function* f = TheModule->getFunction("Core_Primitives_Bool_str");
            if (f) return Builder.CreateCall(f, {ext});
        }
        if (val->getType()->getIntegerBitWidth() != 32)
            val = Builder.CreateIntCast(val, Type::getInt32Ty(Context), true);

        Function* f = TheModule->getFunction("Core_Primitives_Int_str");
        if (f) return Builder.CreateCall(f, {val});

        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(mallocFunc, {ConstantInt::get(Type::getInt64Ty(Context), 32)});
        Function* sprintfFunc = TheModule->getFunction("sprintf");
        Value* fmt = Builder.CreateGlobalStringPtr("%d");
        Builder.CreateCall(sprintfFunc, {buffer, fmt, val});
        std::vector<Value*> ctorArgs = {buffer};
        return structGen->allocateAndInit("String", ctorArgs);
    }

    Value* generateDoubleToString(Value* val) {
        Function* f = TheModule->getFunction("Core_Primitives_Double_str");
        if (f) return Builder.CreateCall(f, {val});

        Function* f2s = TheModule->getFunction("Core_String_float_to_str");
        if (f2s) {
            Value* rawStr = Builder.CreateCall(f2s, {val});
            std::vector<Value*> ctorArgs = {rawStr};
            return structGen->allocateAndInit("String", ctorArgs);
        }
        return nullptr;
    }

    Value* generateStringConcat(Value* L, Value* R) {
        Function* addFunc = TheModule->getFunction("Core_String_String___add");
        if (addFunc) return Builder.CreateCall(addFunc, {L, R});
        return L;
    }

    Value* generateStringCompare(Value* L, Value* R, std::string op) {
        Function* eqFunc = TheModule->getFunction("Core_String_String___eq");
        if (!eqFunc) return ConstantInt::getFalse(Context);
        Value* res = Builder.CreateCall(eqFunc, {L, R});
        if (op == "!=") return Builder.CreateNot(res);
        return res;
    }

    Value* generatePrint(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        Function* printfFunc = TheModule->getFunction("printf");

        for (auto& arg : call->args) {
            Value* val = exprHandler(arg.value.get());
            if (!val) continue;

            Type* type = val->getType();

            if (type->isPointerTy() && type->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(type->getPointerElementType());
                std::string structName = st->getName().str();
                if (structName.find("struct.") == 0) structName = structName.substr(7);
                size_t dotPos = structName.find('.');
                if (dotPos != std::string::npos && std::isdigit(structName[dotPos+1]))
                    structName = structName.substr(0, dotPos);

                if (structName == "String") {
                    Value* bufPtr = structGen->getMemberPtr(val, "buffer");
                    if (bufPtr) {
                        Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                        Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                        Builder.CreateCall(printfFunc, {fmt, cStr});
                    }
                } else if (structName == "Any") {
                    Function* anyStr = TheModule->getFunction("Core_Primitives_Any_to_string");
                    if (anyStr) {
                        Value* strObj = Builder.CreateCall(anyStr, {val});
                        Value* bufPtr = structGen->getMemberPtr(strObj, "buffer");
                        if (bufPtr) {
                            Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                            Builder.CreateCall(printfFunc, {fmt, cStr});
                        }
                    }
                } else {
                    Value* strObj = structGen->generateStrCall(val, structName);
                    if (strObj) {
                        if (strObj->getType()->isPointerTy() &&
                            strObj->getType()->getPointerElementType()->isStructTy()) {
                            Value* bufPtr = structGen->getMemberPtr(strObj, "buffer");
                            if (bufPtr) {
                                Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                                Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                                Builder.CreateCall(printfFunc, {fmt, cStr});
                            }
                        } else if (strObj->getType()->isPointerTy() &&
                                   strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                            Builder.CreateCall(printfFunc, {fmt, strObj});
                        }
                    } else {
                        Value* fmt = Builder.CreateGlobalStringPtr("<%s at %p>\n");
                        Value* nameVal = Builder.CreateGlobalStringPtr(structName);
                        Builder.CreateCall(printfFunc, {fmt, nameVal, val});
                    }
                }
            } else if (type->isIntegerTy()) {
                if (type->getIntegerBitWidth() == 1) {
                    Function* boolStrFunc = TheModule->getFunction("Core_Primitives_Bool_str");
                    if (boolStrFunc) {
                        Value* ext = Builder.CreateZExt(val, Type::getInt32Ty(Context));
                        Value* strObj = Builder.CreateCall(boolStrFunc, {ext});
                        Value* bufPtr = structGen->getMemberPtr(strObj, "buffer");
                        if (bufPtr) {
                            Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                            Builder.CreateCall(printfFunc, {fmt, cStr});
                        }
                    }
                } else if (type->getIntegerBitWidth() == 8) {
                    Value* fmt = Builder.CreateGlobalStringPtr("%c\n");
                    Builder.CreateCall(printfFunc, {fmt, val});
                } else {
                    Value* fmt = Builder.CreateGlobalStringPtr("%d\n");
                    Builder.CreateCall(printfFunc, {fmt, val});
                }
            } else if (type->isDoubleTy()) {
                Value* fmt = Builder.CreateGlobalStringPtr("%f\n");
                Builder.CreateCall(printfFunc, {fmt, val});
            } else if (type->isPointerTy() && type->getPointerElementType()->isIntegerTy(8)) {
                // i8* may be a boxed String* (from map.get / Any-typed returns).
                // quirk_print_opaque distinguishes heap String* from tagged-int pointers.
                Function* printOpaque = TheModule->getFunction("quirk_print_opaque");
                if (!printOpaque) {
                    FunctionType* ft = FunctionType::get(
                        Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false);
                    printOpaque = Function::Create(ft, Function::ExternalLinkage,
                        "quirk_print_opaque", TheModule);
                }
                Builder.CreateCall(printOpaque, {val});
            }
        }

        return ConstantInt::get(Type::getInt32Ty(Context), 0);
    }
};
