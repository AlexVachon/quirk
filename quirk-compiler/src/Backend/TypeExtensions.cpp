#include <functional>
#include <iostream>
#include <vector>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class StructGen;
class BuiltinGen;

class TypeExtensions
{
    LLVMContext &Context;
    Module *TheModule;
    IRBuilder<> &Builder;
    StructGen *structGen;
    BuiltinGen *builtinGen;

public:
    TypeExtensions(LLVMContext &ctx,
                   Module *mod,
                   IRBuilder<> &build,
                   StructGen *sg,
                   BuiltinGen *bg)
        : Context(ctx),
          TheModule(mod),
          Builder(build),
          structGen(sg),
          builtinGen(bg) {}

    Value *tryHandleMethod(const std::string &typeName,
                       const std::string &methodName,
                       Value *objPtr,
                       const std::vector<Arg> &args,
                       std::function<Value *(Node *)> exprHandler)
    {
        if (typeName == "String" && methodName == "format")
            return handleStringFormat(objPtr, args, exprHandler);

        if (typeName == "String" && methodName == "format_map")
            return handleQuirkFormatMap(objPtr, args, exprHandler);

        if (typeName == "String" && methodName == "format_list")
            return handleQuirkFormatList(objPtr, args, exprHandler);

        return nullptr;
    }

private:
    // Boxes a value into Any* for passing to format list functions.
    // The C runtime (append_formatted/append_any) now expects Any* pointers.
    Value* prepareValueForRuntime(Value* v)
    {
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

        // Bool (i1)
        if (type->isIntegerTy(1)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("box_bool", {ext});
        }
        // Char (i8)
        if (type->isIntegerTy(8)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("box_char", {ext});
        }
        // Int (i32 or other integer)
        if (type->isIntegerTy()) {
            Value* casted = Builder.CreateIntCast(v, Type::getInt32Ty(Context), true);
            return callBox("box_int", {casted});
        }
        // Double
        if (type->isDoubleTy()) {
            return callBox("box_double", {v});
        }
        // Pointer types
        if (type->isPointerTy()) {
            Type* el = type->getPointerElementType();
            if (el->isStructTy()) {
                StructType* st = cast<StructType>(el);
                std::string name = st->getName().str();
                if (name.find("struct.") == 0) name = name.substr(7);

                // Already Any* — pass through as i8*
                if (name == "Any") {
                    return Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
                }
                // box_* declared as i8*(i8*) — must bitcast struct ptr to i8* first
                Value* asPtr = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
                if (name.find("String") != std::string::npos) return callBox("box_string", {asPtr});
                if (name.find("List")   != std::string::npos) return callBox("box_list",   {asPtr});
                if (name.find("Map")    != std::string::npos) return callBox("box_map",    {asPtr});
                // Other struct — call __str first, then box as String
                std::string strMethod = name + "___str";
                Function* strFunc = TheModule->getFunction(strMethod);
                if (!strFunc) strFunc = TheModule->getFunction(name + "__str");
                if (strFunc) {
                    Value* strObj = Builder.CreateCall(strFunc, {v});
                    if (strObj->getType()->isPointerTy() &&
                        strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                        std::vector<Value*> args = {strObj};
                        strObj = structGen->allocateAndInit("String", args);
                    }
                    return callBox("box_string", {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
                }
                return callBox("box_ptr", {asPtr});
            }
            // Raw i8* — wrap in String then box
            if (el->isIntegerTy(8)) {
                std::vector<Value*> wrapArgs = {v};
                Value* strObj = structGen->allocateAndInit("String", wrapArgs);
                return callBox("box_string", {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
            }
        }

        return Constant::getNullValue(Type::getInt8PtrTy(Context));
    }

    Value *handleStringFormat(Value *objPtr,
                              const std::vector<Arg> &args,
                              std::function<Value *(Node *)> exprHandler)
    {
        if (args.empty())
            return objPtr;
        bool isPositional = args[0].name.empty();
        return isPositional ? handlePositionalFormat(objPtr, args, exprHandler)
                            : handleMapFormat(objPtr, args, exprHandler);
    }

    Value *handlePositionalFormat(Value *objPtr,
                                  const std::vector<Arg> &args,
                                  std::function<Value *(Node *)> exprHandler)
    {
        std::vector<Value *> runtimeArgs;

        for (const auto &arg : args)
        {
            Value *val = exprHandler(arg.value.get());
            // --- FIX: Box raw value instead of converting to string ---
            runtimeArgs.push_back(prepareValueForRuntime(val));
        }

        Value *argsList = structGen->createListFromValues(runtimeArgs);

        Function *func = TheModule->getFunction("String_format_list");
        if (!func)
        {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }

        return Builder.CreateCall(func, {objPtr, argsList});
    }

    Value *handleMapFormat(Value *objPtr,
                           const std::vector<Arg> &args,
                           std::function<Value *(Node *)> exprHandler)
    {
        std::vector<Value *> keys;
        std::vector<Value *> vals;

        for (const auto &arg : args)
        {
            // Wrap the raw key name in a String* so find_key_index can handle it
            // consistently with Quirk-level format_map(["key"], ["val"]) calls.
            Value* rawKey = Builder.CreateGlobalStringPtr(arg.name);
            std::vector<Value*> keyArgs = {rawKey};
            Value* keyStr = structGen->allocateAndInit("String", keyArgs);
            keys.push_back(Builder.CreateBitCast(keyStr, Type::getInt8PtrTy(Context)));

            Value *v = exprHandler(arg.value.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value *keysList = structGen->createListFromValues(keys);
        Value *valsList = structGen->createListFromValues(vals);

        Function *func = TheModule->getFunction("String_format_map");
        if (!func)
        {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }

    // Handles: "template".format_map(["key1", "key2"], ["val1", "val2"])
    // args[0] = keys ListLiteralNode, args[1] = values ListLiteralNode
    Value *handleQuirkFormatMap(Value *objPtr,
                                const std::vector<Arg> &args,
                                std::function<Value *(Node *)> exprHandler)
    {
        if (args.size() < 2) return nullptr;

        auto* keysLit  = dynamic_cast<ListLiteralNode*>(args[0].value.get());
        auto* valsLit  = dynamic_cast<ListLiteralNode*>(args[1].value.get());
        if (!keysLit || !valsLit) return nullptr;

        // Keys: must stay as raw String* — find_key_index expects String*, not Any*
        std::vector<Value*> keys;
        for (auto& elem : keysLit->elements) {
            Value* v = exprHandler(elem.get());
            // Ensure it's an i8* (String* bitcast) for find_key_index
            if (v->getType()->isPointerTy() &&
                v->getType()->getPointerElementType()->isStructTy()) {
                v = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
            }
            keys.push_back(v);
        }

        // Values: must be boxed as Any* for append_formatted
        std::vector<Value*> vals;
        for (auto& elem : valsLit->elements) {
            Value* v = exprHandler(elem.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* keysList = structGen->createListFromValues(keys);
        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("String_format_map");
        if (!func) {
            std::cerr << "Error: 'String_format_map' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, keysList, valsList});
    }

    // Handles: "template".format_list(["val1", "val2"])
    // args[0] = values ListLiteralNode
    Value *handleQuirkFormatList(Value *objPtr,
                                const std::vector<Arg> &args,
                                std::function<Value *(Node *)> exprHandler)
    {
        if (args.empty()) return nullptr;

        auto* valsLit = dynamic_cast<ListLiteralNode*>(args[0].value.get());
        if (!valsLit) return nullptr;

        std::vector<Value*> vals;
        for (auto& elem : valsLit->elements) {
            Value* v = exprHandler(elem.get());
            vals.push_back(prepareValueForRuntime(v));
        }

        Value* valsList = structGen->createListFromValues(vals);

        Function* func = TheModule->getFunction("String_format_list");
        if (!func) {
            std::cerr << "Error: 'String_format_list' not found." << std::endl;
            return Constant::getNullValue(objPtr->getType());
        }
        return Builder.CreateCall(func, {objPtr, valsList});
    }
};