#pragma once
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <functional>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"
#include "BoxInt.hpp"

using namespace llvm;

class BuiltinGen;

class StructGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;
    BuiltinGen* builtinGen;
    std::map<std::string, std::vector<std::string>> structLayouts;
    std::map<std::string, std::string> structInitMap;
    std::map<std::string, std::vector<std::string>>* structHierarchy = nullptr;
    std::map<std::string, int> typeIdMap;

    // Virtual-dispatch bookkeeping (v2.3.3 — merged from Codegen.cpp).
    // `vtableEligible`: structs that get an `__type_id` (i32) prepended
    //   as field 0 — needed for the type-switch in overridden-method
    //   dispatch. Excludes extern-init C-backed structs (String, List,
    //   Char, …) and Exception.
    // `overrideMap[parent][methodName] = [(childType, childTypeId), …]`:
    //   for each method on a vtable-eligible struct, the chain of
    //   ancestor types that also define it. The Codegen dispatch site
    //   walks this list and emits one `case <id>: call <Child>_method`
    //   per entry.
    std::set<std::string> vtableEligible;
    std::map<std::string,
        std::map<std::string,
            std::vector<std::pair<std::string,int>>>> overrideMap;

   public:
    StructGen(LLVMContext& ctx, Module* mod, IRBuilder<>& builder, std::map<std::string, StructType*>& structs)
        : Context(ctx), TheModule(mod), Builder(builder), StructTypes(structs), builtinGen(nullptr) {}

    void setBuiltinGen(BuiltinGen* bg) { builtinGen = bg; }
    void setHierarchy(std::map<std::string, std::vector<std::string>>* h) { structHierarchy = h; }

    // Returns PointerType::getUnqual(String struct), or i8* if String not yet defined.
    Type* getStringPtrType() {
        if (StructTypes.count("String"))
            return PointerType::getUnqual(StructTypes.at("String"));
        return Type::getInt8PtrTy(Context);
    }

    void registerStructLayout(const std::string& name, const std::vector<std::string>& fields) {
        structLayouts[name] = fields;
    }

    void registerStructInit(const std::string& structName, const std::string& llvmFuncName) {
        structInitMap[structName] = llvmFuncName;
    }

    void registerTypeId(const std::string& name, int id) { typeIdMap[name] = id; }
    int  getTypeId(const std::string& name) const {
        auto it = typeIdMap.find(name);
        return it == typeIdMap.end() ? 0 : it->second;
    }

    // -- Virtual-dispatch state (v2.3.3). Codegen feeds entries via
    // the mutators below during its pre-scan passes; the dispatch
    // site at method-call time reads through the queries.
    void markVtableEligible(const std::string& name) { vtableEligible.insert(name); }
    bool isVtableEligible(const std::string& name) const {
        return vtableEligible.count(name) > 0;
    }
    void recordOverride(const std::string& parent, const std::string& method,
                        const std::string& child, int childTypeId) {
        overrideMap[parent][method].push_back({child, childTypeId});
    }
    // Returns nullptr if no overrides recorded for (parent, method).
    const std::vector<std::pair<std::string,int>>*
    getOverrides(const std::string& parent, const std::string& method) const {
        auto pIt = overrideMap.find(parent);
        if (pIt == overrideMap.end()) return nullptr;
        auto mIt = pIt->second.find(method);
        if (mIt == pIt->second.end()) return nullptr;
        return &mIt->second;
    }

    bool inheritsFrom(const std::string& typeName, const std::string& base) {
        if (typeName == base) return true;
        if (!structHierarchy || !structHierarchy->count(typeName)) return false;
        for (const auto& p : structHierarchy->at(typeName))
            if (inheritsFrom(p, base)) return true;
        return false;
    }

    Value* generateStrCall(Value* objPtr, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* strFunc = TheModule->getFunction(funcName);
        if (!strFunc) {
            funcName = structName + "__str";
            strFunc = TheModule->getFunction(funcName);
        }
        // Endswith fallback: find any function whose name ends with "<Struct>___str"
        // e.g. Core_Collections_Tuple_Tuple___str found when searching "Tuple"
        if (!strFunc) {
            std::string suffix = structName + "___str";
            for (auto& F : *TheModule)
                if (F.getName().endswith(suffix)) { strFunc = &F; break; }
        }
        if (!strFunc) {
            std::string suffix = structName + "__str";
            for (auto& F : *TheModule)
                if (F.getName().endswith(suffix)) { strFunc = &F; break; }
        }

        if (!strFunc && structHierarchy && structHierarchy->count(structName)) {
            std::function<Function*(const std::string&)> searchHierarchy = [&](const std::string& currentType) -> Function* {
                if (structHierarchy->count(currentType)) {
                    for (const std::string& parentName : structHierarchy->at(currentType)) {
                        Function* foundFunc = TheModule->getFunction(parentName + "___str");
                        if (!foundFunc) foundFunc = TheModule->getFunction(parentName + "__str");
                        if (!foundFunc) {
                            std::string sfx = parentName + "___str";
                            for (auto& F : *TheModule)
                                if (F.getName().endswith(sfx)) { foundFunc = &F; break; }
                        }
                        if (foundFunc) return foundFunc;
                        foundFunc = searchHierarchy(parentName);
                        if (foundFunc) return foundFunc;
                    }
                }
                return nullptr;
            };
            strFunc = searchHierarchy(structName);
        }

        if (!strFunc) return nullptr;

        Type* expectedSelfType = strFunc->getFunctionType()->getParamType(0);
        Value* castedSelf = objPtr;
        if (castedSelf->getType() != expectedSelfType)
            castedSelf = Builder.CreateBitCast(castedSelf, expectedSelfType);

        return Builder.CreateCall(strFunc, {castedSelf}, "str_res");
    }

    Value* allocateAndInit(const std::string& name, std::vector<Value*> args) {
        if (!StructTypes.count(name)) {
            std::cerr << "Error: Unknown struct " << name << std::endl;
            return nullptr;
        }

        StructType* st = StructTypes[name];

        if (st->isOpaque()) {
            std::cerr << "[StructGen] Warning: cannot allocate opaque struct '" << name << "'" << std::endl;
            return nullptr;
        }

        const auto& DL = TheModule->getDataLayout();
        uint64_t sizeBytes = DL.getTypeAllocSize(st);
        if (sizeBytes == 0) sizeBytes = 1;
        Value* allocSize = ConstantInt::get(Type::getInt64Ty(Context), sizeBytes);

        FunctionCallee mallocFunc = TheModule->getOrInsertFunction("GC_malloc",
            FunctionType::get(Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false));

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* objPtr = Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        std::string initName = name + "__init";
        Function* initFunc = nullptr;
        if (structInitMap.count(name))
            initFunc = TheModule->getFunction(structInitMap.at(name));
        if (!initFunc)
            initFunc = TheModule->getFunction(initName);

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

        if (initFunc) {
            std::vector<Value*> initArgs;

            Type* expectedSelfType = initFunc->getFunctionType()->getParamType(0);
            Value* castedSelf = objPtr;
            if (castedSelf->getType() != expectedSelfType)
                castedSelf = Builder.CreateBitCast(castedSelf, expectedSelfType);
            initArgs.push_back(castedSelf);

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
                        else if (argVal->getType()->isIntegerTy() && expectedType->isDoubleTy())
                            argVal = Builder.CreateSIToFP(argVal, expectedType);
                        else if (argVal->getType()->isDoubleTy() && expectedType->isIntegerTy())
                            argVal = Builder.CreateFPToSI(argVal, expectedType);
                        else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy())
                            argVal = Builder.CreateBitCast(argVal, expectedType);
                        else if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy())
                            argVal = quirk::boxIntToOpaque(Context, TheModule, Builder, argVal, expectedType);
                        else if (argVal->getType()->isDoubleTy() && expectedType->isPointerTy()) {
                            // Double → opaque ptr — box through the
                            // runtime helper so the value carries a
                            // real ANY_DOUBLE tag. Mirrors the
                            // Codegen.cpp call-arg path. Surfaces in
                            // generic-T constructor slots like
                            // `Some(value: T)` when T is erased.
                            Type* i8PtrTy = Type::getInt8PtrTy(Context);
                            FunctionCallee box = TheModule->getOrInsertFunction(
                                "Core_Primitives_Any_box_double",
                                i8PtrTy, Type::getDoubleTy(Context));
                            argVal = Builder.CreateCall(box, {argVal}, "arg_dbl_box");
                            if (expectedType != i8PtrTy)
                                argVal = Builder.CreateBitCast(argVal, expectedType);
                        }
                    }
                }
                initArgs.push_back(argVal);
            }
            Builder.CreateCall(initFunc, initArgs);

            // Store __type_id for virtual dispatch (field 0 of vtable-eligible structs)
            if (typeIdMap.count(name) && structLayouts.count(name)) {
                const auto& layout = structLayouts[name];
                if (!layout.empty() && layout[0] == "__type_id") {
                    Value* tidPtr = Builder.CreateStructGEP(st, objPtr, 0, "tid_gep");
                    Builder.CreateStore(
                        ConstantInt::get(Type::getInt32Ty(Context), typeIdMap[name]), tidPtr);
                }
            }

            // ISerializable registration
            if (structHierarchy && structHierarchy->count(name)) {
                bool isSerializable = false;
                for (const auto& parent : structHierarchy->at(name)) {
                    if (parent == "ISerializable") { isSerializable = true; break; }
                }
                if (!isSerializable) {
                    for (const auto& parent : structHierarchy->at(name)) {
                        if (structHierarchy->count(parent)) {
                            for (const auto& gp : structHierarchy->at(parent)) {
                                if (gp == "ISerializable") { isSerializable = true; break; }
                            }
                        }
                    }
                }
                if (isSerializable) {
                    Function* toJsonFn = TheModule->getFunction(name + "_to_json");
                    if (toJsonFn) {
                        Function* regFn = TheModule->getFunction("Quirk_register_serializable");
                        if (!regFn) {
                            FunctionType* ft = FunctionType::get(
                                Type::getVoidTy(Context),
                                {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)},
                                false);
                            regFn = Function::Create(ft, Function::ExternalLinkage,
                                "Quirk_register_serializable", TheModule);
                        }
                        Value* selfI8 = Builder.CreateBitCast(objPtr, Type::getInt8PtrTy(Context));
                        Value* fnI8   = Builder.CreateBitCast(toJsonFn, Type::getInt8PtrTy(Context));
                        Builder.CreateCall(regFn, {selfI8, fnI8});
                    }
                }
            }
        } else {
            // No __init found — directly GEP-assign each field (normal path for plain user-defined structs).
            // For vtable-eligible structs __type_id is field 0; skip it here and store below.
            bool hasTypeId = typeIdMap.count(name) && structLayouts.count(name) &&
                             !structLayouts[name].empty() && structLayouts[name][0] == "__type_id";
            int idx = hasTypeId ? 1 : 0;
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
                            val = allocateAndInit("String", boxArgs);
                        } else {
                            val = Builder.CreateBitCast(val, expectedType);
                        }
                    }
                    else if (val->getType()->isIntegerTy() && expectedType->isIntegerTy()) val = Builder.CreateIntCast(val, expectedType, true);
                    else if (val->getType()->isPointerTy() && expectedType->isPointerTy()) val = Builder.CreateBitCast(val, expectedType);
                    else if (val->getType()->isIntegerTy() && expectedType->isPointerTy()) val = quirk::boxIntToOpaque(Context, TheModule, Builder, val, expectedType);
                    else if (val->getType()->isIntegerTy() && expectedType->isDoubleTy()) val = Builder.CreateSIToFP(val, expectedType);
                }

                Builder.CreateStore(val, fieldPtr);
            }
            // Store __type_id for fallback path as well
            if (hasTypeId) {
                Value* tidPtr = Builder.CreateStructGEP(st, objPtr, 0, "tid_gep");
                Builder.CreateStore(
                    ConstantInt::get(Type::getInt32Ty(Context), typeIdMap[name]), tidPtr);
            }
        }
        return objPtr;
    }

    Value* generateConstructor(ConstructorNode* node, std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> args;
        for (auto& arg : node->args) args.push_back(exprHandler(arg.value.get()));
        return allocateAndInit(node->structName, args);
    }

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        // __len magic: .length on a struct that defines __len calls it
        if (memberName == "length" && objPtr && objPtr->getType()->isPointerTy()) {
            Type* elTy = objPtr->getType()->getPointerElementType();
            if (elTy->isStructTy()) {
                std::string sName = cast<StructType>(elTy)->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                Function* lenFunc = TheModule->getFunction(sName + "___len");
                if (!lenFunc) {
                    std::string suffix = sName + "___len";
                    for (auto& F : *TheModule)
                        if (F.getName().endswith(suffix)) { lenFunc = &F; break; }
                }
                if (lenFunc) return Builder.CreateCall(lenFunc, {objPtr}, "obj_len");
            }
        }

        Value* ptr = getMemberPtr(objPtr, memberName);
        if (!ptr) return nullptr;
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        if (!objPtr || !objPtr->getType()->isPointerTy() ||
            !objPtr->getType()->getPointerElementType()->isStructTy())
            return nullptr;

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

    Value* generateTupleLiteral(TupleLiteralNode* node, std::function<Value*(Node*)> exprHandler,
                                std::function<Value*(Value*)> boxHandler) {
        if (!StructTypes.count("Tuple")) return nullptr;
        int count = (int)node->elements.size();

        // Declare quirk_tuple_new if not yet in module
        Function* newFn = TheModule->getFunction("quirk_tuple_new");
        if (!newFn) {
            Type* retTy = PointerType::getUnqual(StructTypes["Tuple"]);
            FunctionType* ft = FunctionType::get(retTy, {Type::getInt32Ty(Context)}, false);
            newFn = Function::Create(ft, Function::ExternalLinkage, "quirk_tuple_new", TheModule);
        }
        Function* setFn = TheModule->getFunction("quirk_tuple_set");
        if (!setFn) {
            Type* tuplePtrTy = PointerType::getUnqual(StructTypes["Tuple"]);
            FunctionType* ft = FunctionType::get(Type::getVoidTy(Context),
                {tuplePtrTy, Type::getInt32Ty(Context), Type::getInt8PtrTy(Context)}, false);
            setFn = Function::Create(ft, Function::ExternalLinkage, "quirk_tuple_set", TheModule);
        }

        Value* tupleObj = Builder.CreateCall(newFn, {ConstantInt::get(Type::getInt32Ty(Context), count)});
        Type* voidPtr = Type::getInt8PtrTy(Context);
        for (int i = 0; i < count; i++) {
            Value* elem = exprHandler(node->elements[i].get());
            Value* boxed = boxHandler(elem);
            if (boxed->getType() != voidPtr) boxed = Builder.CreateBitCast(boxed, voidPtr);
            Builder.CreateCall(setFn, {tupleObj, ConstantInt::get(Type::getInt32Ty(Context), i), boxed});
        }
        return tupleObj;
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
        Value* listObj = allocateAndInit("List", listArgs);

        Value* dataPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 0);
        Builder.CreateStore(buffer, dataPtr);

        Value* lenPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 1);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), values.size()), lenPtr);

        Value* capPtr = Builder.CreateStructGEP(StructTypes["List"], listObj, 2);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), cap), capPtr);

        return listObj;
    }

    Value* generateSetLiteral(SetLiteralNode* node, std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> emptyArgs;
        Value* setObj = allocateAndInit("Set", emptyArgs);
        if (!setObj) return nullptr;

        Function* addFunc = TheModule->getFunction("Core_Collections_Set_Set_add");
        if (!addFunc) return setObj;

        Type* i8PtrTy = Type::getInt8PtrTy(Context);
        for (auto& elem : node->elements) {
            Value* v = exprHandler(elem.get());
            if (!v) continue;
            if (v->getType()->isIntegerTy()) v = Builder.CreateIntToPtr(v, i8PtrTy);
            else if (v->getType()->isPointerTy() && v->getType() != i8PtrTy) v = Builder.CreateBitCast(v, i8PtrTy);
            Builder.CreateCall(addFunc, {setObj, v});
        }
        return setObj;
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
