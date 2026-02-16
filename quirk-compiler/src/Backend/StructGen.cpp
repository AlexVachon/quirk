#include <iostream>
#include <map>
#include <string>
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

   public:
    StructGen(LLVMContext& ctx, Module* mod, IRBuilder<>& builder, std::map<std::string, StructType*>& structs)
        : Context(ctx), TheModule(mod), Builder(builder), StructTypes(structs), builtinGen(nullptr) {}

    void setBuiltinGen(BuiltinGen* bg) { builtinGen = bg; }

    void registerStructLayout(const std::string& name, const std::vector<std::string>& fields) {
        structLayouts[name] = fields;
    }

    Value* generateStrCall(Value* objPtr, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* strFunc = TheModule->getFunction(funcName);
        if (!strFunc) {
            funcName = structName + "__str";
            strFunc = TheModule->getFunction(funcName);
        }
        if (!strFunc) return nullptr;
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
        if (allocSize->getType()->getIntegerBitWidth() < 64)
            allocSize = Builder.CreateZExt(allocSize, Type::getInt64Ty(Context));

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* objPtr = Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        std::string initName = name + "__init";
        Function* initFunc = TheModule->getFunction(initName);

        if (initFunc) {
            std::vector<Value*> initArgs;
            initArgs.push_back(objPtr); 

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

        Function* mallocFunc = TheModule->getFunction("malloc");
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