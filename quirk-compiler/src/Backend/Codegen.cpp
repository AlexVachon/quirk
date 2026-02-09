#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "ast.hpp"

#include "ControlFlowGen.cpp"
#include "MathGen.cpp"
#include "StructGen.cpp"
#include "BuiltinGen.cpp"
#include "TypeExtensions.cpp"
#include "TypeGen.cpp"
#include "VariableGen.cpp"

#include "../Modules/ModuleRegistry.cpp"

using namespace llvm;

std::string printType(Type* t) {
    std::string type_str;
    raw_string_ostream rso(type_str);
    t->print(rso);
    return rso.str();
}

class LLVMCodegen {
    LLVMContext Context;
    std::unique_ptr<Module> TheModule;
    IRBuilder<> Builder;

    std::map<std::string, bool> variadicFunctions;

    std::map<std::string, StructType*> StructTypes;

    std::unique_ptr<TypeGen> typeGen;
    std::unique_ptr<ControlFlowGen> flowGen;
    std::unique_ptr<StructGen> structGen;
    std::unique_ptr<BuiltinGen> builtinGen;
    std::unique_ptr<VariableGen> varGen;
    std::unique_ptr<MathGen> mathGen;

    std::unique_ptr<ModuleRegistry> moduleRegistry;

    std::unique_ptr<TypeExtensions> typeExtensions;

   public:
    static std::string currentCodegenClass;

    LLVMCodegen() : Builder(Context) {
        TheModule = std::make_unique<Module>("ApexCompiler", Context);
        typeGen = std::make_unique<TypeGen>(Context, StructTypes);
        flowGen =
            std::make_unique<ControlFlowGen>(Context, TheModule.get(), Builder);
        structGen = std::make_unique<StructGen>(Context, TheModule.get(),
                                                Builder, StructTypes);
        builtinGen = std::make_unique<BuiltinGen>(Context, TheModule.get(),
                                                  Builder, structGen.get());
        structGen->setBuiltinGen(builtinGen.get());

        typeExtensions =
            std::make_unique<TypeExtensions>(Context, TheModule.get(), Builder,
                                             structGen.get(), builtinGen.get());

        varGen = std::make_unique<VariableGen>(Context, Builder);
        mathGen = std::make_unique<MathGen>(Context, Builder);
        moduleRegistry = std::make_unique<ModuleRegistry>();
    }

    void compile(const std::vector<std::unique_ptr<Node>>& nodes,
                 raw_ostream& out = errs()) {
        std::cerr << "[CG] Starting Compilation..." << std::endl;
        builtinGen->Initialize();

        // Modules
        moduleRegistry->registerAll(Context, StructTypes, structGen.get(),
                                    TheModule.get());
        std::cerr << "[CG] Modules Registered. Structs: " << StructTypes.size()
                  << std::endl;

        // =========================================================
        // PASS 0: User Structs (Two-Phase for Circular Deps)
        // =========================================================
        std::cerr << "[CG] Pass 0: User Structs..." << std::endl;

        // Phase A: Forward Declare all structs (Opaque)
        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                if (!StructTypes.count(s->name)) {
                    StructTypes[s->name] = StructType::create(Context, s->name);
                }
            }
        }

        // Phase B: Define Bodies
        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                StructType* st = StructTypes[s->name];

                if (!st->isOpaque()) {
                    std::vector<std::string> fieldNames;
                    for (const auto& field : s->fields)
                        fieldNames.push_back(field.name);
                    structGen->registerStructLayout(s->name, fieldNames);
                    continue;
                }

                std::vector<Type*> elementTypes;
                std::vector<std::string> fieldNames;

                // 1. Add Parent Fields (Inheritance)
                for (const auto& parent : s->parents) {
                    if (StructTypes.count(parent)) {
                        elementTypes.push_back(StructTypes[parent]);
                    }
                }

                // 2. Add Own Fields
                for (const auto& field : s->fields) {
                    Type* t = typeGen->getLLVMType(field.type);

                    // Structs as Fields should be Pointers
                    if (t->isStructTy()) {
                        t = PointerType::getUnqual(t);
                    }
                    elementTypes.push_back(t);
                    fieldNames.push_back(field.name);
                }

                st->setBody(elementTypes);
                structGen->registerStructLayout(s->name, fieldNames);
            }
        }

        // PASS 1: Pre-declare helper functions
        std::cerr << "[CG] Pass 1: Function Declarations..." << std::endl;
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name == "main" || builtinGen->isBuiltin(func->name))
                    continue;
                if (TheModule->getFunction(func->name) && !func->isExtern)
                    continue;

                Type* retTy = typeGen->getFunctionReturnType(func->returnType);
                if (func->returnType.empty()) {
                    for (auto& stmt : func->body) {
                        if (dynamic_cast<ReturnNode*>(stmt.get())) {
                            retTy = Type::getInt32Ty(Context);
                            break;
                        }
                    }
                }

                if (!func->parameters.empty() &&
                    func->parameters.back().isVariadic) {
                    variadicFunctions[func->name] = true;
                }

                std::vector<Type*> argTypes;
                if (!func->cls.empty() && !func->isStatic)
                    argTypes.push_back(typeGen->getLLVMType(func->cls));
                for (const auto& param : func->parameters)
                    argTypes.push_back(typeGen->getLLVMType(param.type));

                FunctionType* FT = FunctionType::get(retTy, argTypes, false);
                Function::Create(FT, Function::ExternalLinkage, func->name,
                                 TheModule.get());
            }
        }

        // PASS 2: Compile bodies
        std::cerr << "[CG] Pass 2: Function Bodies..." << std::endl;
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name != "main") {
                    std::cerr << "  -> Compiling " << func->name << "..."
                              << std::endl;
                    compileFunction(func);
                }
            }
        }

        // PASS 3: Main
        std::cerr << "[CG] Pass 3: Main..." << std::endl;
        FunctionType* mainType =
            FunctionType::get(Type::getInt32Ty(Context), {}, false);
        Function* mainFunc = Function::Create(
            mainType, Function::ExternalLinkage, "main", TheModule.get());
        Builder.SetInsertPoint(BasicBlock::Create(Context, "entry", mainFunc));
        varGen->clear();
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name == "main") {
                    std::cerr << "[CG] Found main function. Generating body..."
                              << std::endl;  // Log this
                    for (const auto& stmt : func->body)
                        handleStatement(stmt.get(), mainFunc);
                }
            }
        }
        if (!Builder.GetInsertBlock()->getTerminator()) {
            Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
        }

        std::cerr << "[CG] Finished. Printing..." << std::endl;
        TheModule->print(out, nullptr);
    }

    void compileFunction(FunctionNode* node) {
        std::string prevClass = currentCodegenClass;
        currentCodegenClass = node->cls;

        Function* F = TheModule->getFunction(node->name);
        if (!F || node->isExtern) {
            currentCodegenClass = prevClass;
            return;
        }

        BasicBlock* prevBB = Builder.GetInsertBlock();
        BasicBlock* BB = BasicBlock::Create(Context, "entry", F);
        Builder.SetInsertPoint(BB);
        varGen->clear();

        size_t paramIdx = 0;
        auto argIt = F->arg_begin();

        // Define Self
        if (!node->cls.empty() && !node->isStatic) {
            llvm::Argument* selfArg = &*argIt++;
            selfArg->setName("self");
            std::cerr << "     [Arg] self" << std::endl;
            varGen->defineArgument("self", selfArg);
        }

        // Define Params
        for (; argIt != F->arg_end(); ++argIt) {
            if (paramIdx >= node->parameters.size())
                break;
            std::string argName = node->parameters[paramIdx++].name;
            argIt->setName(argName);
            std::cerr << "     [Arg] " << argName << std::endl;
            varGen->defineArgument(argName, &*argIt);
        }

        // Statements
        int stmtIdx = 0;
        for (const auto& stmt : node->body) {
            std::cerr << "     [Stmt " << stmtIdx++ << "] Processing..."
                      << std::endl;
            handleStatement(stmt.get(), F);
        }

        if (!Builder.GetInsertBlock()->getTerminator()) {
            if (F->getReturnType()->isVoidTy())
                Builder.CreateRetVoid();
            else
                Builder.CreateRet(
                    ConstantInt::get(Type::getInt32Ty(Context), 0));
        }
        if (prevBB)
            Builder.SetInsertPoint(prevBB);
        currentCodegenClass = prevClass;
    }

    std::string unescapeString(const std::string& raw) {
        std::string res;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                switch (raw[i + 1]) {
                    case 'n':
                        res += '\n';
                        break;
                    case 't':
                        res += '\t';
                        break;
                    case 'r':
                        res += '\r';
                        break;
                    case '\\':
                        res += '\\';
                        break;
                    case '"':
                        res += '\"';
                        break;
                    default:
                        res += raw[i];
                        res += raw[i + 1];
                }
                i++;
            } else {
                res += raw[i];
            }
        }
        return res;
    }

   private:
    Value* handleExpression(Node* node);

    Value* handleCall(CallNode* call) {
        // 1. Builtin Functions
        if (auto lit = dynamic_cast<LiteralNode*>(call->callee.get())) {
            if (builtinGen->isBuiltin(lit->value)) {
                return builtinGen->handleBuiltin(
                    lit->value, call,
                    [this](Node* n) { return this->handleExpression(n); });
            }

            // 2. Struct Constructors
            if (StructTypes.count(lit->value)) {
                std::vector<Value*> args;
                for (auto& a : call->args)
                    args.push_back(handleExpression(a.value.get()));
                return structGen->allocateAndInit(lit->value, args);
            }

            // 3. Global Functions (Standard & Variadic)
            Function* func = TheModule->getFunction(lit->value);
            if (!func && !currentCodegenClass.empty())
                func = TheModule->getFunction(currentCodegenClass + "_" +
                                              lit->value);

            if (!func)
                return nullptr;

            bool isVariadic = variadicFunctions.count(lit->value);
            size_t fixedArgCount = func->arg_size();
            if (isVariadic)
                fixedArgCount--;

            std::vector<Value*> finalArgs;
            std::vector<Value*> variadicBundle;

            size_t argIdx = 0;
            for (auto& a : call->args) {
                Value* argVal = handleExpression(a.value.get());

                if (argIdx < fixedArgCount) {
                    // --- FIXED ARGUMENT ---
                    Type* expectedType =
                        func->getFunctionType()->getParamType(argIdx);

                    // FIX: Auto-box c_string -> String struct
                    if (argVal->getType()->isPointerTy() &&
                        argVal->getType()->getPointerElementType()->isIntegerTy(
                            8) &&
                        expectedType->isPointerTy() &&
                        expectedType->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(
                            expectedType->getPointerElementType());
                        if (st->getName() == "String") {
                            std::vector<Value*> ctorArgs = {argVal};
                            argVal =
                                structGen->allocateAndInit("String", ctorArgs);
                        }
                    }
                    // Implicit Casts
                    else if (argVal->getType() != expectedType) {
                        if (argVal->getType()->isIntegerTy() &&
                            expectedType->isIntegerTy())
                            argVal = Builder.CreateIntCast(argVal, expectedType,
                                                           true);
                        else if (argVal->getType()->isIntegerTy() &&
                                 expectedType->isPointerTy())
                            argVal =
                                Builder.CreateIntToPtr(argVal, expectedType);
                        else if (argVal->getType()->isPointerTy() &&
                                 expectedType->isPointerTy())
                            argVal =
                                Builder.CreateBitCast(argVal, expectedType);
                        else if (argVal->getType()->isIntegerTy() &&
                                 expectedType->isDoubleTy())
                            argVal = Builder.CreateSIToFP(argVal, expectedType);
                    }
                    finalArgs.push_back(argVal);
                } else {
                    // --- VARIADIC ARGUMENT ---
                    if (argVal->getType()->isIntegerTy()) {
                        argVal = Builder.CreateIntToPtr(
                            argVal, Type::getInt8PtrTy(Context));
                    } else if (argVal->getType() !=
                               Type::getInt8PtrTy(Context)) {
                        argVal = Builder.CreateBitCast(
                            argVal, Type::getInt8PtrTy(Context));
                    }
                    variadicBundle.push_back(argVal);
                }
                argIdx++;
            }

            if (isVariadic) {
                Value* listObj =
                    structGen->createListFromValues(variadicBundle);
                finalArgs.push_back(listObj);
            }

            return Builder.CreateCall(func, finalArgs);
        }

        // 4. Method Calls
        if (auto member = dynamic_cast<MemberAccessNode*>(call->callee.get())) {
            Value* objPtr = handleExpression(member->object.get());
            std::string typeName;

            // Static Call
            if (!objPtr) {
                if (auto lit =
                        dynamic_cast<LiteralNode*>(member->object.get())) {
                    if (StructTypes.count(lit->value)) {
                        typeName = lit->value;
                    }
                }
                if (typeName.empty())
                    return nullptr;
            } else {
                // Instance Call

                // 1. Handle Structs (passed by value? store to stack)
                if (objPtr->getType()->isStructTy()) {
                    Value* mem = Builder.CreateAlloca(objPtr->getType());
                    Builder.CreateStore(objPtr, mem);
                    objPtr = mem;
                }

                // 2. Handle Double Pointers (Ptr -> Ptr -> Struct)
                if (objPtr->getType()->isPointerTy() &&
                    objPtr->getType()->getPointerElementType()->isPointerTy()) {
                    objPtr = Builder.CreateLoad(
                        objPtr->getType()->getPointerElementType(), objPtr);
                }

                // 3. Handle C-String Auto-boxing (char* -> String)
                if (objPtr->getType()->isPointerTy() &&
                    objPtr->getType()->getPointerElementType()->isIntegerTy(
                        8)) {
                    std::vector<Value*> ctorArgs = {objPtr};
                    objPtr = structGen->allocateAndInit("String", ctorArgs);
                }

                // --- FIX: DETECT PRIMITIVES SAFELY ---
                if (objPtr->getType()->isIntegerTy()) {
                    if (objPtr->getType()->isIntegerTy(1)) {
                        typeName = "Bool";
                    } else {
                        typeName = "Int";
                    }
                } else if (objPtr->getType()->isDoubleTy()) {
                    typeName = "Double";
                } else if (objPtr->getType()->isPointerTy() &&
                           objPtr->getType()
                               ->getPointerElementType()
                               ->isStructTy()) {
                    StructType* st = cast<StructType>(
                        objPtr->getType()->getPointerElementType());
                    typeName = st->getName().str();
                } else {
                    std::cerr << "Error: Cannot call method on type "
                              << printType(objPtr->getType()) << std::endl;
                    return nullptr;
                }
            }

            if (objPtr) {
                Value* extResult = typeExtensions->tryHandleMethod(
                    typeName, member->memberName, objPtr, call->args,
                    [this](Node* n) { return this->handleExpression(n); });
                if (extResult)
                    return extResult;
            }

            std::string funcName = typeName + "_" + member->memberName;
            Function* func = TheModule->getFunction(funcName);
            if (!func) {
                std::cerr << "Error: Function '" << funcName << "' not found."
                          << std::endl;
                return nullptr;
            }

            std::vector<Value*> args;
            size_t argIdx = 0;

            if (objPtr) {
                args.push_back(objPtr);
                argIdx++;
            }

            for (auto& a : call->args) {
                Value* argVal = handleExpression(a.value.get());

                if (argIdx < func->arg_size()) {
                    Type* expectedType =
                        func->getFunctionType()->getParamType(argIdx);

                    // FIX: Auto-box c_string -> String struct
                    if (argVal->getType()->isPointerTy() &&
                        argVal->getType()->getPointerElementType()->isIntegerTy(
                            8) &&
                        expectedType->isPointerTy() &&
                        expectedType->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(
                            expectedType->getPointerElementType());
                        if (st->getName() == "String") {
                            std::vector<Value*> ctorArgs = {argVal};
                            argVal =
                                structGen->allocateAndInit("String", ctorArgs);
                        }
                    } else if (argVal->getType()->isIntegerTy() &&
                               expectedType->isDoubleTy()) {
                        argVal = Builder.CreateSIToFP(argVal, expectedType);
                    } else if (argVal->getType()->isIntegerTy() &&
                               expectedType->isPointerTy()) {
                        argVal = Builder.CreateIntToPtr(argVal, expectedType);
                    } else if (argVal->getType()->isPointerTy() &&
                               expectedType->isPointerTy() &&
                               argVal->getType() != expectedType) {
                        argVal = Builder.CreateBitCast(argVal, expectedType);
                    }
                }
                args.push_back(argVal);
                argIdx++;
            }
            return Builder.CreateCall(func, args);
        }
        return nullptr;
    }

    void handleStatement(Node* node, Function* parentFunc) {
        if (auto vdecl = dynamic_cast<VarDeclNode*>(node)) {
            handleVarDecl(vdecl);
        } else if (auto ifNode = dynamic_cast<IfNode*>(node))
            handleIf(ifNode, parentFunc);
        else if (auto whileNode = dynamic_cast<WhileNode*>(node))
            handleWhile(whileNode, parentFunc);
        else if (auto withNode = dynamic_cast<WithNode*>(node))
            handleWith(withNode, parentFunc);
        else if (auto forNode = dynamic_cast<ForNode*>(node)) {
            flowGen->generateFor(
                forNode, parentFunc,
                [this](Node* n) { return this->handleExpression(n); },
                [this](const std::string& name, std::vector<Value*>& args) {
                    return structGen->allocateAndInit(name, args);
                },
                [this, parentFunc](Node* n) {
                    this->handleStatement(n, parentFunc);
                },
                varGen.get());
        } else if (auto call = dynamic_cast<CallNode*>(node))
            handleCall(call);
        else if (auto ret = dynamic_cast<ReturnNode*>(node)) {
            if (ret->expression) {
                Value* retVal = handleExpression(ret->expression.get());

                if (!retVal) {
                    std::cerr << "COMPILER ERROR: Return expression null in "
                              << parentFunc->getName().str() << std::endl;
                    exit(1);
                }

                Type* expectedType = parentFunc->getReturnType();
                if (retVal->getType() != expectedType) {
                    if (retVal->getType()->isIntegerTy() &&
                        expectedType->isIntegerTy())
                        retVal =
                            Builder.CreateIntCast(retVal, expectedType, true);
                    else if (retVal->getType()->isIntegerTy() &&
                             expectedType->isPointerTy())
                        retVal = Builder.CreateIntToPtr(retVal, expectedType);
                    else if (retVal->getType()->isPointerTy() &&
                             expectedType->isIntegerTy())
                        retVal = Builder.CreatePtrToInt(retVal, expectedType);
                    else if (retVal->getType()->isIntegerTy() &&
                             expectedType->isDoubleTy())
                        retVal = Builder.CreateSIToFP(retVal, expectedType);
                }
                Builder.CreateRet(retVal);
            } else {
                Builder.CreateRetVoid();
            }
        } else if (auto delNode = dynamic_cast<DeleteNode*>(node)) {
            Value* objPtr = handleExpression(delNode->target.get());
            if (objPtr && objPtr->getType()->isPointerTy() &&
                objPtr->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(
                    objPtr->getType()->getPointerElementType());
                Function* dtorFunc =
                    TheModule->getFunction(st->getName().str() + "__del");
                if (dtorFunc)
                    Builder.CreateCall(dtorFunc, {objPtr});
            }
            Value* rawPtr =
                Builder.CreateBitCast(objPtr, Type::getInt8PtrTy(Context));
            Builder.CreateCall(TheModule->getFunction("free"), {rawPtr});
        }
    }

    void handleVarDecl(VarDeclNode* vdecl) {
        Value* val = handleExpression(vdecl->expression.get());
        if (!val || val->getType()->isVoidTy())
            return;

        // --- NEW: Implicit String Conversion ---
        if (!vdecl->typeAnnotation.empty()) {
            if (vdecl->typeAnnotation == "String" &&
                val->getType()->isPointerTy() &&
                val->getType()->getPointerElementType()->isIntegerTy(8)) {
                std::vector<Value*> args = {val};
                val = structGen->allocateAndInit("String", args);
            }
        }
        // ---------------------------------------

        if (!vdecl->typeAnnotation.empty()) {
            Type* targetType = typeGen->getLLVMType(vdecl->typeAnnotation);
            if (val->getType()->isIntegerTy() && targetType->isPointerTy())
                val = Builder.CreateIntToPtr(val, targetType);
            else if (val->getType()->isPointerTy() &&
                     targetType->isPointerTy() && val->getType() != targetType)
                val = Builder.CreateBitCast(val, targetType);
            else if (val->getType()->isIntegerTy() && targetType->isDoubleTy())
                val = Builder.CreateSIToFP(val, targetType);
        }

        if (auto lhs = dynamic_cast<LiteralNode*>(vdecl->lhs.get())) {
            if (!varGen->exists(lhs->value))
                varGen->defineLocalVariable(lhs->value, val);
            else
                varGen->updateLocalVariable(lhs->value, val);
        } else if (auto member =
                       dynamic_cast<MemberAccessNode*>(vdecl->lhs.get())) {
            Value* objPtr = handleExpression(member->object.get());
            if (objPtr->getType()->isPointerTy() &&
                objPtr->getType()->getPointerElementType()->isPointerTy())
                objPtr = Builder.CreateLoad(
                    objPtr->getType()->getPointerElementType(), objPtr);

            Value* memberPtr =
                structGen->getMemberPtr(objPtr, member->memberName);
            if (memberPtr) {
                Type* fieldType = memberPtr->getType()->getPointerElementType();
                if (val->getType() != fieldType) {
                    if (val->getType()->isIntegerTy() &&
                        fieldType->isIntegerTy())
                        val = Builder.CreateIntCast(val, fieldType, true);
                    else if (val->getType()->isPointerTy() &&
                             fieldType->isPointerTy())
                        val = Builder.CreateBitCast(val, fieldType);
                    else if (val->getType()->isIntegerTy() &&
                             fieldType->isPointerTy())
                        val = Builder.CreateIntToPtr(val, fieldType);
                    else if (val->getType()->isIntegerTy() &&
                             fieldType->isDoubleTy())
                        val = Builder.CreateSIToFP(val, fieldType);
                }
                Builder.CreateStore(val, memberPtr);
            }
        } else if (auto binOp = dynamic_cast<BinaryOpNode*>(vdecl->lhs.get())) {
            if (binOp->op == "[]") {
                Value* ptr = handleExpression(binOp->left.get());
                Value* index = handleExpression(binOp->right.get());

                // 1. Handle Structs (e.g. List, Map)
                if (ptr->getType()->isPointerTy() &&
                    ptr->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(
                        ptr->getType()->getPointerElementType());
                    Function* func =
                        TheModule->getFunction(st->getName().str() + "___set");
                    
                    if (func) {
                        // --- FIX START: Auto-boxing for __set ---
                        // Ensure the value matches the 3rd argument (item) type
                        if (func->arg_size() >= 3) {
                            Type* expectedType = func->getFunctionType()->getParamType(2);
                            
                            if (val->getType() != expectedType) {
                                // Box Integer -> Ptr (e.g. i32 -> i8*)
                                if (val->getType()->isIntegerTy() && expectedType->isPointerTy()) {
                                    val = Builder.CreateIntToPtr(val, expectedType);
                                } 
                                // Bitcast Ptr -> Ptr (e.g. String* -> i8*)
                                else if (val->getType()->isPointerTy() && expectedType->isPointerTy()) {
                                    val = Builder.CreateBitCast(val, expectedType);
                                }
                                // Double -> Ptr (if needed) or other casts could go here
                            }
                        }
                        // --- FIX END ---
                        
                        Builder.CreateCall(func, {ptr, index, val});
                        return;
                    }
                }
                
                // 2. Fallback for raw arrays (non-structs)
                if (val->getType()->isPointerTy()) {
                    ptr = Builder.CreateBitCast(
                        ptr,
                        PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                    Value* elementPtr = Builder.CreateGEP(
                        Type::getInt8PtrTy(Context), ptr, index);
                    Builder.CreateStore(val, elementPtr);
                    return;
                }
                if (val->getType()->isIntegerTy()) {
                    Value* boxedVal = Builder.CreateIntToPtr(
                        val, Type::getInt8PtrTy(Context));
                    ptr = Builder.CreateBitCast(
                        ptr,
                        PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                    Value* elementPtr = Builder.CreateGEP(
                        Type::getInt8PtrTy(Context), ptr, index);
                    Builder.CreateStore(boxedVal, elementPtr);
                    return;
                }
                ptr = Builder.CreateBitCast(ptr, Type::getInt32PtrTy(Context));
                Builder.CreateStore(
                    val,
                    Builder.CreateGEP(Type::getInt32Ty(Context), ptr, index));
            }
        }
    }

    void handleIf(IfNode* node, Function* parentFunc) {
        flowGen->generateIf(
            node, parentFunc,
            [this](Node* n) { return this->handleExpression(n); },
            [this, parentFunc](Node* n) {
                this->handleStatement(n, parentFunc);
            });
    }

    void handleWhile(WhileNode* node, Function* parentFunc) {
        flowGen->generateWhile(
            node, parentFunc,
            [this](Node* n) { return this->handleExpression(n); },
            [this, parentFunc](Node* n) {
                this->handleStatement(n, parentFunc);
            });
    }

    Value* handleConstructor(ConstructorNode* node) {
        return structGen->generateConstructor(
            node, [this](Node* n) { return this->handleExpression(n); });
    }

    void handleWith(WithNode* node, Function* parentFunc) {
        Value* resource = handleExpression(node->resource.get());
        if (!resource)
            return;

        if (!resource->getType()->isPointerTy() ||
            !resource->getType()->getPointerElementType()->isStructTy()) {
            std::cerr << "Error: 'with' resource must be a Struct instance."
                      << std::endl;
            exit(1);
        }
        StructType* st =
            cast<StructType>(resource->getType()->getPointerElementType());
        std::string typeName = st->getName().str();

        std::string enterName = typeName + "___enter";
        Function* enterFunc = TheModule->getFunction(enterName);
        if (!enterFunc)
            exit(1);
        Value* contextVal = Builder.CreateCall(enterFunc, {resource});

        varGen->defineLocalVariable(node->varName, contextVal);

        for (const auto& stmt : node->body) {
            handleStatement(stmt.get(), parentFunc);
        }

        std::string exitName = typeName + "___exit";
        Function* exitFunc = TheModule->getFunction(exitName);
        if (exitFunc)
            Builder.CreateCall(exitFunc, {resource});
    }
};

Value* LLVMCodegen::handleExpression(Node* node) {
    if (auto call = dynamic_cast<CallNode*>(node))
        return handleCall(call);

    if (auto lit = dynamic_cast<LiteralNode*>(node)) {
        if (lit->value == "true")
            return ConstantInt::getTrue(Context);
        if (lit->value == "false")
            return ConstantInt::getFalse(Context);
        if (std::isdigit(lit->value[0])) {
            if (lit->value.find('.') != std::string::npos)
                return ConstantFP::get(Context, APFloat(std::stod(lit->value)));
            return ConstantInt::get(Type::getInt32Ty(Context),
                                    std::stoi(lit->value));
        }
        if (lit->value.size() >= 2 && lit->value.front() == '"') {
            return Builder.CreateGlobalStringPtr(
                unescapeString(lit->value.substr(1, lit->value.size() - 2)));
        }
        if (varGen->exists(lit->value))
            return varGen->resolveVariable(lit->value);
    }

    if (auto arr = dynamic_cast<ListLiteralNode*>(node)) {
        return structGen->generateListLiteral(
            arr, [this](Node* n) { return this->handleExpression(n); });
    }

    if (auto binOp = dynamic_cast<BinaryOpNode*>(node)) {
        if (binOp->op == "not")
            return mathGen->generateNot(handleExpression(binOp->left.get()));
        if (binOp->op == "and" || binOp->op == "or")
            return mathGen->generateLogicOp(
                binOp->op, handleExpression(binOp->left.get()),
                binOp->right.get(),
                [this](Node* n) { return this->handleExpression(n); });

        Value* L = handleExpression(binOp->left.get());
        Value* R = handleExpression(binOp->right.get());
        if (!L || !R)
            return nullptr;

        // 1. Array Access
        if (binOp->op == "[]") {
            if (L->getType()->isPointerTy() &&
                L->getType()->getPointerElementType()->isIntegerTy(8)) {
                Value* ptr = Builder.CreateBitCast(
                    L, PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                return Builder.CreateLoad(
                    Type::getInt8PtrTy(Context),
                    Builder.CreateGEP(Type::getInt8PtrTy(Context), ptr, R));
            }
            if (L->getType()->isPointerTy() &&
                L->getType()->getPointerElementType()->isStructTy()) {
                StructType* st =
                    cast<StructType>(L->getType()->getPointerElementType());
                std::string funcName = st->getName().str() + "___get";
                if (auto* func = TheModule->getFunction(funcName))
                    return Builder.CreateCall(func, {L, R});
            }
            Value* ptr = Builder.CreateBitCast(L, Type::getInt32PtrTy(Context));
            return Builder.CreateLoad(
                Type::getInt32Ty(Context),
                Builder.CreateGEP(Type::getInt32Ty(Context), ptr, R));
        }

        // 2. Handle "Struct != 0" (Null Checks)
        if (binOp->op == "==" || binOp->op == "!=") {
            if (L->getType()->isPointerTy() && R->getType()->isIntegerTy())
                R = Builder.CreateIntToPtr(R, L->getType());
            else if (L->getType()->isIntegerTy() && R->getType()->isPointerTy())
                L = Builder.CreateIntToPtr(L, R->getType());

            if (L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
                if (binOp->op == "==")
                    return Builder.CreateICmpEQ(L, R, "ptr_eq");
                if (binOp->op == "!=")
                    return Builder.CreateICmpNE(L, R, "ptr_ne");
            }
        }

        // --- 5. Robust String Auto-Boxing & Coercion ---
        
        bool L_is_cstring = (L->getType()->isPointerTy() && 
                             L->getType()->getPointerElementType()->isIntegerTy(8));
        bool R_is_cstring = (R->getType()->isPointerTy() && 
                             R->getType()->getPointerElementType()->isIntegerTy(8));

        bool L_is_StringObj = false;
        if (L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isStructTy()) {
             StructType* st = cast<StructType>(L->getType()->getPointerElementType());
             if (st->getName() == "String") L_is_StringObj = true;
        }

        bool R_is_StringObj = false;
        if (R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isStructTy()) {
             StructType* st = cast<StructType>(R->getType()->getPointerElementType());
             if (st->getName() == "String") R_is_StringObj = true;
        }

        if (binOp->op == "+") {
            // Case A: String (Obj/CStr) + Object -> Convert Object to StringObj
            if ((L_is_cstring || L_is_StringObj) && !R_is_cstring && !R_is_StringObj && 
                R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(R->getType()->getPointerElementType());
                Value* strVal = structGen->generateStrCall(R, st->getName().str());
                if (strVal) {
                    R = strVal;
                    R_is_StringObj = true; // R is now a String Object
                }
            }

            // Case B: Object + String (Obj/CStr) -> Convert Object to StringObj
            if (!L_is_cstring && !L_is_StringObj && (R_is_cstring || R_is_StringObj) && 
                L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(L->getType()->getPointerElementType());
                Value* strVal = structGen->generateStrCall(L, st->getName().str());
                if (strVal) {
                    L = strVal;
                    L_is_StringObj = true; // L is now a String Object
                }
            }

            // Case C: Convert Numbers to String Objects if other side is String
            bool isStrConcat = (L_is_cstring || L_is_StringObj) || (R_is_cstring || R_is_StringObj);
            
            if (isStrConcat) {
                if (L->getType()->isIntegerTy()) {
                    L = builtinGen->generateIntToString(L); 
                    L_is_StringObj = true;
                } else if (L->getType()->isDoubleTy()) {
                    L = builtinGen->generateDoubleToString(L);
                    L_is_StringObj = true;
                }

                if (R->getType()->isIntegerTy()) {
                    R = builtinGen->generateIntToString(R); 
                    R_is_StringObj = true;
                } else if (R->getType()->isDoubleTy()) {
                    R = builtinGen->generateDoubleToString(R);
                    R_is_StringObj = true;
                }
            }

            // Case D: Execute Concatenation (Box c-strings if needed)
            // We re-check flags because L/R might have changed above
            L_is_cstring = (L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isIntegerTy(8));
            R_is_cstring = (R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isIntegerTy(8));
            
            if (isStrConcat) {
                // BOX L if it is still a c-string
                if (L_is_cstring) {
                    std::vector<Value*> args = {L};
                    L = structGen->allocateAndInit("String", args);
                }
                // BOX R if it is still a c-string
                if (R_is_cstring) {
                    std::vector<Value*> args = {R};
                    R = structGen->allocateAndInit("String", args);
                }
                
                // Now both are definitely String Objects
                return builtinGen->generateStringConcat(L, R);
            }
        }

        // 6. String Comparison (C-String specific optimization)
        if (L_is_cstring && R_is_cstring && (binOp->op == "==" || binOp->op == "!="))
            return builtinGen->generateStringCompare(L, R, binOp->op);

        // 8. Struct Operator Overloading
        if (L->getType()->isPointerTy() &&
            L->getType()->getPointerElementType()->isStructTy()) {
            StructType* st =
                cast<StructType>(L->getType()->getPointerElementType());
            std::string structName = st->getName().str();
            std::string magicMethod = "";
            if (binOp->op == "+")
                magicMethod = "__add";
            else if (binOp->op == "-")
                magicMethod = "__sub";
            else if (binOp->op == "*")
                magicMethod = "__mul";
            else if (binOp->op == "/")
                magicMethod = "__div";
            else if (binOp->op == "==")
                magicMethod = "__eq";
            else if (binOp->op == "!=")
                magicMethod = "__eq";

            if (!magicMethod.empty()) {
                std::string funcName = structName + "_" + magicMethod;
                Function* opFunc = TheModule->getFunction(funcName);

                if (opFunc) {
                    Value* arg2 = R;
                    if (opFunc->arg_size() == 2) {
                        Type* expectedType =
                            opFunc->getFunctionType()->getParamType(1);
                        if (arg2->getType() != expectedType) {
                            // Auto-box literal to String
                            if (arg2->getType()->isPointerTy() &&
                                arg2->getType()
                                    ->getPointerElementType()
                                    ->isIntegerTy(8) &&
                                expectedType->isPointerTy() &&
                                expectedType->getPointerElementType()
                                    ->isStructTy() &&
                                expectedType->getPointerElementType()
                                        ->getStructName() == "String") {
                                std::vector<Value*> args = {arg2};
                                arg2 =
                                    structGen->allocateAndInit("String", args);
                            } else if (arg2->getType()->isIntegerTy() &&
                                       expectedType->isDoubleTy()) {
                                arg2 = Builder.CreateSIToFP(arg2, expectedType);
                            }
                        }
                    }
                    Value* result = Builder.CreateCall(opFunc, {L, arg2});
                    if (binOp->op == "!=")
                        return Builder.CreateNot(result);
                    return result;
                }
            }
        }

        return mathGen->generateBinaryOp(binOp->op, L, R);
    }

    if (auto member = dynamic_cast<MemberAccessNode*>(node)) {
        Value* objPtr = handleExpression(member->object.get());
        return structGen->generateMemberAccess(objPtr, member->memberName);
    }

    if (auto c = dynamic_cast<ConstructorNode*>(node))
        return handleConstructor(c);

    return nullptr;
}

std::string LLVMCodegen::currentCodegenClass = "";