#include <iostream>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

// We assume StructGen is defined because Codegen.cpp includes StructGen.cpp BEFORE BuiltinGen.cpp
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

    void setStructGen(StructGen* sg) { structGen = sg; }

    void Initialize() {
        // --- libc functions ---

        // printf(i8*, ...) -> i32
        std::vector<Type*> printfArgs = {Type::getInt8PtrTy(Context)};
        FunctionType* printfType =
            FunctionType::get(Type::getInt32Ty(Context), printfArgs, true);
        Function::Create(printfType, Function::ExternalLinkage, "printf",
                         TheModule);

        // sprintf(i8*, i8*, ...) -> i32
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
    Value* generateIntToString(Value* val) {
        // 1. Handle Boolean
        if (val->getType()->isIntegerTy(1)) {
            Function* f = TheModule->getFunction("Bool_str");
            if (f)
                return Builder.CreateCall(f, {val});
        }

        // 2. Ensure i32 for C calls
        if (val->getType()->getIntegerBitWidth() != 32) {
            val = Builder.CreateIntCast(val, Type::getInt32Ty(Context), true);
        }

        // 3. Delegate to C Runtime
        Function* f = TheModule->getFunction("Int_str");
        if (f) {
            return Builder.CreateCall(f, {val});
        }

        // Fallback: Manual sprintf
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
        Function* f = TheModule->getFunction("Double_str");
        if (f) {
            return Builder.CreateCall(f, {val});
        }

        Function* f2s = TheModule->getFunction("_float_to_str");
        if (f2s) {
            Value* rawStr = Builder.CreateCall(f2s, {val});
            std::vector<Value*> ctorArgs = {rawStr};
            return structGen->allocateAndInit("String", ctorArgs);
        }
        return nullptr;
    }

    Value* generateStringConcat(Value* L, Value* R) {
        Function* addFunc = TheModule->getFunction("String___add");
        if (addFunc)
            return Builder.CreateCall(addFunc, {L, R});
        return L;
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

        // Loop through ALL arguments
        for (auto& arg : call->args) {
            Value* val = exprHandler(arg.value.get());
            if (!val) continue;
            
            Type* type = val->getType();

            // 1. Strings / Structs
            if (type->isPointerTy() && type->getPointerElementType()->isStructTy()) {
                StructType* st =
                    cast<StructType>(type->getPointerElementType());
                std::string structName = st->getName().str();

                // Clean name (remove "struct." or suffixes)
                if (structName.find("struct.") == 0) 
                    structName = structName.substr(7);
                size_t dotPos = structName.find('.');
                if (dotPos != std::string::npos && std::isdigit(structName[dotPos+1])) {
                     structName = structName.substr(0, dotPos);
                }

                if (structName == "String") {
                    // String Object: Unwrap 'buffer'
                    Value* bufPtr = structGen->getMemberPtr(val, "buffer");
                    if (bufPtr) {
                        Value* cStr = Builder.CreateLoad(
                            Type::getInt8PtrTy(Context), bufPtr);
                        Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                        Builder.CreateCall(printfFunc, {fmt, cStr});
                    }
                } else {
                    // Generic Struct: Call __str()
                    // This relies on StructGen::generateStrCall (added in previous step)
                    Value* strObj = structGen->generateStrCall(val, structName);
                    
                    if (strObj) {
                        // Check if __str returned a String Object or cstring
                        if (strObj->getType()->isPointerTy() &&
                            strObj->getType()->getPointerElementType()->isStructTy()) {
                            
                            // It returned a String Object -> Unwrap buffer
                            Value* bufPtr = structGen->getMemberPtr(strObj, "buffer");
                            if (bufPtr) {
                                Value* cStr = Builder.CreateLoad(
                                    Type::getInt8PtrTy(Context), bufPtr);
                                Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                                Builder.CreateCall(printfFunc, {fmt, cStr});
                            }
                        } 
                        else if (strObj->getType()->isPointerTy() && 
                                 strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                            // It returned a raw cstring -> Print directly
                            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                            Builder.CreateCall(printfFunc, {fmt, strObj});
                        }
                    } else {
                         // Fallback: Print Address if no __str
                         Value* fmt = Builder.CreateGlobalStringPtr("<%s at %p>\n");
                         Value* nameVal = Builder.CreateGlobalStringPtr(structName);
                         Builder.CreateCall(printfFunc, {fmt, nameVal, val});
                    }
                }
            }
            // 2. Int or Bool (Both are IntegerTy in LLVM)
            else if (type->isIntegerTy()) {
                // Check if it's a 1-bit boolean
                if (type->getIntegerBitWidth() == 1) {
                    // Call Bool_str which returns a String*
                    Function* boolStrFunc = TheModule->getFunction("Bool_str");
                    if (boolStrFunc) {
                        Value* strObj = Builder.CreateCall(boolStrFunc, {val});
                        
                        // Extract cstring from String Object and print
                        Value* bufPtr = structGen->getMemberPtr(strObj, "buffer");
                        if (bufPtr) {
                            Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                            Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                            Builder.CreateCall(printfFunc, {fmt, cStr});
                        }
                    }
                } 
                // Regular 32-bit integer
                else {
                    Value* fmt = Builder.CreateGlobalStringPtr("%d\n");
                    Builder.CreateCall(printfFunc, {fmt, val});
                }
            }
            // 3. Double
            else if (type->isDoubleTy()) {
                Value* fmt = Builder.CreateGlobalStringPtr("%f\n");
                Builder.CreateCall(printfFunc, {fmt, val});
            }
            // 4. Raw C-String
            else if (type->isPointerTy() &&
                     type->getPointerElementType()->isIntegerTy(8)) {
                Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                Builder.CreateCall(printfFunc, {fmt, val});
            }
        }
        
        return ConstantInt::get(Type::getInt32Ty(Context), 0);
    }
};