#pragma once
#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>
#include <iostream>
#include <map>
#include <vector>

using namespace llvm;

class BuiltinGen;

class StructGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;
    std::map<std::string, std::map<std::string, int>> fieldOffsets;

   public:
    StructGen(LLVMContext& ctx,
              Module* mod,
              IRBuilder<>& build,
              std::map<std::string, StructType*>& types)
        : Context(ctx), TheModule(mod), Builder(build), StructTypes(types) {}

    // Empty impl to break circular dependency
    void setBuiltinGen(BuiltinGen* bg) {}

    void registerStructLayout(const std::string& name,
                              const std::vector<std::string>& fieldNames) {
        int idx = 0;
        for (const auto& field : fieldNames) {
            fieldOffsets[name][field] = idx++;
        }
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        StructType* st =
            cast<StructType>(objPtr->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        if (fieldOffsets.find(structName) == fieldOffsets.end()) {
            std::cerr << "COMPILER ERROR: Struct '" << structName
                      << "' layout was never registered." << std::endl;
            exit(1);
        }
        if (fieldOffsets[structName].find(memberName) ==
            fieldOffsets[structName].end()) {
            std::cerr << "COMPILER ERROR: Field '" << memberName
                      << "' not found for struct '" << structName << "'"
                      << std::endl;
            exit(1);
        }

        int index = fieldOffsets[structName][memberName];
        return Builder.CreateStructGEP(st, objPtr, index);
    }

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        Value* ptr = getMemberPtr(objPtr, memberName);
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    // --- INTERNAL MEMORY GUARD ---
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

    Value* allocateAndInit(const std::string& structName,
                           std::vector<Value*>& args) {
        if (!StructTypes.count(structName))
            return nullptr;
        StructType* st = StructTypes[structName];
        DataLayout DL(TheModule);
        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context),
                                            DL.getTypeAllocSize(st));
        Function* mallocFunc = TheModule->getFunction("malloc");
        if (!mallocFunc)
            return nullptr;

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        addMemoryGuard(rawPtr, "Allocation failed for " + structName);

        Value* objPtr =
            Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));
        Function* initFunc = TheModule->getFunction(structName + "__init");
        if (initFunc) {
            std::vector<Value*> initArgs = {objPtr};
            // Argument Casting Logic
            for (size_t i = 0; i < args.size(); i++) {
                Value* argVal = args[i];
                if (i + 1 < initFunc->arg_size()) {
                    Type* expectedType =
                        initFunc->getFunctionType()->getParamType(i + 1);
                    if (argVal->getType() != expectedType) {
                        if (argVal->getType()->isIntegerTy() &&
                            expectedType->isPointerTy()) {
                            argVal =
                                Builder.CreateIntToPtr(argVal, expectedType);
                        } else if (argVal->getType()->isPointerTy() &&
                                   expectedType->isPointerTy()) {
                            argVal =
                                Builder.CreateBitCast(argVal, expectedType);
                        }
                    }
                }
                initArgs.push_back(argVal);
            }
            Builder.CreateCall(initFunc, initArgs);
        }
        return objPtr;
    }

    Value* generateConstructor(ConstructorNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        if (StructTypes.find(node->structName) == StructTypes.end()) {
            std::cerr << "[ERROR-SG] Struct '" << node->structName
                      << "' is undefined in StructTypes map." << std::endl;
            exit(1);
        }

        StructType* st = StructTypes[node->structName];
        DataLayout DL(TheModule);

        // 1. Alloc
        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context),
                                            DL.getTypeAllocSize(st));
        Function* mallocFunc = TheModule->getFunction("malloc");
        if (!mallocFunc) {
            std::cerr << "[ERROR-SG] 'malloc' function not found." << std::endl;
            exit(1);
        }
        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        addMemoryGuard(rawPtr, "Allocation failed for " + node->structName);

        Value* objPtr =
            Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        // 2. Args
        std::vector<Value*> initArgs;
        initArgs.push_back(objPtr);  // self

        for (const auto& arg : node->args) {
            Value* val = exprHandler(arg.value.get());
            if (!val) {
                std::cerr << "[ERROR-SG] Failed to generate code for argument: "
                          << arg.fieldName << std::endl;
                exit(1);
            }
            initArgs.push_back(val);
        }

        // 3. Init Call
        std::string initName = node->structName + "__init";

        if (Function* initFunc = TheModule->getFunction(initName)) {
            if (initFunc->arg_size() == initArgs.size()) {
                std::vector<Value*> finalArgs;
                for (size_t i = 0; i < initArgs.size(); i++) {
                    Value* argVal = initArgs[i];
                    Type* expectedType =
                        initFunc->getFunctionType()->getParamType(i);

                    if (argVal->getType() != expectedType) {
                        if (argVal->getType()->isIntegerTy() &&
                            expectedType->isPointerTy()) {
                            argVal =
                                Builder.CreateIntToPtr(argVal, expectedType);
                        } else if (argVal->getType()->isPointerTy() &&
                                   expectedType->isPointerTy()) {
                            argVal =
                                Builder.CreateBitCast(argVal, expectedType);
                        }
                    }
                    finalArgs.push_back(argVal);
                }
                Builder.CreateCall(initFunc, finalArgs);
            }
        }

        return objPtr;
    }

    Value* generateStrCall(Value* structPtr, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* strFunc = TheModule->getFunction(funcName);
        if (strFunc)
            return Builder.CreateCall(strFunc, {structPtr});
        return nullptr;
    }

    Value* generateReprCall(Value* structPtr, const std::string& structName) {
        std::string funcName = structName + "___repr";
        Function* reprFunc = TheModule->getFunction(funcName);
        if (reprFunc)
            return Builder.CreateCall(reprFunc, {structPtr});
        return nullptr;
    }

    // --- [ADDED] Missing method implementation ---
    Value* generateListLiteral(ListLiteralNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        int size = node->elements.size();
        Value* sizeVal = ConstantInt::get(Type::getInt32Ty(Context), size);
        std::vector<Value*> ctorArgs = {sizeVal};

        Value* listObj = allocateAndInit("List", ctorArgs);
        if (!listObj)
            return nullptr;

        StructType* listType = StructTypes["List"];

        // 1. Set Data (Index 0)
        Value* dataPtrPtr = Builder.CreateStructGEP(listType, listObj, 0);
        Value* dataPtr =
            Builder.CreateLoad(Type::getInt8PtrTy(Context), dataPtrPtr);
        Value* ptrDataPtr = Builder.CreateBitCast(
            dataPtr, PointerType::getUnqual(Type::getInt8PtrTy(Context)));

        for (int i = 0; i < size; ++i) {
            Value* val = exprHandler(node->elements[i].get());

            if (val->getType()->isIntegerTy()) {
                val = Builder.CreateIntToPtr(val, Type::getInt8PtrTy(Context));
            } else if (val->getType()->isPointerTy()) {
                if (val->getType() != Type::getInt8PtrTy(Context)) {
                    val =
                        Builder.CreateBitCast(val, Type::getInt8PtrTy(Context));
                }
            }
            Value* slotPtr = Builder.CreateConstGEP1_32(ptrDataPtr, i);
            Builder.CreateStore(val, slotPtr);
        }

        // 2. Set Length (Index 1)
        Value* lenPtr = Builder.CreateStructGEP(listType, listObj, 1);
        Builder.CreateStore(sizeVal, lenPtr);

        // 3. Set Capacity (Index 2)
        if (listType->getNumElements() > 2) {
            Value* capPtr = Builder.CreateStructGEP(listType, listObj, 2);
            Builder.CreateStore(sizeVal, capPtr);
        }

        return listObj;
    }

    Value* createListFromValues(std::vector<Value*>& values) {
        int size = values.size();
        Value* sizeVal = ConstantInt::get(Type::getInt32Ty(Context), size);
        std::vector<Value*> ctorArgs = {sizeVal};

        Value* listObj = allocateAndInit("List", ctorArgs);
        if (!listObj)
            return nullptr;

        StructType* listType = StructTypes["List"];

        // 1. Set Data (Index 0)
        Value* dataPtrPtr = Builder.CreateStructGEP(listType, listObj, 0);
        Value* dataPtr =
            Builder.CreateLoad(Type::getInt8PtrTy(Context), dataPtrPtr);
        Value* ptrDataPtr = Builder.CreateBitCast(
            dataPtr, PointerType::getUnqual(Type::getInt8PtrTy(Context)));

        for (int i = 0; i < size; ++i) {
            Value* val = values[i];
            if (val->getType()->isIntegerTy()) {
                val = Builder.CreateIntToPtr(val, Type::getInt8PtrTy(Context));
            } else if (val->getType() != Type::getInt8PtrTy(Context)) {
                val = Builder.CreateBitCast(val, Type::getInt8PtrTy(Context));
            }
            Value* slotPtr = Builder.CreateConstGEP1_32(ptrDataPtr, i);
            Builder.CreateStore(val, slotPtr);
        }

        // 2. Set Length
        Value* lenPtr = Builder.CreateStructGEP(listType, listObj, 1);
        Builder.CreateStore(sizeVal, lenPtr);

        // 3. Set Capacity
        if (listType->getNumElements() > 2) {
            Value* capPtr = Builder.CreateStructGEP(listType, listObj, 2);
            Builder.CreateStore(sizeVal, capPtr);
        }

        return listObj;
    }
};