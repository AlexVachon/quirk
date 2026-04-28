#pragma once
#include <functional>
#include <iostream>
#include <vector>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class StructGen;
class BuiltinGen;

class TypeExtensions {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    StructGen* structGen;
    BuiltinGen* builtinGen;
    std::map<std::string, std::vector<std::string>>* structHierarchy = nullptr;
    std::map<std::string, StructType*>* structTypes = nullptr;

public:
    TypeExtensions(LLVMContext& ctx, Module* mod, IRBuilder<>& build, StructGen* sg, BuiltinGen* bg)
        : Context(ctx), TheModule(mod), Builder(build), structGen(sg), builtinGen(bg) {}

    void setHierarchy(std::map<std::string, std::vector<std::string>>* h,
                      std::map<std::string, StructType*>* t) {
        structHierarchy = h;
        structTypes = t;
    }

    Value* tryHandleMethod(const std::string& typeName, const std::string& methodName,
                           Value* objPtr, const std::vector<Arg>& args,
                           std::function<Value*(Node*)> exprHandler) {
        if (typeName == "String" && methodName == "format")
            return handleStringFormat(objPtr, args, exprHandler);
        if (typeName == "String" && methodName == "format_map")
            return handleQuirkFormatMap(objPtr, args, exprHandler);
        if (typeName == "String" && methodName == "format_list")
            return handleQuirkFormatList(objPtr, args, exprHandler);
        return nullptr;
    }

private:
    Value* prepareValueForRuntime(Value* v) {
        if (!v) return Constant::getNullValue(Type::getInt8PtrTy(Context));

        Type* type = v->getType();

        auto callBox = [&](const std::string& fname, std::vector<Value*> args) -> Value* {
            Function* f = TheModule->getFunction(fname);
            if (f) {
                Value* boxed = Builder.CreateCall(f, args);
                return Builder.CreateBitCast(boxed, Type::getInt8PtrTy(Context));
            }
            return Constant::getNullValue(Type::getInt8PtrTy(Context));
        };

        if (type->isIntegerTy(1)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("Core_Primitives_Any_box_bool", {ext});
        }
        if (type->isIntegerTy(8)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("Core_Primitives_Any_box_char", {ext});
        }
        if (type->isIntegerTy()) {
            Value* casted = Builder.CreateIntCast(v, Type::getInt32Ty(Context), true);
            return callBox("Core_Primitives_Any_box_int", {casted});
        }
        if (type->isDoubleTy())
            return callBox("Core_Primitives_Any_box_double", {v});

        if (type->isPointerTy()) {
            Type* el = type->getPointerElementType();
            if (el->isStructTy()) {
                StructType* st = cast<StructType>(el);
                std::string name = st->getName().str();
                if (name.find("struct.") == 0) name = name.substr(7);
                { size_t d = name.find('.'); if (d != std::string::npos && std::isdigit((unsigned char)name[d+1])) name = name.substr(0, d); }

                if (name == "Any")
                    return Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));

                Value* asPtr = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
                if (name.find("String") != std::string::npos) return callBox("Core_Primitives_Any_box_string", {asPtr});
                if (name.find("List")   != std::string::npos) return callBox("Core_Primitives_Any_box_list",   {asPtr});
                if (name.find("Map")    != std::string::npos) return callBox("Core_Primitives_Any_box_map",    {asPtr});

                // Find __str walking up the inheritance hierarchy
                auto findStr = [&](const std::string& t) -> Function* {
                    Function* f = TheModule->getFunction(t + "___str");
                    if (!f) { std::string sfx = t + "___str"; for (auto& F : *TheModule) if (F.getName().endswith(sfx)) { f = &F; break; } }
                    if (!f) f = TheModule->getFunction(t + "__str");
                    return f;
                };
                Function* strFunc = findStr(name);
                Value* strSelf = v;
                if (!strFunc && structHierarchy) {
                    std::string cur = name;
                    while (!strFunc && structHierarchy->count(cur) && !structHierarchy->at(cur).empty()) {
                        cur = structHierarchy->at(cur)[0];
                        strFunc = findStr(cur);
                        if (strFunc && structTypes && structTypes->count(cur))
                            strSelf = Builder.CreateBitCast(v, PointerType::getUnqual(structTypes->at(cur)));
                    }
                }
                if (strFunc) {
                    Value* strObj = Builder.CreateCall(strFunc, {strSelf});
                    if (strObj->getType()->isPointerTy() &&
                        strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                        std::vector<Value*> args = {strObj};
                        strObj = structGen->allocateAndInit("String", args);
                    }
                    return callBox("Core_Primitives_Any_box_string",
                                   {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
                }
                return callBox("Core_Primitives_Any_box_ptr", {asPtr});
            }
            if (el->isIntegerTy(8)) {
                // raw i8* — may be String*, Any*, or tagged integer; convert safely
                Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                if (!opaqueToStr) {
                    StructType* strTy = StructType::getTypeByName(Context, "String");
                    if (!strTy) strTy = StructType::getTypeByName(Context, "struct.String");
                    Type* retTy = strTy ? (Type*)PointerType::getUnqual(strTy) : (Type*)Type::getInt8PtrTy(Context);
                    FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                    opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule);
                }
                Value* strObj = Builder.CreateCall(opaqueToStr, {v});
                return callBox("Core_Primitives_Any_box_string",
                               {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
            }
        }

        return Constant::getNullValue(Type::getInt8PtrTy(Context));
    }

    Value* handleStringFormat(Value* objPtr, const std::vector<Arg>& args,
                              std::function<Value*(Node*)> exprHandler) {
        if (args.empty()) return objPtr;
        bool isPositional = args[0].name.empty();
        return isPositional ? handlePositionalFormat(objPtr, args, exprHandler)
                            : handleMapFormat(objPtr, args, exprHandler);
    }

    Value* handlePositionalFormat(Value* objPtr, const std::vector<Arg>& args,
                                  std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> runtimeArgs;
        for (const auto& arg : args) {
            Value* val = exprHandler(arg.value.get());
            runtimeArgs.push_back(prepareValueForRuntime(val));
        }

        Value* argsList = structGen->createListFromValues(runtimeArgs);

        Function* func = TheModule->getFunction("Core_String_String_format_list");
        if (!func) {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, argsList});
    }

    Value* handleMapFormat(Value* objPtr, const std::vector<Arg>& args,
                           std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> keys;
        std::vector<Value*> vals;

        for (const auto& arg : args) {
            Value* rawKey = Builder.CreateGlobalStringPtr(arg.name);
            std::vector<Value*> keyArgs = {rawKey};
            Value* keyStr = structGen->allocateAndInit("String", keyArgs);
            keys.push_back(Builder.CreateBitCast(keyStr, Type::getInt8PtrTy(Context)));

            Value* v = exprHandler(arg.value.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* keysList = structGen->createListFromValues(keys);
        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("Core_String_String_format_map");
        if (!func) {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }

    Value* handleQuirkFormatMap(Value* objPtr, const std::vector<Arg>& args,
                                std::function<Value*(Node*)> exprHandler) {
        if (args.size() < 2) return nullptr;

        auto* keysLit = dynamic_cast<ListLiteralNode*>(args[0].value.get());
        auto* valsLit = dynamic_cast<ListLiteralNode*>(args[1].value.get());
        if (!keysLit || !valsLit) return nullptr;

        std::vector<Value*> keys;
        for (auto& elem : keysLit->elements) {
            Value* v = exprHandler(elem.get());
            if (v->getType()->isPointerTy() && v->getType()->getPointerElementType()->isStructTy())
                v = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
            keys.push_back(v);
        }

        std::vector<Value*> vals;
        for (auto& elem : valsLit->elements) {
            Value* v = exprHandler(elem.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* keysList = structGen->createListFromValues(keys);
        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("Core_String_String_format_map");
        if (!func) {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }

    Value* handleQuirkFormatList(Value* objPtr, const std::vector<Arg>& args,
                                 std::function<Value*(Node*)> exprHandler) {
        if (args.empty()) return nullptr;

        auto* valsLit = dynamic_cast<ListLiteralNode*>(args[0].value.get());
        if (!valsLit) return nullptr;

        std::vector<Value*> vals;
        for (auto& elem : valsLit->elements) {
            Value* v = exprHandler(elem.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("Core_String_String_format_list");
        if (!func) {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, valsList});
    }
};
