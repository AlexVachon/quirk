#include <iostream>
#include <map>
#include <string>
#include <functional>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class BuiltinGen;  

class StructGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;
    BuiltinGen* builtinGen;
    std::map<std::string, std::vector<std::string>> structLayouts;
    // Maps struct name -> actual LLVM function name for __init
    std::map<std::string, std::string> structInitMap;

    // --- NEW: Hierarchy tracking ---
    std::map<std::string, std::vector<std::string>>* structHierarchy = nullptr;

   public:
    StructGen(LLVMContext& ctx, Module* mod, IRBuilder<>& builder, std::map<std::string, StructType*>& structs)
        : Context(ctx), TheModule(mod), Builder(builder), StructTypes(structs), builtinGen(nullptr) {}

    void setBuiltinGen(BuiltinGen* bg) { builtinGen = bg; }
    
    // --- NEW: Setter for Hierarchy ---
    void setHierarchy(std::map<std::string, std::vector<std::string>>* h) { structHierarchy = h; }

    void registerStructLayout(const std::string& name, const std::vector<std::string>& fields) {
        structLayouts[name] = fields;
    }

    void registerStructInit(const std::string& structName, const std::string& llvmFuncName) {
        structInitMap[structName] = llvmFuncName;
    }

    Value* generateStrCall(Value* objPtr, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* strFunc = TheModule->getFunction(funcName);
        if (!strFunc) {
            funcName = structName + "__str";
            strFunc = TheModule->getFunction(funcName);
        }

        // --- NEW: Search parent __str methods recursively ---
        if (!strFunc && structHierarchy && structHierarchy->count(structName)) {
            std::function<Function*(const std::string&)> searchHierarchy = [&](const std::string& currentType) -> Function* {
                if (structHierarchy->count(currentType)) {
                    for (const std::string& parentName : structHierarchy->at(currentType)) {
                        Function* foundFunc = TheModule->getFunction(parentName + "___str");
                        if (!foundFunc) foundFunc = TheModule->getFunction(parentName + "__str");
                        if (foundFunc) return foundFunc;
                        
                        foundFunc = searchHierarchy(parentName);
                        if (foundFunc) return foundFunc;
                    }
                }
                return nullptr;
            };
            strFunc = searchHierarchy(structName);
        }
        // --- END NEW ---

        if (!strFunc) return nullptr;

        // --- NEW: Cast 'self' pointer if using an inherited __str ---
        Type* expectedSelfType = strFunc->getFunctionType()->getParamType(0);
        Value* castedSelf = objPtr;
        if (castedSelf->getType() != expectedSelfType) {
            castedSelf = Builder.CreateBitCast(castedSelf, expectedSelfType);
        }

        return Builder.CreateCall(strFunc, {castedSelf}, "str_res");
    }

    Value* allocateAndInit(const std::string& name, std::vector<Value*>& args) {
        if (!StructTypes.count(name)) {
            std::cerr << "Error: Unknown struct " << name << std::endl;
            return nullptr;
        }

        StructType* st = StructTypes[name];

        // Guard: opaque structs cannot be size-computed or heap-allocated.
        // This can happen for empty marker structs (Int, Bool, etc.)
        // that were never given a body via setBody().
        if (st->isOpaque()) {
            std::cerr << "[StructGen] Warning: cannot allocate opaque struct '" << name << "'" << std::endl;
            return nullptr;
        }

        // Use DataLayout for safe size calculation (avoids ConstantExpr::getSizeOf
        // which returns null for zero-element structs in some LLVM versions).
        const auto& DL = TheModule->getDataLayout();
        uint64_t sizeBytes = DL.getTypeAllocSize(st);
        // Ensure at least 1 byte so malloc(0) is never called.
        if (sizeBytes == 0) sizeBytes = 1;
        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context), sizeBytes);

        FunctionCallee mallocFunc = TheModule->getOrInsertFunction("GC_malloc", 
            FunctionType::get(Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false));

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* objPtr = Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        // Resolve __init via structInitMap first (accounts for FFI renaming),
        // then fall back to the bare "<Name>__init" for Quirk-defined structs.
        std::string initName = name + "__init";
        Function* initFunc = nullptr;
        if (structInitMap.count(name))
            initFunc = TheModule->getFunction(structInitMap.at(name));
        if (!initFunc)
            initFunc = TheModule->getFunction(initName);

        // --- NEW: Search parent constructors recursively ---
        if (!initFunc && structHierarchy && structHierarchy->count(name)) {
            std::function<Function*(const std::string&)> searchHierarchy = [&](const std::string& currentType) -> Function* {
                if (structHierarchy->count(currentType)) {
                    for (const std::string& parentName : structHierarchy->at(currentType)) {
                        Function* foundFunc = nullptr;
                        if (structInitMap.count(parentName))
                            foundFunc = TheModule->getFunction(structInitMap.at(parentName));
                        if (!foundFunc) foundFunc = TheModule->getFunction(parentName + "__init");
                        if (foundFunc) return foundFunc;
                        
                        foundFunc = searchHierarchy(parentName);
                        if (foundFunc) return foundFunc;
                    }
                }
                return nullptr;
            };
            initFunc = searchHierarchy(name);
        }
        // --- END NEW ---

        if (initFunc) {
            std::vector<Value*> initArgs;
            
            // --- NEW: Cast 'self' pointer if using a parent constructor ---
            Type* expectedSelfType = initFunc->getFunctionType()->getParamType(0);
            Value* castedSelf = objPtr;
            if (castedSelf->getType() != expectedSelfType) {
                castedSelf = Builder.CreateBitCast(castedSelf, expectedSelfType);
            }
            initArgs.push_back(castedSelf);
            // --- END NEW ---

            for (size_t i = 0; i < args.size(); i++) {
                Value* argVal = args[i];
                if (i + 1 < initFunc->arg_size()) {
                    Type* expectedType = initFunc->getFunctionType()->getParamType(i + 1);

                    if (argVal->getType() != expectedType) {
                        if (argVal->getType()->isPointerTy() && 
                            argVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                            expectedType->isPointerTy() && 
                            expectedType->getPointerElementType()->isStructTy()) {
                            
                            StructType* pst = cast<StructType>(expectedType->getPointerElementType());
                            if (pst->getName() == "String") {
                                if (name == "String") {
                                    argVal = Builder.CreateBitCast(argVal, expectedType);
                                } else {
                                    std::vector<Value*> ctorArgs = {argVal};
                                    argVal = allocateAndInit("String", ctorArgs);
                                }
                            } else {
                                argVal = Builder.CreateBitCast(argVal, expectedType);
                            }
                        }
                        else if (argVal->getType()->isIntegerTy() && expectedType->isDoubleTy()) {
                            argVal = Builder.CreateSIToFP(argVal, expectedType);
                        }
                        else if (argVal->getType()->isDoubleTy() && expectedType->isIntegerTy()) {
                            argVal = Builder.CreateFPToSI(argVal, expectedType);
                        }
                        else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) {
                            argVal = Builder.CreateBitCast(argVal, expectedType);
                        }
                        else if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy()) {
                            argVal = Builder.CreateIntToPtr(argVal, expectedType);
                        }
                    }
                }
                initArgs.push_back(argVal);
            }
            Builder.CreateCall(initFunc, initArgs);
        } else {
            std::cerr << "[DEBUG] StructGen::allocateAndInit fallback - About to GEP fields manually\n";
            int idx = 0;
            for (auto val : args) {
                if (idx >= (int)st->getNumElements()) break;
                Value* fieldPtr = Builder.CreateStructGEP(st, objPtr, idx++);
                
                Type* expectedType = fieldPtr->getType()->getPointerElementType();
                
                if (val->getType() != expectedType) {
                    if (val->getType()->isPointerTy() && 
                        val->getType()->getPointerElementType()->isIntegerTy(8) &&
                        expectedType->isPointerTy() && 
                        expectedType->getPointerElementType()->isStructTy()) {
                        
                        StructType* pst = cast<StructType>(expectedType->getPointerElementType());
                        std::string sName = pst->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        
                        if (sName == "String") {
                            std::vector<Value*> boxArgs = {val};
                            val = allocateAndInit("String", boxArgs); // Auto-box string!
                        } else {
                            val = Builder.CreateBitCast(val, expectedType);
                        }
                    }
                    else if (val->getType()->isIntegerTy() && expectedType->isIntegerTy()) val = Builder.CreateIntCast(val, expectedType, true);
                    else if (val->getType()->isPointerTy() && expectedType->isPointerTy()) val = Builder.CreateBitCast(val, expectedType);
                    else if (val->getType()->isIntegerTy() && expectedType->isPointerTy()) val = Builder.CreateIntToPtr(val, expectedType);
                    else if (val->getType()->isIntegerTy() && expectedType->isDoubleTy()) val = Builder.CreateSIToFP(val, expectedType);
                }

                Builder.CreateStore(val, fieldPtr);
            }
        }
        return objPtr;
    }

    Value* generateConstructor(ConstructorNode* node, std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> args;
        for (auto& arg : node->args) {
            args.push_back(exprHandler(arg.value.get()));
        }
        return allocateAndInit(node->structName, args);
    }

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        Value* ptr = getMemberPtr(objPtr, memberName);
        if (!ptr) return nullptr;
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        if (!objPtr || !objPtr->getType()->isPointerTy() ||
            !objPtr->getType()->getPointerElementType()->isStructTy()) {
            return nullptr;
        }

        StructType* st = cast<StructType>(objPtr->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        if (structName.find("struct.") == 0) structName = structName.substr(7);

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

        if (matchedName.empty()) return nullptr;

        int index = -1;
        const auto& fields = structLayouts[matchedName];
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i] == memberName) {
                index = i;
                break;
            }
        }

        if (index == -1) return nullptr;

        return Builder.CreateStructGEP(st, objPtr, index);
    }

    Value* generateListLiteral(ListLiteralNode* node, std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> values;
        for (auto& elem : node->elements) values.push_back(exprHandler(elem.get()));
        return createListFromValues(values);
    }

    Value* createListFromValues(std::vector<Value*> values) {
        if (!StructTypes.count("List")) return nullptr;

        Type* voidPtr = Type::getInt8PtrTy(Context);
        uint64_t bufSize = values.empty() ? 8 : values.size() * 8;
        Value* size = ConstantInt::get(Type::getInt64Ty(Context), bufSize);

        FunctionCallee mallocFunc = TheModule->getOrInsertFunction("GC_malloc", 
            FunctionType::get(Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false));
            
        Value* buffer = Builder.CreateCall(mallocFunc, {size});
        Value* bufferPtr = Builder.CreateBitCast(buffer, PointerType::getUnqual(voidPtr));

        for (size_t i = 0; i < values.size(); i++) {
            Value* slot = Builder.CreateGEP(voidPtr, bufferPtr, ConstantInt::get(Type::getInt32Ty(Context), i));
            Value* v = values[i];
            if (v->getType()->isIntegerTy()) v = Builder.CreateIntToPtr(v, voidPtr);
            else if (v->getType() != voidPtr) v = Builder.CreateBitCast(v, voidPtr);
            Builder.CreateStore(v, slot);
        }

        std::vector<Value*> listArgs;
        int cap = values.empty() ? 1 : values.size();
        listArgs.push_back(ConstantInt::get(Type::getInt32Ty(Context), cap));

        Value* listObj = allocateAndInit("List", listArgs);

        Value* dataPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 0);
        Builder.CreateStore(buffer, dataPtr);

        Value* lenPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 1);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), values.size()), lenPtr);

        Value* capPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 2);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), cap), capPtr);

        return listObj;
    }
    
    Value* generateMapLiteral(MapLiteralNode* node, std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> emptyArgs;
        Value* mapObj = allocateAndInit("Map", emptyArgs);
        if (!mapObj) return nullptr;

        Function* putFunc = TheModule->getFunction("Core_Collections_Map_Map_put");
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
            if (val->getType()->isIntegerTy()) val = Builder.CreateIntToPtr(val, voidPtr);
            else if (val->getType()->isPointerTy() && val->getType() != voidPtr) val = Builder.CreateBitCast(val, voidPtr);
            else if (val->getType()->isDoubleTy()) {
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