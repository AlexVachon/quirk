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
        // box_tuple — missing from this block before v3.24.0,
        // which made emitBox's Tuple branch silently fall back to
        // a null Constant when the helper couldn't be looked up
        // in the LLVM module. Tuples flowing through `Any` slots
        // (List.append items, etc.) lost their identity and
        // `print(xs)` rendered them as empty / "null" / garbage.
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_tuple",  TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_box_ptr",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {},           false), Function::ExternalLinkage, "Core_Primitives_Any_box_null",   TheModule);

        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_string", TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_str",    TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any___str",     TheModule);
        Function::Create(FunctionType::get(anyPtrTy, {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_get_type",  TheModule);
        Function::Create(FunctionType::get(i32Ty,    {anyPtrTy},  false), Function::ExternalLinkage, "Core_Primitives_Any_to_int",    TheModule);
        Function::Create(FunctionType::get(Type::getDoubleTy(Context), {anyPtrTy}, false), Function::ExternalLinkage, "Core_Primitives_Any_to_float", TheModule);
        Function::Create(FunctionType::get(i32Ty,    {anyPtrTy, anyPtrTy}, false), Function::ExternalLinkage, "Core_Primitives_Any_isinstance", TheModule);

        // Core_Primitives_Bool_str is intentionally NOT pre-declared here.
        // Pass 3 declares it with the correct %String* return type from bool.quirk's extern.
        // Pre-declaring with i8* causes a type conflict that makes all Bool→String calls return i8*.
        auto strPtrTy = Type::getInt8PtrTy(Context);
        Function::Create(FunctionType::get(i32Ty, {strPtrTy, strPtrTy}, false), Function::ExternalLinkage, "Core_Primitives_Quirk_isinstance", TheModule);

        // libc file I/O — backing the read_file/write_file builtins.
        // Signatures match the selfhost compiler's `ensure_decl`
        // calls so both compilers emit interchangeable IR.
        Type* i64Ty = Type::getInt64Ty(Context);
        Function::Create(FunctionType::get(voidPtrTy, {voidPtrTy, voidPtrTy}, false),
                         Function::ExternalLinkage, "fopen", TheModule);
        Function::Create(FunctionType::get(i32Ty, {voidPtrTy}, false),
                         Function::ExternalLinkage, "fclose", TheModule);
        Function::Create(FunctionType::get(i32Ty, {voidPtrTy, i64Ty, i32Ty}, false),
                         Function::ExternalLinkage, "fseek", TheModule);
        Function::Create(FunctionType::get(i64Ty, {voidPtrTy}, false),
                         Function::ExternalLinkage, "ftell", TheModule);
        Function::Create(FunctionType::get(i64Ty, {voidPtrTy, i64Ty, i64Ty, voidPtrTy}, false),
                         Function::ExternalLinkage, "fread", TheModule);
        Function::Create(FunctionType::get(i64Ty, {voidPtrTy, i64Ty, i64Ty, voidPtrTy}, false),
                         Function::ExternalLinkage, "fwrite", TheModule);

        // argv-access builtins forward to the runtime helpers
        // in src/Runtime/libs/sys.c, which already track argc/argv
        // populated at process startup.
        Function::Create(FunctionType::get(i32Ty, {}, false),
                         Function::ExternalLinkage, "Sys_arg_count", TheModule);
        Function::Create(FunctionType::get(voidPtrTy, {i32Ty}, false),
                         Function::ExternalLinkage, "Sys_arg_get", TheModule);
    }

    bool isBuiltin(const std::string& name) {
        return name == "print" || name == "printf" || name == "malloc" || name == "free"
            || name == "type" || name == "str" || name == "write" || name == "writeln"
            || name == "read_file" || name == "write_file"
            || name == "arg_count" || name == "arg_get";
    }

    Value* handleBuiltin(const std::string& name, CallNode* call,
                         std::function<Value*(Node*)> exprHandler) {
        if (name == "print")      return generatePrint(call, exprHandler);
        if (name == "printf")     return generatePrintf(call, exprHandler);
        if (name == "type")       return generateType(call, exprHandler);
        if (name == "str")        return generateStr(call, exprHandler);
        if (name == "write")      return generateWrite(call, exprHandler, false);
        if (name == "writeln")    return generateWrite(call, exprHandler, true);
        if (name == "read_file")  return generateReadFile(call, exprHandler);
        if (name == "write_file") return generateWriteFile(call, exprHandler);
        if (name == "arg_count")  return generateArgCount(call, exprHandler);
        if (name == "arg_get")    return generateArgGet(call, exprHandler);
        return nullptr;
    }

    // arg_count() -> Int — forwards to Sys_arg_count (sys.c)
    Value* generateArgCount(CallNode* call, std::function<Value*(Node*)>) {
        if (!call->args.empty()) return nullptr;
        Function* fn = TheModule->getFunction("Sys_arg_count");
        if (!fn) return nullptr;
        return Builder.CreateCall(fn, {});
    }

    // arg_get(i: Int) -> String — forwards to Sys_arg_get,
    // which returns a String* (allocated in the runtime). The
    // selfhost-emitted shape returns a raw i8* via the
    // @quirk_argv global; both shape-match `String` at the
    // Quirk type system level.
    Value* generateArgGet(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        if (call->args.size() != 1) return nullptr;
        Value* idx = exprHandler(call->args[0].value.get());
        if (!idx) return nullptr;
        // Sys_arg_get expects i32; coerce if needed.
        Type* i32Ty = Type::getInt32Ty(Context);
        if (idx->getType() != i32Ty) {
            if (idx->getType()->isIntegerTy())
                idx = Builder.CreateIntCast(idx, i32Ty, true);
            else
                return nullptr;
        }
        Function* fn = TheModule->getFunction("Sys_arg_get");
        if (!fn) return nullptr;
        Value* raw = Builder.CreateCall(fn, {idx});
        // Sys_arg_get returns String* (boxed) — but the C
        // signature in this module is declared i8* for ABI
        // simplicity. Wrap the raw i8* into a fresh String so
        // user code can call .length() / .substring() etc.
        return structGen->allocateAndInit("String", {raw});
    }

    // Extract the underlying i8* buffer from a Quirk-String value.
    // The compiler routes String literals/locals as `%String*`
    // (a struct with `_buffer: i8*`). For raw i8* values that
    // somehow slipped through, return as-is.
    Value* stringBuffer(Value* v) {
        if (!v) return nullptr;
        Type* ty = v->getType();
        if (ty->isPointerTy()) {
            Type* elem = ty->getPointerElementType();
            if (elem->isStructTy()) {
                Value* bufPtr = structGen->getMemberPtr(v, "_buffer");
                if (bufPtr)
                    return Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
            }
            if (elem->isIntegerTy(8)) return v;
        }
        return v;
    }

    // read_file(path: String) -> String
    // Lowers to fopen + fseek/ftell/fseek + malloc + fread + null-
    // terminate + fclose, then wraps the resulting i8* into a
    // freshly allocated String. Matches `_gen_read_file` in
    // selfhost/codegen.quirk so the C++ binary and a self-hosted
    // standalone binary emit interchangeable IR for the same
    // source.
    Value* generateReadFile(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        if (call->args.size() != 1) return nullptr;
        Value* pathVal = exprHandler(call->args[0].value.get());
        Value* pathBuf = stringBuffer(pathVal);
        if (!pathBuf) return nullptr;

        Type* i8PtrTy = Type::getInt8PtrTy(Context);
        Type* i64Ty   = Type::getInt64Ty(Context);
        Type* i32Ty   = Type::getInt32Ty(Context);

        Function* fopenFn  = TheModule->getFunction("fopen");
        Function* fcloseFn = TheModule->getFunction("fclose");
        Function* fseekFn  = TheModule->getFunction("fseek");
        Function* ftellFn  = TheModule->getFunction("ftell");
        Function* freadFn  = TheModule->getFunction("fread");
        Function* mallocFn = TheModule->getFunction("malloc");
        if (!fopenFn || !fcloseFn || !fseekFn || !ftellFn || !freadFn || !mallocFn)
            return nullptr;

        Value* mode = Builder.CreateGlobalStringPtr("r");
        Value* fp   = Builder.CreateCall(fopenFn, {pathBuf, mode});
        Builder.CreateCall(fseekFn, {fp, ConstantInt::get(i64Ty, 0), ConstantInt::get(i32Ty, 2)});
        Value* size = Builder.CreateCall(ftellFn, {fp});
        Builder.CreateCall(fseekFn, {fp, ConstantInt::get(i64Ty, 0), ConstantInt::get(i32Ty, 0)});
        Value* allocSize = Builder.CreateAdd(size, ConstantInt::get(i64Ty, 1));
        Value* buf = Builder.CreateCall(mallocFn, {allocSize});
        Builder.CreateCall(freadFn,
            {buf, ConstantInt::get(i64Ty, 1), size, fp});
        Value* nulSlot = Builder.CreateGEP(Type::getInt8Ty(Context), buf, size);
        Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Context), 0), nulSlot);
        Builder.CreateCall(fcloseFn, {fp});
        // Wrap raw i8* into a String object so callers (typed
        // `String`) can use string methods.
        (void)i8PtrTy;
        return structGen->allocateAndInit("String", {buf});
    }

    // write_file(path: String, content: String) -> Int
    // Lowers to fopen("w") + strlen(content) + fwrite + fclose.
    // Returns 0 (the value the selfhost lowering also returns).
    Value* generateWriteFile(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        if (call->args.size() != 2) return nullptr;
        Value* pathVal    = exprHandler(call->args[0].value.get());
        Value* contentVal = exprHandler(call->args[1].value.get());
        Value* pathBuf    = stringBuffer(pathVal);
        Value* contentBuf = stringBuffer(contentVal);
        if (!pathBuf || !contentBuf) return nullptr;

        Type* i64Ty = Type::getInt64Ty(Context);

        Function* fopenFn  = TheModule->getFunction("fopen");
        Function* fcloseFn = TheModule->getFunction("fclose");
        Function* fwriteFn = TheModule->getFunction("fwrite");
        Function* strlenFn = TheModule->getFunction("strlen");
        if (!strlenFn) {
            FunctionType* ft = FunctionType::get(i64Ty,
                {Type::getInt8PtrTy(Context)}, false);
            strlenFn = Function::Create(ft, Function::ExternalLinkage,
                                        "strlen", TheModule);
        }
        if (!fopenFn || !fcloseFn || !fwriteFn) return nullptr;

        Value* mode = Builder.CreateGlobalStringPtr("w");
        Value* fp   = Builder.CreateCall(fopenFn, {pathBuf, mode});
        Value* len  = Builder.CreateCall(strlenFn, {contentBuf});
        Builder.CreateCall(fwriteFn,
            {contentBuf, ConstantInt::get(i64Ty, 1), len, fp});
        Builder.CreateCall(fcloseFn, {fp});
        return ConstantInt::get(Type::getInt32Ty(Context), 0);
    }

    // Convert an already-evaluated LLVM Value* to a String*.
    // Shared by str(), write(), and the struct branch of generatePrint.
    Value* valueToString(Value* val) {
        if (!val) return nullptr;
        Type* ty = val->getType();

        if (ty->isPointerTy()) {
            Type* elem = ty->getPointerElementType();
            if (elem->isStructTy()) {
                StructType* st = cast<StructType>(elem);
                std::string sName = st->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                size_t dotPos = sName.find('.');
                if (dotPos != std::string::npos && std::isdigit(sName[dotPos + 1]))
                    sName = sName.substr(0, dotPos);

                if (sName == "String") return val;
                if (sName == "Any") {
                    Function* fn = TheModule->getFunction("Core_Primitives_Any_to_string");
                    if (fn) return Builder.CreateCall(fn, {val});
                }
                // User-defined struct — call its __str
                Value* result = structGen->generateStrCall(val, sName);
                if (result) return result;
                // No __str: return "<TypeName>"
                Value* raw = Builder.CreateGlobalStringPtr("<" + sName + ">");
                return structGen->allocateAndInit("String", {raw});
            }
            // i8* opaque — tagged int or boxed Any; quirk_opaque_to_string always
            // returns String* in memory even though its C declaration says void*.
            // Bitcast the i8* result to String* so the caller can access _buffer.
            if (elem->isIntegerTy(8)) {
                Function* fn = TheModule->getFunction("quirk_opaque_to_string");
                if (!fn) {
                    FunctionType* ft = FunctionType::get(
                        Type::getInt8PtrTy(Context),
                        {Type::getInt8PtrTy(Context)}, false);
                    fn = Function::Create(ft, Function::ExternalLinkage,
                                          "quirk_opaque_to_string", TheModule);
                }
                Value* raw = Builder.CreateCall(fn, {val});
                Type* strPtrTy = structGen->getStringPtrType();
                if (strPtrTy != Type::getInt8PtrTy(Context))
                    return Builder.CreateBitCast(raw, strPtrTy);
                return raw;
            }
        }

        if (ty->isIntegerTy(1)) {
            Value* raw = Builder.CreateSelect(val,
                Builder.CreateGlobalStringPtr("true"),
                Builder.CreateGlobalStringPtr("false"));
            return structGen->allocateAndInit("String", {raw});
        }
        if (ty->isIntegerTy(8)) {
            Function* fn = TheModule->getFunction("Core_Primitives_Char_str");
            if (fn) {
                Value* r = Builder.CreateCall(fn, {val});
                if (r->getType()->isPointerTy() &&
                    r->getType()->getPointerElementType()->isIntegerTy(8))
                    return structGen->allocateAndInit("String", {r});
                return r;
            }
        }
        if (ty->isIntegerTy()) {
            Function* fn = TheModule->getFunction("Core_Primitives_Int_str");
            if (fn) {
                Value* r = Builder.CreateCall(fn, {val});
                if (r->getType()->isPointerTy() &&
                    r->getType()->getPointerElementType()->isIntegerTy(8))
                    return structGen->allocateAndInit("String", {r});
                return r;
            }
            // Fallback: sprintf into a temp buffer
            Value* raw = Builder.CreateGlobalStringPtr("<int>");
            return structGen->allocateAndInit("String", {raw});
        }
        if (ty->isDoubleTy()) {
            Function* fn = TheModule->getFunction("Core_Primitives_Double_str");
            if (fn) {
                Value* r = Builder.CreateCall(fn, {val});
                if (r->getType()->isPointerTy() &&
                    r->getType()->getPointerElementType()->isIntegerTy(8))
                    return structGen->allocateAndInit("String", {r});
                return r;
            }
        }
        Value* raw = Builder.CreateGlobalStringPtr("<unknown>");
        return structGen->allocateAndInit("String", {raw});
    }

    // str(x) -> String  — first-class Any-to-string conversion
    Value* generateStr(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        if (call->args.empty()) {
            Value* empty = Builder.CreateGlobalStringPtr("");
            return structGen->allocateAndInit("String", {empty});
        }
        Value* val = exprHandler(call->args[0].value.get());
        return valueToString(val);
    }

    // write(x) / writeln(x) — print without/with trailing newline
    Value* generateWrite(CallNode* call, std::function<Value*(Node*)> exprHandler, bool newline) {
        Function* printfFunc = TheModule->getFunction("printf");
        if (!printfFunc) return nullptr;

        for (auto& arg : call->args) {
            Value* val = exprHandler(arg.value.get());
            if (!val) continue;

            Type* ty = val->getType();
            Value* fmtStr = Builder.CreateGlobalStringPtr(newline ? "%s\n" : "%s");
            Value* fmtInt = Builder.CreateGlobalStringPtr(newline ? "%d\n" : "%d");
            Value* fmtFlt = Builder.CreateGlobalStringPtr(newline ? "%f\n" : "%f");

            if (ty->isIntegerTy(1)) {
                Value* s = Builder.CreateSelect(val,
                    Builder.CreateGlobalStringPtr(newline ? "true\n" : "true"),
                    Builder.CreateGlobalStringPtr(newline ? "false\n" : "false"));
                Builder.CreateCall(printfFunc, {Builder.CreateGlobalStringPtr("%s"), s});
            } else if (ty->isIntegerTy(8)) {
                Builder.CreateCall(printfFunc, {Builder.CreateGlobalStringPtr(newline ? "%c\n" : "%c"), val});
            } else if (ty->isIntegerTy()) {
                Builder.CreateCall(printfFunc, {fmtInt, val});
            } else if (ty->isDoubleTy()) {
                Builder.CreateCall(printfFunc, {fmtFlt, val});
            } else {
                // Anything else: convert to String then print the buffer
                Value* strObj = valueToString(val);
                if (strObj && strObj->getType()->isPointerTy() &&
                    strObj->getType()->getPointerElementType()->isStructTy()) {
                    Value* bufPtr = structGen->getMemberPtr(strObj, "_buffer");
                    if (bufPtr) {
                        Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                        Builder.CreateCall(printfFunc, {fmtStr, cStr});
                    }
                } else if (strObj && strObj->getType()->isPointerTy() &&
                           strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                    Builder.CreateCall(printfFunc, {fmtStr, strObj});
                }
            }
        }
        return nullptr;
    }

    Value* generateType(CallNode* call, std::function<Value*(Node*)> exprHandler) {
        if (call->args.empty()) return nullptr;
        Value* val = exprHandler(call->args[0].value.get());
        if (!val) return nullptr;

        Type* llvmType = val->getType();

        auto makeTypeString = [&](const std::string& name) -> Value* {
            Value* raw = Builder.CreateGlobalStringPtr(name);
            std::vector<Value*> args = {raw};
            return structGen->allocateAndInit("String", args);
        };

        // Pointer types: struct*, Any*, i8*
        if (llvmType->isPointerTy()) {
            Type* elem = llvmType->getPointerElementType();
            if (elem->isStructTy()) {
                StructType* st = cast<StructType>(elem);
                std::string structName = st->getName().str();
                if (structName.find("struct.") == 0) structName = structName.substr(7);
                size_t dotPos = structName.find('.');
                if (dotPos != std::string::npos && std::isdigit(structName[dotPos + 1]))
                    structName = structName.substr(0, dotPos);

                if (structName == "Any") {
                    // Runtime dispatch via the Any tag
                    Function* getType = TheModule->getFunction("Core_Primitives_Any_get_type");
                    if (getType) return Builder.CreateCall(getType, {val});
                }
                return makeTypeString(structName);
            }
            // i8* opaque — may be a tagged integer or boxed Any*; use the safe helper
            // that checks the pointer value before dereferencing (avoids segfault on tagged ints).
            Function* opaqueGetType = TheModule->getFunction("quirk_opaque_get_type");
            if (!opaqueGetType) {
                Type* retTy = TheModule->getFunction("Core_Primitives_Any_get_type")
                    ? TheModule->getFunction("Core_Primitives_Any_get_type")->getReturnType()
                    : (Type*)Type::getInt8PtrTy(Context);
                FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                opaqueGetType = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_get_type", TheModule);
            }
            return Builder.CreateCall(opaqueGetType, {val});
        }

        // Primitive LLVM types — known statically
        if (llvmType->isIntegerTy(1))  return makeTypeString("Bool");
        if (llvmType->isIntegerTy())   return makeTypeString("Int");
        if (llvmType->isDoubleTy())    return makeTypeString("Double");
        return makeTypeString("Unknown");
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
            Function* f = TheModule->getFunction("Core_Primitives_Bool_str");
            if (f) return Builder.CreateCall(f, {val});
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
                    Value* bufPtr = structGen->getMemberPtr(val, "_buffer");
                    if (bufPtr) {
                        Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                        Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                        Builder.CreateCall(printfFunc, {fmt, cStr});
                    }
                } else if (structName == "Any") {
                    Function* anyStr = TheModule->getFunction("Core_Primitives_Any_to_string");
                    if (anyStr) {
                        Value* strObj = Builder.CreateCall(anyStr, {val});
                        Value* bufPtr = structGen->getMemberPtr(strObj, "_buffer");
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
                            Value* bufPtr = structGen->getMemberPtr(strObj, "_buffer");
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
                    } else if (structGen->inheritsFrom(structName, "String")) {
                        // String subclass with no __str override — read the String buffer directly.
                        // The layout is identical to String (no __type_id since String is
                        // extern-backed), so bitcasting to String* is safe.
                        StructType* strTy = StructType::getTypeByName(Context, "String");
                        if (!strTy) strTy = StructType::getTypeByName(Context, "struct.String");
                        if (strTy) {
                            Value* asStr = Builder.CreateBitCast(val, PointerType::getUnqual(strTy));
                            Value* bufPtr = structGen->getMemberPtr(asStr, "buffer");
                            if (bufPtr) {
                                Value* cStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufPtr);
                                Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                                Builder.CreateCall(printfFunc, {fmt, cStr});
                            }
                        }
                    } else {
                        Value* fmt = Builder.CreateGlobalStringPtr("<%s at %p>\n");
                        Value* nameVal = Builder.CreateGlobalStringPtr(structName);
                        Builder.CreateCall(printfFunc, {fmt, nameVal, val});
                    }
                }
            } else if (type->isIntegerTy()) {
                if (type->getIntegerBitWidth() == 1) {
                    Value* trueStr  = Builder.CreateGlobalStringPtr("true");
                    Value* falseStr = Builder.CreateGlobalStringPtr("false");
                    Value* str = Builder.CreateSelect(val, trueStr, falseStr, "bool_str");
                    Value* fmt = Builder.CreateGlobalStringPtr("%s\n");
                    Builder.CreateCall(printfFunc, {fmt, str});
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
