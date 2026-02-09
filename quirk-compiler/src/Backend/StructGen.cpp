#include <iostream>
#include <map>
#include <string>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class BuiltinGen;  // Forward Declaration

class StructGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;
    BuiltinGen* builtinGen;

   public:
    StructGen(LLVMContext& ctx,
              Module* mod,
              IRBuilder<>& builder,
              std::map<std::string, StructType*>& structs)
        : Context(ctx),
          TheModule(mod),
          Builder(builder),
          StructTypes(structs),
          builtinGen(nullptr) {}

    void setBuiltinGen(BuiltinGen* bg) { builtinGen = bg; }

    void registerStructLayout(const std::string& name,
                              const std::vector<std::string>& fields) {
        structLayouts[name] = fields;
    }

    Value* allocateAndInit(const std::string& name, std::vector<Value*>& args) {
        if (!StructTypes.count(name)) {
            std::cerr << "Error: Unknown struct " << name << std::endl;
            return nullptr;
        }

        StructType* st = StructTypes[name];
        Value* allocSize = ConstantExpr::getSizeOf(st);

        Function* mallocFunc = TheModule->getFunction("malloc");
        if (!mallocFunc) {
            std::cerr << "Error: malloc not found" << std::endl;
            return nullptr;
        }

        if (allocSize->getType()->getIntegerBitWidth() < 64)
            allocSize =
                Builder.CreateZExt(allocSize, Type::getInt64Ty(Context));

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* objPtr =
            Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        std::string initName = name + "__init";
        Function* initFunc = TheModule->getFunction(initName);

        if (initFunc) {
            std::vector<Value*> initArgs;
            initArgs.push_back(objPtr);
            for (auto val : args)
                initArgs.push_back(val);
            Builder.CreateCall(initFunc, initArgs);
        } else {
            int idx = 0;
            for (auto val : args) {
                if (idx >= (int)st->getNumElements())
                    break;
                Value* fieldPtr = Builder.CreateStructGEP(st, objPtr, idx++);
                Builder.CreateStore(val, fieldPtr);
            }
        }
        return objPtr;
    }

    // --- ADDED THIS METHOD ---
    Value* generateConstructor(ConstructorNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> args;
        for (auto& arg : node->args) {
            args.push_back(exprHandler(arg.value.get()));
        }
        return allocateAndInit(node->structName, args);
    }
    // -------------------------

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        Value* ptr = getMemberPtr(objPtr, memberName);
        if (!ptr)
            return nullptr;
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        if (!objPtr->getType()->isPointerTy() ||
            !objPtr->getType()->getPointerElementType()->isStructTy()) {
            // std::cerr << "Error: Not a struct pointer." << std::endl;
            return nullptr;
        }
        StructType* st =
            cast<StructType>(objPtr->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        if (!structLayouts.count(structName))
            return nullptr;

        int index = -1;
        const auto& fields = structLayouts[structName];
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i] == memberName) {
                index = i;
                break;
            }
        }

        if (index == -1)
            return nullptr;
        return Builder.CreateStructGEP(st, objPtr, index);
    }

    Value* createListFromValues(std::vector<Value*> values) {
        if (!StructTypes.count("List"))
            return nullptr;

        // 1. Create the buffer for the list items
        Type* voidPtr = Type::getInt8PtrTy(Context);

        // Safety: Allocate at least 8 bytes even if empty
        uint64_t bufSize = values.empty() ? 8 : values.size() * 8;
        Value* size = ConstantInt::get(Type::getInt64Ty(Context), bufSize);

        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(mallocFunc, {size});
        Value* bufferPtr =
            Builder.CreateBitCast(buffer, PointerType::getUnqual(voidPtr));

        // 2. Fill the buffer
        for (size_t i = 0; i < values.size(); i++) {
            Value* slot = Builder.CreateGEP(
                voidPtr, bufferPtr,
                ConstantInt::get(Type::getInt32Ty(Context), i));
            Value* v = values[i];

            // Box integers/bools to void*
            if (v->getType()->isIntegerTy()) {
                v = Builder.CreateIntToPtr(v, voidPtr);
            } else if (v->getType() != voidPtr) {
                v = Builder.CreateBitCast(v, voidPtr);
            }
            Builder.CreateStore(v, slot);
        }

        // 3. Initialize the List Object
        // --- FIX: Pass 'initial_cap' argument to match List__init signature
        // ---
        std::vector<Value*> listArgs;
        int cap = values.empty() ? 1 : values.size();
        listArgs.push_back(ConstantInt::get(Type::getInt32Ty(Context), cap));

        Value* listObj = allocateAndInit("List", listArgs);
        // ----------------------------------------------------------------------

        // 4. Manually overwrite fields to point to our pre-filled buffer
        // Note: This technically leaks the empty buffer created inside
        // List__init, but it prevents the segfault and is safe for now.

        // self.data = buffer (Index 0)
        Value* dataPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 0);
        Builder.CreateStore(buffer, dataPtr);

        // self.length = size (Index 1)
        Value* lenPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 1);
        Builder.CreateStore(
            ConstantInt::get(Type::getInt32Ty(Context), values.size()), lenPtr);

        // self.capacity = cap (Index 2)
        Value* capPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 2);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), cap),
                            capPtr);

        return listObj;
    }

    Value* generateListLiteral(ListLiteralNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> values;
        for (auto& elem : node->elements) {
            values.push_back(exprHandler(elem.get()));
        }
        return createListFromValues(values);
    }

    Value* generateStrCall(Value* obj, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* f = TheModule->getFunction(funcName);
        if (f)
            return Builder.CreateCall(f, {obj});

        return generateReprCall(obj, structName);
    }
    
    Value* generateReprCall(Value* obj, const std::string& structName) {
        std::string funcName = structName + "___repr";
        Function* f = TheModule->getFunction(funcName);
        if (f)
            return Builder.CreateCall(f, {obj});
        return nullptr;
    }

   private:
    std::map<std::string, std::vector<std::string>> structLayouts;
};