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
    std::map<std::string, std::vector<std::string>> structLayouts;

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

    Value* generateStrCall(Value* objPtr, const std::string& structName) {
        // 1. Construct method name (e.g., "Vector2___str")
        // We try triple underscore first (standard for operators/str in your setup)
        std::string funcName = structName + "___str";
        Function* strFunc = TheModule->getFunction(funcName);

        // 2. Fallback to double underscore
        if (!strFunc) {
            funcName = structName + "__str";
            strFunc = TheModule->getFunction(funcName);
        }

        if (!strFunc) {
            // If no __str exists, we cannot print it safely.
            // Return null so the caller can fallback (e.g. print address)
            return nullptr;
        }

        // 3. Call the function
        return Builder.CreateCall(strFunc, {objPtr}, "str_res");
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

        // Detect __init function (e.g., Vector2__init)
        std::string initName = name + "__init";
        Function* initFunc = TheModule->getFunction(initName);

        if (initFunc) {
            std::vector<Value*> initArgs;
            initArgs.push_back(objPtr); // 'self' argument

            // --- FIX START: Cast Arguments to Match Signature ---
            for (size_t i = 0; i < args.size(); i++) {
                Value* argVal = args[i];

                // Check if function has a defined parameter for this argument
                // (Index i+1 because index 0 is 'self')
                if (i + 1 < initFunc->arg_size()) {
                    Type* expectedType = initFunc->getFunctionType()->getParamType(i + 1);

                    if (argVal->getType() != expectedType) {
                        
                        // ✨ 1. NEW: Auto-box cstring -> String Struct
                        if (argVal->getType()->isPointerTy() && 
                            argVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                            expectedType->isPointerTy() && 
                            expectedType->getPointerElementType()->isStructTy()) {
                            
                            StructType* st = cast<StructType>(expectedType->getPointerElementType());
                            if (st->getName() == "String") {
                                // Recursively call allocateAndInit to properly build the String object!
                                std::vector<Value*> ctorArgs = {argVal};
                                argVal = allocateAndInit("String", ctorArgs);
                            } else {
                                argVal = Builder.CreateBitCast(argVal, expectedType);
                            }
                        }
                        // 2. Int -> Double (e.g. Vector2(3, 4))
                        else if (argVal->getType()->isIntegerTy() && expectedType->isDoubleTy()) {
                            argVal = Builder.CreateSIToFP(argVal, expectedType);
                        }
                        // 3. Double -> Int
                        else if (argVal->getType()->isDoubleTy() && expectedType->isIntegerTy()) {
                            argVal = Builder.CreateFPToSI(argVal, expectedType);
                        }
                        // 4. Pointer Casting (e.g. void* -> int*)
                        else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) {
                            argVal = Builder.CreateBitCast(argVal, expectedType);
                        }
                        // 5. Int -> Pointer (e.g. 0 -> NULL)
                        else if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy()) {
                            argVal = Builder.CreateIntToPtr(argVal, expectedType);
                        }
                    }
                }
                initArgs.push_back(argVal);
            }
            // --- FIX END ----------------------------------------

            Builder.CreateCall(initFunc, initArgs);
        } else {
            // Fallback: Manual Field Setting (No __init defined)
            int idx = 0;
            for (auto val : args) {
                if (idx >= (int)st->getNumElements()) break;
                
                // We should technically cast here too, but this path is rarely used
                // for complex types in your current setup.
                Value* fieldPtr = Builder.CreateStructGEP(st, objPtr, idx++);
                Builder.CreateStore(val, fieldPtr);
            }
        }
        return objPtr;
    }

    Value* generateConstructor(ConstructorNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> args;
        for (auto& arg : node->args) {
            args.push_back(exprHandler(arg.value.get()));
        }
        return allocateAndInit(node->structName, args);
    }

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        Value* ptr = getMemberPtr(objPtr, memberName);
        if (!ptr)
            return nullptr;
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        if (!objPtr->getType()->isPointerTy() ||
            !objPtr->getType()->getPointerElementType()->isStructTy()) {
            return nullptr;
        }
        StructType* st =
            cast<StructType>(objPtr->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        // 1. Strip "struct." prefix
        if (structName.find("struct.") == 0)
            structName = structName.substr(7);

        // 2. Fuzzy Lookup
        std::string matchedName = "";
        if (structLayouts.count(structName)) {
            matchedName = structName;
        } else {
            for (auto const& [key, val] : structLayouts) {
                if (structName.find(key) == 0) {
                    matchedName = key;
                    break;
                }
            }
        }

        if (matchedName.empty()) {
            std::cerr << "[StructGen Error] Layout not found for: " << structName << std::endl;
            return nullptr;
        }

        int index = -1;
        const auto& fields = structLayouts[matchedName];
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i] == memberName) {
                index = i;
                break;
            }
        }

        if (index == -1) {
            std::cerr << "[StructGen Error] Field '" << memberName 
                      << "' not found in struct '" << matchedName << "'" << std::endl;
            return nullptr;
        }
        return Builder.CreateStructGEP(st, objPtr, index);
    }

    Value* generateListLiteral(ListLiteralNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> values;
        for (auto& elem : node->elements) {
            values.push_back(exprHandler(elem.get()));
        }
        return createListFromValues(values);
    }

    Value* createListFromValues(std::vector<Value*> values) {
        if (!StructTypes.count("List"))
            return nullptr;

        Type* voidPtr = Type::getInt8PtrTy(Context);
        uint64_t bufSize = values.empty() ? 8 : values.size() * 8;
        Value* size = ConstantInt::get(Type::getInt64Ty(Context), bufSize);

        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(mallocFunc, {size});
        Value* bufferPtr =
            Builder.CreateBitCast(buffer, PointerType::getUnqual(voidPtr));

        for (size_t i = 0; i < values.size(); i++) {
            Value* slot = Builder.CreateGEP(
                voidPtr, bufferPtr,
                ConstantInt::get(Type::getInt32Ty(Context), i));
            Value* v = values[i];

            if (v->getType()->isIntegerTy()) {
                v = Builder.CreateIntToPtr(v, voidPtr);
            } else if (v->getType() != voidPtr) {
                v = Builder.CreateBitCast(v, voidPtr);
            }
            Builder.CreateStore(v, slot);
        }

        std::vector<Value*> listArgs;
        int cap = values.empty() ? 1 : values.size();
        listArgs.push_back(ConstantInt::get(Type::getInt32Ty(Context), cap));

        Value* listObj = allocateAndInit("List", listArgs);

        // Manually overwrite fields (since List__init allocates its own empty buffer)
        // This replaces the empty buffer with our pre-filled one.
        Value* dataPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 0);
        Builder.CreateStore(buffer, dataPtr);

        Value* lenPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 1);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), values.size()), lenPtr);

        Value* capPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 2);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), cap), capPtr);

        return listObj;
    }
    
    // Support for Map Literals (Added previously)
    Value* generateMapLiteral(MapLiteralNode* node, 
                              std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> emptyArgs;
        Value* mapObj = allocateAndInit("Map", emptyArgs);
        if (!mapObj) return nullptr;

        Function* putFunc = TheModule->getFunction("Map_put");
        if (!putFunc) return mapObj;

        for (auto& pair : node->elements) {
            Value* key = exprHandler(pair.first.get());
            Value* val = exprHandler(pair.second.get());

            if (key->getType()->isPointerTy() && 
                key->getType()->getPointerElementType()->isIntegerTy(8)) {
                std::vector<Value*> args = {key};
                key = allocateAndInit("String", args);
            }

            Type* voidPtr = Type::getInt8PtrTy(Context);
            if (val->getType()->isIntegerTy()) {
                val = Builder.CreateIntToPtr(val, voidPtr);
            } else if (val->getType()->isPointerTy() && val->getType() != voidPtr) {
                val = Builder.CreateBitCast(val, voidPtr);
            }
            else if (val->getType()->isDoubleTy()) {
                // Double -> String -> Void*
                Function* f2s = TheModule->getFunction("_float_to_str");
                if (f2s) {
                    Value* rawStr = Builder.CreateCall(f2s, {val});
                    std::vector<Value*> sArgs = {rawStr};
                    Value* strObj = allocateAndInit("String", sArgs);
                    val = Builder.CreateBitCast(strObj, voidPtr);
                } else {
                     val = Builder.CreateBitCast(val, voidPtr);
                }
            }

            Builder.CreateCall(putFunc, {mapObj, key, val});
        }
        return mapObj;
    }
};