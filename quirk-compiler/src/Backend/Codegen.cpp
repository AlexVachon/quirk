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

class LLVMCodegen {
    LLVMContext Context;
    std::unique_ptr<Module> TheModule;
    IRBuilder<> Builder;

    std::map<std::string, bool> variadicFunctions;
    std::map<std::string, FunctionNode*> functionDeclarations;
    std::map<std::string, StructType*> StructTypes;

    std::map<std::string, std::vector<std::string>> structHierarchy;

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
    std::map<std::string, std::string> activeModuleAliases;
    std::map<std::string, std::string> activeTriggers;

    LLVMCodegen() : Builder(Context) {
        TheModule = std::make_unique<Module>("QuirkCompiler", Context);
        typeGen = std::make_unique<TypeGen>(Context, StructTypes);
        flowGen = std::make_unique<ControlFlowGen>(Context, TheModule.get(), Builder);
        structGen = std::make_unique<StructGen>(Context, TheModule.get(), Builder, StructTypes);
        builtinGen = std::make_unique<BuiltinGen>(Context, TheModule.get(), Builder, structGen.get());
        structGen->setBuiltinGen(builtinGen.get());

        typeExtensions = std::make_unique<TypeExtensions>(Context, TheModule.get(), Builder, structGen.get(), builtinGen.get());
        varGen = std::make_unique<VariableGen>(Context, Builder);
        mathGen = std::make_unique<MathGen>(Context, Builder);
        moduleRegistry = std::make_unique<ModuleRegistry>();
    }

    void compile(const std::vector<std::unique_ptr<Node>>& nodes, raw_ostream& out = errs()) {
        builtinGen->Initialize();

        // --- NEW: Process Top-Level Uses globally ---
        for (const auto& node : nodes) {
            if (auto u = dynamic_cast<UseNode*>(node.get())) handleUse(u);
        }
        // --------------------------------------------

        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                if (!StructTypes.count(s->name)) StructTypes[s->name] = StructType::create(Context, s->name);
            }
        }
        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                StructType* st = StructTypes[s->name];
                if (!st->isOpaque()) continue;

                std::vector<Type*> elementTypes;
                std::vector<std::string> fieldNames;

                auto extractFields = [&](StructNode* sn, std::vector<Type*>& types, std::vector<std::string>& names, auto& extractRef) -> void {
                    for (const std::string& parentName : sn->parents) {
                        for (const auto& searchNode : nodes) {
                            if (auto ps = dynamic_cast<StructNode*>(searchNode.get())) {
                                if (ps->name == parentName) {
                                    extractRef(ps, types, names, extractRef); 
                                    break;
                                }
                            }
                        }
                    }
                    for (const auto& field : sn->fields) {
                        Type* t = typeGen->getLLVMType(field.type);
                        if (t->isStructTy()) t = PointerType::getUnqual(t);
                        types.push_back(t);
                        names.push_back(field.name);
                    }
                };

                structHierarchy[s->name] = s->parents; 
                structGen->setHierarchy(&structHierarchy);
                extractFields(s, elementTypes, fieldNames, extractFields);

                st->setBody(elementTypes);
                structGen->registerStructLayout(s->name, fieldNames);
            }
        }

        moduleRegistry->registerAll(Context, StructTypes, structGen.get(), TheModule.get());

        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                functionDeclarations[func->name] = func;
                
                if (func->name == "main" || builtinGen->isBuiltin(func->name)) continue;
                Type* retTy = typeGen->getFunctionReturnType(func->returnType);
                std::vector<Type*> argTypes;
                if (!func->cls.empty() && !func->isStatic) argTypes.push_back(typeGen->getLLVMType(func->cls));
                for (const auto& param : func->parameters) argTypes.push_back(typeGen->getLLVMType(param.type));
                FunctionType* FT = FunctionType::get(retTy, argTypes, false);
                
                // --- NEW: LINKAGE NAME INJECTION ---
                std::string llvmName = func->linkageName.empty() ? func->name : func->linkageName;
                Function::Create(FT, Function::ExternalLinkage, llvmName, TheModule.get());
            }
        }

        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name != "main") compileFunction(func);
            }
        }

        FunctionType* mainType = FunctionType::get(
            Type::getInt32Ty(Context), 
            {Type::getInt32Ty(Context), Type::getInt8PtrTy(Context)->getPointerTo()}, 
            false
        );
        Function* mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", TheModule.get());
        Builder.SetInsertPoint(BasicBlock::Create(Context, "entry", mainFunc));
        
        Value* argc = mainFunc->getArg(0);
        Value* argv = mainFunc->getArg(1);
        
        FunctionCallee runtimeInit = TheModule->getOrInsertFunction("QuirkRuntime_init", 
            FunctionType::get(Type::getVoidTy(Context), {Type::getInt32Ty(Context), Type::getInt8PtrTy(Context)->getPointerTo()}, false));
            
        Builder.CreateCall(runtimeInit, {argc, argv});

        // --- NEW: Push 'main' to the shadow stack! ---
        FunctionCallee pushFrame = TheModule->getOrInsertFunction("quirk_push_frame",
            Type::getVoidTy(Context), Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context));
        
        Value* mainFuncName = Builder.CreateGlobalStringPtr("main");
        Value* mainFileName = Builder.CreateGlobalStringPtr("main"); 
        Builder.CreateCall(pushFrame, {mainFuncName, mainFileName});
        // ---------------------------------------------
        
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name == "main") {
                    for (const auto& stmt : func->body) handleStatement(stmt.get(), mainFunc);
                }
            } else if (!dynamic_cast<StructNode*>(node.get()) && !dynamic_cast<FunctionNode*>(node.get())) {
                 handleStatement(node.get(), mainFunc);
            }
        }

        // --- NEW: Pop 'main' from the shadow stack! ---
        FunctionCallee popFrame = TheModule->getOrInsertFunction("quirk_pop_frame", Type::getVoidTy(Context));
        Builder.CreateCall(popFrame);
        // ----------------------------------------------
        
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
        TheModule->print(out, nullptr);
    }

    void compileFunction(FunctionNode* node) {
        std::string prevClass = currentCodegenClass;
        currentCodegenClass = node->cls;

        // --- NEW: LINKAGE NAME LOOKUP ---
        std::string llvmName = node->linkageName.empty() ? node->name : node->linkageName;
        Function* F = TheModule->getFunction(llvmName);
        
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

        if (!node->cls.empty() && !node->isStatic) {
            llvm::Argument* selfArg = &*argIt++;
            selfArg->setName("self");
            varGen->defineArgument("self", selfArg);
        }

        for (; argIt != F->arg_end(); ++argIt) {
            if (paramIdx >= node->parameters.size()) break;
            std::string argName = node->parameters[paramIdx++].name;
            argIt->setName(argName);
            varGen->defineArgument(argName, &*argIt);
        }

        // --- NEW: INJECT SHADOW STACK PUSH ---
        FunctionCallee pushFrame = TheModule->getOrInsertFunction("quirk_push_frame",
            Type::getVoidTy(Context), Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context));
        
        Value* funcNameVal = Builder.CreateGlobalStringPtr(node->name);
        Value* fileNameVal = Builder.CreateGlobalStringPtr(node->moduleName);
        Builder.CreateCall(pushFrame, {funcNameVal, fileNameVal});
        // -------------------------------------

        for (const auto& stmt : node->body) {
            handleStatement(stmt.get(), F);
        }

        // --- NEW: INJECT SHADOW STACK POP ON IMPLICIT RETURN ---
        if (!Builder.GetInsertBlock()->getTerminator()) {
            FunctionCallee popFrame = TheModule->getOrInsertFunction("quirk_pop_frame", Type::getVoidTy(Context));
            Builder.CreateCall(popFrame);
            
            if (F->getReturnType()->isVoidTy()) Builder.CreateRetVoid();
            else Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
        }
        // -------------------------------------------------------

        if (prevBB) Builder.SetInsertPoint(prevBB);
        currentCodegenClass = prevClass;
    }

    std::string unescapeString(const std::string& raw) {
        std::string res;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                switch (raw[i + 1]) {
                    case 'n': res += '\n'; break;
                    case 't': res += '\t'; break;
                    case 'r': res += '\r'; break;
                    case '\\': res += '\\'; break;
                    case '"': res += '\"'; break;
                    default: res += raw[i]; res += raw[i + 1];
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

    void handleUse(UseNode* node) {
        if (node->filterList.empty()) {
            std::string alias = node->moduleName;
            size_t lastDot = alias.rfind('.');
            if (lastDot == std::string::npos) lastDot = alias.rfind('/');
            if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
            activeModuleAliases[alias] = node->moduleName;
        }
    }

    Value* handleCall(CallNode* call) 
    {
        if (auto lit = dynamic_cast<LiteralNode*>(call->callee.get())) {

            // --- FIXED: Super keyword codegen ---
            if (lit->value == "super") {
                Value* selfVal = varGen->resolveVariable("self");
                if (!selfVal || structHierarchy[currentCodegenClass].empty()) return nullptr;
                std::string parentName = structHierarchy[currentCodegenClass][0];
                return Builder.CreateBitCast(selfVal, PointerType::getUnqual(StructTypes[parentName]));
            }
            // ----------------------------------
            
            if (builtinGen->isBuiltin(lit->value)) {
                return builtinGen->handleBuiltin(lit->value, call, [this](Node* n) { return this->handleExpression(n); });
            }

            if (StructTypes.count(lit->value)) {
                std::vector<Value*> args;
                for (auto& a : call->args) args.push_back(handleExpression(a.value.get()));

                // --- FIX: Resolve Default Arguments for Constructors ---
                std::string initName = lit->value + "__init";
                
                // 1. Find the correct __init (handling inheritance)
                std::string currentType = lit->value;
                while (!functionDeclarations.count(initName) && structHierarchy.count(currentType) && !structHierarchy[currentType].empty()) {
                    currentType = structHierarchy[currentType][0];
                    initName = currentType + "__init";
                }

                // 2. Fill missing arguments from defaults
                if (functionDeclarations.count(initName)) {
                    FunctionNode* funcNode = functionDeclarations[initName];
                    // Note: 'self' is already stripped from funcNode->parameters in Parser
                    for (size_t i = args.size(); i < funcNode->parameters.size(); i++) {
                        if (funcNode->parameters[i].defaultValue) {
                            args.push_back(handleExpression(funcNode->parameters[i].defaultValue.get()));
                        }
                    }
                }
                // -------------------------------------------------------

                return structGen->allocateAndInit(lit->value, args);
            }

            // --- NEW: LINKAGE NAME RESOLUTION ---
            Function* func = nullptr;
            std::string targetName = lit->value;
            
            if (functionDeclarations.count(targetName)) {
                std::string llvmName = functionDeclarations[targetName]->linkageName;
                func = TheModule->getFunction(llvmName.empty() ? targetName : llvmName);
            }
            if (!func) func = TheModule->getFunction(targetName);

            if (!func && !currentCodegenClass.empty()) func = TheModule->getFunction(currentCodegenClass + "_" + targetName);
            if (func) return generateGlobalCall(func, call);
        }

        if (auto member = dynamic_cast<MemberAccessNode*>(call->callee.get())) {
            if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                if (activeModuleAliases.count(lit->value)) {
                    std::string memberName = member->memberName;
                    if (StructTypes.count(memberName)) {
                        std::vector<Value*> args;
                        for (auto& a : call->args) args.push_back(handleExpression(a.value.get()));
                        return structGen->allocateAndInit(memberName, args);
                    }
                    
                    // --- NEW: LINKAGE NAME RESOLUTION ---
                    Function* func = nullptr;
                    if (functionDeclarations.count(memberName)) {
                        std::string llvmName = functionDeclarations[memberName]->linkageName;
                        func = TheModule->getFunction(llvmName.empty() ? memberName : llvmName);
                    }
                    if (!func) func = TheModule->getFunction(memberName);

                    if (!func) {
                        std::cerr << "Error: Module function or struct '" << memberName << "' not found." << std::endl;
                        return nullptr;
                    }
                    return generateGlobalCall(func, call);
                }
            }

            Value* objPtr = handleExpression(member->object.get());
            std::string typeName;

            if (!objPtr) { 
                if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                    if (StructTypes.count(lit->value)) typeName = lit->value;
                }
                if (typeName.empty()) return nullptr;
            } else { 
                if (objPtr->getType()->isStructTy()) {
                    Value* mem = Builder.CreateAlloca(objPtr->getType());
                    Builder.CreateStore(objPtr, mem);
                    objPtr = mem;
                }
                if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isPointerTy()) {
                    objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);
                }
                if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isIntegerTy(8)) {
                    std::vector<Value*> ctorArgs = {objPtr};
                    objPtr = structGen->allocateAndInit("String", ctorArgs);
                }

                if (objPtr->getType()->isIntegerTy(1)) typeName = "Bool";
                else if (objPtr->getType()->isIntegerTy()) typeName = "Int";
                else if (objPtr->getType()->isDoubleTy()) typeName = "Double";
                else if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(objPtr->getType()->getPointerElementType());
                    typeName = st->getName().str();
                    if (typeName.find("struct.") == 0) typeName = typeName.substr(7);
                } else {
                    return nullptr;
                }
            }

            if (objPtr) {
                Value* extResult = typeExtensions->tryHandleMethod(typeName, member->memberName, objPtr, call->args, [this](Node* n) { return this->handleExpression(n); });
                if (extResult) return extResult;
            }

            std::string funcName = typeName + "_" + member->memberName;
            Function* func = TheModule->getFunction(funcName);

            // --- NEW: Constructor Fallback ---
            // Allows explicitly calling .__init() on a super object
            if (!func && member->memberName == "__init") {
                func = TheModule->getFunction(typeName + "__init");
            }
            // ---------------------------------

            if (!func && structHierarchy.count(typeName)) {
                std::function<Function*(const std::string&)> searchHierarchy = [&](const std::string& currentType) -> Function* {
                    if (structHierarchy.count(currentType)) {
                        for (const std::string& parentName : structHierarchy[currentType]) {
                            Function* foundFunc = TheModule->getFunction(parentName + "_" + member->memberName);
                            if (foundFunc) return foundFunc;
                            foundFunc = searchHierarchy(parentName);
                            if (foundFunc) return foundFunc;
                        }
                    }
                    return nullptr;
                };
                func = searchHierarchy(typeName);
            }

            if (!func) return nullptr;

            std::vector<Value*> args;
            if (objPtr) {
                // --- FIX: Safely cast 'self' to the parent class type if calling an inherited method ---
                if (func->arg_size() > 0) {
                    Type* expectedSelfType = func->getFunctionType()->getParamType(0);
                    if (objPtr->getType() != expectedSelfType) {
                        if (objPtr->getType()->isPointerTy() && expectedSelfType->isPointerTy()) {
                            objPtr = Builder.CreateBitCast(objPtr, expectedSelfType);
                        }
                    }
                }
                args.push_back(objPtr);
            }
            
            processCallArgs(func, call->args, args, (objPtr ? 1 : 0));
            return Builder.CreateCall(func, args);
        }
        return nullptr;
    }
    
    Value* generateGlobalCall(Function* func, CallNode* call) {
        std::vector<Value*> finalArgs;
        processCallArgs(func, call->args, finalArgs, 0);
        return Builder.CreateCall(func, finalArgs);
    }

    void processCallArgs(Function* func, const std::vector<Arg>& astArgs, std::vector<Value*>& finalArgs, size_t offset) {
        std::string funcName = func->getName().str();
        FunctionNode* funcNode = functionDeclarations[funcName];
        
        bool isVariadic = variadicFunctions.count(funcName);
        size_t fixedArgCount = func->arg_size();
        size_t requiredFixedCount = isVariadic ? (fixedArgCount - 1) : fixedArgCount;

        std::vector<Value*> matchedArgs(fixedArgCount, nullptr);
        std::vector<Value*> variadicBundle;

        size_t positionalIdx = offset;

        for (const auto& arg : astArgs) {
            if (!arg.name.empty()) {
                bool found = false;
                if (funcNode) {
                    for (size_t i = offset; i < requiredFixedCount; ++i) {
                        size_t paramIdx = i - offset;
                        if (paramIdx < funcNode->parameters.size() && funcNode->parameters[paramIdx].name == arg.name) {
                            if (matchedArgs[i] != nullptr) return;
                            matchedArgs[i] = handleExpression(arg.value.get());
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) return;
            } else {
                while (positionalIdx < requiredFixedCount && matchedArgs[positionalIdx] != nullptr) positionalIdx++;
                if (positionalIdx < requiredFixedCount) {
                    matchedArgs[positionalIdx] = handleExpression(arg.value.get());
                    positionalIdx++;
                } else {
                    variadicBundle.push_back(handleExpression(arg.value.get()));
                }
            }
        }

        for (size_t i = offset; i < fixedArgCount; i++) {
            size_t astIdx = i - offset;
            Value* argVal = matchedArgs[i];

            if (!argVal && funcNode && astIdx < funcNode->parameters.size() && funcNode->parameters[astIdx].defaultValue) {
                argVal = handleExpression(funcNode->parameters[astIdx].defaultValue.get());
            }

            if (!argVal && i < requiredFixedCount) return;

            if (argVal) {
                Type* expectedType = func->getFunctionType()->getParamType(i);

                if (argVal->getType()->isIntegerTy(1) && expectedType->isIntegerTy(32)) {
                    argVal = Builder.CreateZExt(argVal, Type::getInt32Ty(Context));
                }

                if (argVal->getType()->isPointerTy() &&
                    argVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                    expectedType->isPointerTy() &&
                    expectedType->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(expectedType->getPointerElementType());
                    if (st->getName() == "String") {
                        std::vector<Value*> ctorArgs = {argVal};
                        argVal = structGen->allocateAndInit("String", ctorArgs);
                    }
                }
                else if (argVal->getType() != expectedType) {
                    if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy())
                        argVal = Builder.CreateIntCast(argVal, expectedType, true);
                    else if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy())
                        argVal = Builder.CreateIntToPtr(argVal, expectedType);
                    else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy())
                        argVal = Builder.CreateBitCast(argVal, expectedType);
                    else if (argVal->getType()->isIntegerTy() && expectedType->isDoubleTy())
                        argVal = Builder.CreateSIToFP(argVal, expectedType);
                }
                finalArgs.push_back(argVal);
            }
        }

        if (isVariadic) {
            std::vector<Value*> castedVariadic;
            for (Value* vArg : variadicBundle) {
                if (vArg->getType()->isIntegerTy()) vArg = Builder.CreateIntToPtr(vArg, Type::getInt8PtrTy(Context));
                else if (!vArg->getType()->isPointerTy()) vArg = Builder.CreateBitCast(vArg, Type::getInt8PtrTy(Context));
                castedVariadic.push_back(vArg);
            }
            Value* listObj = structGen->createListFromValues(castedVariadic);
            finalArgs.push_back(listObj);
        }
    }
    
    void handleStatement(Node* node, Function* parentFunc) {
        if (Builder.GetInsertBlock()->getTerminator()) return; 

        if (auto u = dynamic_cast<UseNode*>(node)) handleUse(u);
        else if (auto vdecl = dynamic_cast<VarDeclNode*>(node)) handleVarDecl(vdecl);
        else if (auto call = dynamic_cast<CallNode*>(node)) handleCall(call);
        else if (auto i = dynamic_cast<IfNode*>(node)) handleIf(i, parentFunc);
        else if (auto w = dynamic_cast<WhileNode*>(node)) handleWhile(w, parentFunc);
        else if (auto brk = dynamic_cast<BreakNode*>(node)) {
            if (!flowGen->breakStack.empty()) {
                Builder.CreateBr(flowGen->breakStack.back());
                // Create a dead block so subsequent IR in this branch has an insert point
                BasicBlock* dead = BasicBlock::Create(Context, "after_break", parentFunc);
                Builder.SetInsertPoint(dead);
            }
        }
        else if (auto cont = dynamic_cast<ContinueNode*>(node)) {
            if (!flowGen->continueStack.empty()) {
                Builder.CreateBr(flowGen->continueStack.back());
                // Create a dead block so subsequent IR in this branch has an insert point
                BasicBlock* dead = BasicBlock::Create(Context, "after_continue", parentFunc);
                Builder.SetInsertPoint(dead);
            }
        }
        else if (auto f = dynamic_cast<ForNode*>(node)) {
            flowGen->generateFor(
                f, parentFunc,
                [this](Node* n) { return this->handleExpression(n); },
                [this](const std::string& s, std::vector<Value*>& v) { return this->structGen->allocateAndInit(s, v); },
                [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); },
                varGen.get());
        }
        else if (auto wi = dynamic_cast<WithNode*>(node)) handleWith(wi, parentFunc);
        else if (auto t = dynamic_cast<TryCatchNode*>(node)) {
            flowGen->generateTryCatch(t, parentFunc, [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); }, varGen.get(), StructTypes, structHierarchy);
        }
        else if (auto th = dynamic_cast<ThrowNode*>(node)) {
            flowGen->generateThrow(th, parentFunc, 
                [this](Node* n) { return this->handleExpression(n); },
                StructTypes,
                [this](const std::string& s, std::vector<Value*>& v) { return this->structGen->allocateAndInit(s, v); }
            );
        }
        else if (auto tr = dynamic_cast<TriggerNode*>(node)) {
            activeTriggers[tr->varName] = tr->handlerName;
        }
        else if (auto ret = dynamic_cast<ReturnNode*>(node)) {
            // 1. Evaluate the expression FIRST while the shadow stack frame is still valid.
            Value* retVal = nullptr;
            if (ret->expression) {
                retVal = handleExpression(ret->expression.get());
                if (!retVal) return;
            }

            // 2. NOW safely pop the shadow stack frame.
            // --- NEW: INJECT SHADOW STACK POP ON EXPLICIT RETURN ---
            FunctionCallee popFrame = TheModule->getOrInsertFunction("quirk_pop_frame", Type::getVoidTy(Context));
            Builder.CreateCall(popFrame);
            // -------------------------------------------------------

            // 3. Emit the Return Instruction
            if (retVal) {
                if (parentFunc->getName() == "main") {
                    if (retVal->getType()->isIntegerTy(32)) { Builder.CreateRet(retVal); return; }
                    Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
                    return;
                }

                Type* expectedType = parentFunc->getReturnType();
                if (retVal->getType() != expectedType) {
                    if (retVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
                        retVal = Builder.CreateIntCast(retVal, expectedType, true);
                    }
                    else if (retVal->getType()->isPointerTy() && retVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                             expectedType->isPointerTy() && expectedType->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(expectedType->getPointerElementType());
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        if (sName == "String") {
                            std::vector<Value*> sArgs = {retVal};
                            retVal = structGen->allocateAndInit("String", sArgs);
                        }
                    }
                }
                Builder.CreateRet(retVal);
            } else {
                if (parentFunc->getName() == "main") Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
                else Builder.CreateRetVoid();
            }
        }
    }

    void handleVarDecl(VarDeclNode* vdecl) {
        Value* val = handleExpression(vdecl->expression.get());
        if (!val || val->getType()->isVoidTy()) return;

        if (!vdecl->typeAnnotation.empty()) {
            Type* targetType = typeGen->getLLVMType(vdecl->typeAnnotation);

            // If val is i8* and the target type is a struct pointer (String*, Socket*, etc.),
            // it is a type-erased pointer from Map_get/List_get — bitcast it directly.
            // Do NOT wrap in String__init, which would treat the pointer value as char data.
            if (val->getType()->isPointerTy() &&
                val->getType()->getPointerElementType()->isIntegerTy(8) &&
                targetType->isPointerTy() &&
                targetType->getPointerElementType()->isStructTy()) {
                val = Builder.CreateBitCast(val, targetType);
            }
            // General numeric/pointer coercions
            else if (val->getType() != targetType) {
                if (val->getType()->isIntegerTy() && targetType->isPointerTy())
                    val = Builder.CreateIntToPtr(val, targetType);
                else if (val->getType()->isPointerTy() && targetType->isPointerTy())
                    val = Builder.CreateBitCast(val, targetType);
                else if (val->getType()->isIntegerTy() && targetType->isDoubleTy())
                    val = Builder.CreateSIToFP(val, targetType);
            }
        }

        // --- NEW: Helper to safely apply +=, -=, etc. including String concatenation ---
        auto applyCompoundAssignment = [&](std::string op, Value* wasVal, Value* newVal) -> Value* {
            if (op == "+=") {
                // 1. Check if the target is already a String struct
                if (wasVal->getType()->isPointerTy() && wasVal->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(wasVal->getType()->getPointerElementType());
                    if (st->getName().contains("String")) {
                        Function* addFunc = TheModule->getFunction("String___add");
                        if (addFunc) return Builder.CreateCall(addFunc, {wasVal, newVal});
                    }
                }
                
                // 2. NEW FIX: Target is a raw C-String (i8*). Box it safely!
                if (wasVal->getType()->isPointerTy() && wasVal->getType()->getPointerElementType()->isIntegerTy(8)) {
                    std::vector<Value*> boxArgs = {wasVal};
                    Value* boxedStr = structGen->allocateAndInit("String", boxArgs);
                    Function* addFunc = TheModule->getFunction("String___add");
                    if (addFunc) return Builder.CreateCall(addFunc, {boxedStr, newVal});
                }

                // 3. Fallback to standard MathGen for numbers
                return mathGen->generateBinaryOp("+", wasVal, newVal);
            }
            if (op == "-=") return mathGen->generateBinaryOp("-", wasVal, newVal);
            if (op == "*=") return mathGen->generateBinaryOp("*", wasVal, newVal);
            if (op == "/=") return mathGen->generateBinaryOp("/", wasVal, newVal);
            return newVal;
        };
        // -------------------------------------------------------------------------------

        if (auto lhs = dynamic_cast<LiteralNode*>(vdecl->lhs.get())) {
            
            Value* wasVal = val; 
            if (varGen->exists(lhs->value)) {
                wasVal = varGen->resolveVariable(lhs->value); 
                
                // --- NEW: Use helper for Local Variables ---
                val = applyCompoundAssignment(vdecl->op, wasVal, val);
            }

            if (!varGen->exists(lhs->value)) varGen->defineLocalVariable(lhs->value, val);
            else varGen->updateLocalVariable(lhs->value, val);
            
            if (activeTriggers.count(lhs->value)) {
                Function* hook = TheModule->getFunction(activeTriggers[lhs->value]);
                if (hook) {
                    Value* argVal = val;
                    Type* expectedType = hook->getFunctionType()->getParamType(0);
                    if (argVal->getType() != expectedType) {
                        if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy()) argVal = Builder.CreateIntToPtr(argVal, expectedType);
                        else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) argVal = Builder.CreateBitCast(argVal, expectedType);
                    }
                    
                    Value* argWasVal = wasVal;
                    Type* expectedWasType = hook->getFunctionType()->getParamType(1);
                    if (argWasVal->getType() != expectedWasType) {
                        if (argWasVal->getType()->isIntegerTy() && expectedWasType->isPointerTy()) argWasVal = Builder.CreateIntToPtr(argWasVal, expectedWasType);
                        else if (argWasVal->getType()->isPointerTy() && expectedWasType->isPointerTy()) argWasVal = Builder.CreateBitCast(argWasVal, expectedWasType);
                    }
                    
                    Builder.CreateCall(hook, {argVal, argWasVal});
                }
            }
        
        } else if (auto member = dynamic_cast<MemberAccessNode*>(vdecl->lhs.get())) {
            Value* objPtr = handleExpression(member->object.get());
            if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isPointerTy())
                objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);

            Value* memberPtr = structGen->getMemberPtr(objPtr, member->memberName);
            if (memberPtr) {
                Type* fieldType = memberPtr->getType()->getPointerElementType();
                
                Value* wasVal = Builder.CreateLoad(fieldType, memberPtr, "was_val");

                // --- NEW: Use helper for Struct Members ---
                val = applyCompoundAssignment(vdecl->op, wasVal, val);

                if (val->getType() != fieldType) {
                    if (val->getType()->isPointerTy() && 
                        val->getType()->getPointerElementType()->isIntegerTy(8) &&
                        fieldType->isPointerTy() && 
                        fieldType->getPointerElementType()->isStructTy()) {
                        
                        StructType* st = cast<StructType>(fieldType->getPointerElementType());
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        if (sName == "String") {
                            std::vector<Value*> boxArgs = {val};
                            val = structGen->allocateAndInit("String", boxArgs);
                        } else {
                            val = Builder.CreateBitCast(val, fieldType);
                        }
                    }
                    else if (val->getType()->isIntegerTy() && fieldType->isIntegerTy()) val = Builder.CreateIntCast(val, fieldType, true);
                    else if (val->getType()->isPointerTy() && fieldType->isPointerTy()) val = Builder.CreateBitCast(val, fieldType);
                    else if (val->getType()->isIntegerTy() && fieldType->isPointerTy()) val = Builder.CreateIntToPtr(val, fieldType);
                    else if (val->getType()->isIntegerTy() && fieldType->isDoubleTy()) val = Builder.CreateSIToFP(val, fieldType);
                }
                
                Builder.CreateStore(val, memberPtr); 

                if (auto objLit = dynamic_cast<LiteralNode*>(member->object.get())) {
                    std::string fullPath = objLit->value + "." + member->memberName;
                    
                    if (activeTriggers.count(fullPath)) {
                        Function* hook = TheModule->getFunction(activeTriggers[fullPath]);
                        if (hook) {
                            std::vector<Value*> callArgs;
                            
                            if (hook->arg_size() == 3) {
                                Value* callObj = objPtr;
                                Type* expectedObjTy = hook->getFunctionType()->getParamType(0);
                                if (callObj->getType() != expectedObjTy) callObj = Builder.CreateBitCast(callObj, expectedObjTy);
                                callArgs.push_back(callObj);
                            }

                            Value* argVal = val;
                            Type* expectedType = hook->getFunctionType()->getParamType(hook->arg_size() - 2);
                            if (argVal->getType() != expectedType) {
                                if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy()) argVal = Builder.CreateIntToPtr(argVal, expectedType);
                                else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy()) argVal = Builder.CreateBitCast(argVal, expectedType);
                            }
                            callArgs.push_back(argVal);

                            Value* argWasVal = wasVal;
                            Type* expectedWasType = hook->getFunctionType()->getParamType(hook->arg_size() - 1);
                            if (argWasVal->getType() != expectedWasType) {
                                if (argWasVal->getType()->isIntegerTy() && expectedWasType->isPointerTy()) argWasVal = Builder.CreateIntToPtr(argWasVal, expectedWasType);
                                else if (argWasVal->getType()->isPointerTy() && expectedWasType->isPointerTy()) argWasVal = Builder.CreateBitCast(argWasVal, expectedWasType);
                            }
                            callArgs.push_back(argWasVal);

                            Builder.CreateCall(hook, callArgs);
                        }
                    }
                }
            }
        } else if (auto binOp = dynamic_cast<BinaryOpNode*>(vdecl->lhs.get())) {
            // ... (keep your existing Array [] assignment logic here)
            if (binOp->op == "[]") {
                Value* ptr = handleExpression(binOp->left.get());
                Value* index = handleExpression(binOp->right.get());

                if (ptr->getType()->isPointerTy() && ptr->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(ptr->getType()->getPointerElementType());
                    std::string structName = st->getName().str();
                    if (structName.find("struct.") == 0) structName = structName.substr(7);

                    Function* func = TheModule->getFunction(structName + "___set");
                    if (func) {
                        if (func->arg_size() >= 2) {
                            Type* keyType = func->getFunctionType()->getParamType(1);
                            if (index->getType()->isPointerTy() && index->getType()->getPointerElementType()->isIntegerTy(8) &&
                                keyType->isPointerTy() && keyType->getPointerElementType()->isStructTy()) {
                                StructType* paramSt = cast<StructType>(keyType->getPointerElementType());
                                if (paramSt->getName().str().find("String") != std::string::npos) {
                                    std::vector<Value*> args = {index};
                                    index = structGen->allocateAndInit("String", args);
                                }
                            }
                        }

                        if (func->arg_size() >= 3) {
                            Type* valType = func->getFunctionType()->getParamType(2);
                            if (val->getType() != valType) {
                                if (val->getType()->isIntegerTy() && valType->isPointerTy()) val = Builder.CreateIntToPtr(val, valType);
                                else if (val->getType()->isPointerTy() && valType->isPointerTy()) val = Builder.CreateBitCast(val, valType);
                            }
                        }
                        Builder.CreateCall(func, {ptr, index, val});
                        return;
                    }
                }
                
                if (val->getType()->isPointerTy()) {
                    ptr = Builder.CreateBitCast(ptr, PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                    Value* elementPtr = Builder.CreateGEP(Type::getInt8PtrTy(Context), ptr, index);
                    Builder.CreateStore(val, elementPtr);
                    return;
                }
                if (val->getType()->isIntegerTy()) {
                    Value* boxedVal = Builder.CreateIntToPtr(val, Type::getInt8PtrTy(Context));
                    ptr = Builder.CreateBitCast(ptr, PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                    Value* elementPtr = Builder.CreateGEP(Type::getInt8PtrTy(Context), ptr, index);
                    Builder.CreateStore(boxedVal, elementPtr);
                    return;
                }
                ptr = Builder.CreateBitCast(ptr, Type::getInt32PtrTy(Context));
                Builder.CreateStore(val, Builder.CreateGEP(Type::getInt32Ty(Context), ptr, index));
            }
        }
    }

    void handleIf(IfNode* node, Function* parentFunc) {
        flowGen->generateIf(node, parentFunc, [this](Node* n) { return this->handleExpression(n); }, [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); });
    }

    void handleWhile(WhileNode* node, Function* parentFunc) {
        flowGen->generateWhile(node, parentFunc, [this](Node* n) { return this->handleExpression(n); }, [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); });
    }

    Value* handleConstructor(ConstructorNode* node) {
        return structGen->generateConstructor(node, [this](Node* n) { return this->handleExpression(n); });
    }

    void handleWith(WithNode* node, Function* parentFunc) {
        Value* resource = handleExpression(node->resource.get());
        if (!resource) return;

        StructType* st = cast<StructType>(resource->getType()->getPointerElementType());
        std::string typeName = st->getName().str();

        if (typeName.find("struct.") == 0) typeName = typeName.substr(7);

        std::string enterName = typeName + "___enter";
        Function* enterFunc = TheModule->getFunction(enterName);
        
        if (!enterFunc) {
            std::cerr << "[Codegen Error] Missing " << enterName << " in LLVM Module." << std::endl;
            return;
        }

        Value* contextVal = Builder.CreateCall(enterFunc, {resource});

        varGen->defineLocalVariable(node->varName, contextVal);

        for (const auto& stmt : node->body) handleStatement(stmt.get(), parentFunc);

        std::string exitName = typeName + "___exit";
        Function* exitFunc = TheModule->getFunction(exitName);
        if (exitFunc) Builder.CreateCall(exitFunc, {resource});
    }
};

Value* LLVMCodegen::handleExpression(Node* node) {
    if (auto call = dynamic_cast<CallNode*>(node)) return handleCall(call);

    if (auto lit = dynamic_cast<LiteralNode*>(node)) {
        if (lit->value == "true") return ConstantInt::getTrue(Context);
        if (lit->value == "false") return ConstantInt::getFalse(Context);

        // Handle super()
        if (lit->value == "super") {
            Value* selfVal = varGen->resolveVariable("self");
            if (!selfVal || structHierarchy[currentCodegenClass].empty()) return nullptr;
            std::string parentName = structHierarchy[currentCodegenClass][0];
            return Builder.CreateBitCast(selfVal, PointerType::getUnqual(StructTypes[parentName]));
        }

        // Handle Numbers
        if (std::isdigit(lit->value[0])) {
            if (lit->value.find('.') != std::string::npos) return ConstantFP::get(Context, APFloat(std::stod(lit->value)));
            return ConstantInt::get(Type::getInt32Ty(Context), std::stoi(lit->value));
        }
        
        // --- CHANGED: Treat "string" as String Object, not raw i8* ---
        if (lit->value.size() >= 2 && lit->value.front() == '"') {
            // 1. Create the raw C-String (i8*)
            std::string rawStr = unescapeString(lit->value.substr(1, lit->value.size() - 2));
            Value* rawPtr = Builder.CreateGlobalStringPtr(rawStr);
            
            // 2. Wrap it in a String Struct immediately!
            std::vector<Value*> args = {rawPtr};
            return structGen->allocateAndInit("String", args);
        }
        // -------------------------------------------------------------

        // Handle 'c' (Char)
        if (lit->value.size() >= 2 && lit->value.front() == '\'') {
            std::string unescaped = unescapeString(lit->value.substr(1, lit->value.size() - 2));
            char c = unescaped.empty() ? '\0' : unescaped[0];
            return ConstantInt::get(Type::getInt8Ty(Context), c);
        }

        if (varGen->exists(lit->value)) return varGen->resolveVariable(lit->value);
    }

    if (auto arr = dynamic_cast<ListLiteralNode*>(node)) return structGen->generateListLiteral(arr, [this](Node* n) { return this->handleExpression(n); });
    if (auto mapLit = dynamic_cast<MapLiteralNode*>(node)) return structGen->generateMapLiteral(mapLit, [this](Node* n) { return this->handleExpression(n); });

    if (auto binOp = dynamic_cast<BinaryOpNode*>(node)) {
        if (binOp->op == "not") return mathGen->generateNot(handleExpression(binOp->left.get()));
        if (binOp->op == "and" || binOp->op == "or") return mathGen->generateLogicOp(binOp->op, handleExpression(binOp->left.get()), binOp->right.get(), [this](Node* n) { return this->handleExpression(n); });

        Value* L = handleExpression(binOp->left.get());
        Value* R = handleExpression(binOp->right.get());
        if (!L || !R) return nullptr;

        if (binOp->op == "[]") {
            if (L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(L->getType()->getPointerElementType());
                std::string sName = st->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                if (auto* func = TheModule->getFunction(sName + "___get")) {
                     if (func->arg_size() >= 2) {
                        Type* keyType = func->getFunctionType()->getParamType(1);
                        if (R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isIntegerTy(8) &&
                            keyType->isPointerTy() && keyType->getPointerElementType()->isStructTy()) {
                                 std::vector<Value*> args = {R};
                                 R = structGen->allocateAndInit("String", args);
                        }
                     }
                    return Builder.CreateCall(func, {L, R});
                }
            }
            if (L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isIntegerTy(8)) {
                Value* ptr = Builder.CreateBitCast(L, PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                return Builder.CreateLoad(Type::getInt8PtrTy(Context), Builder.CreateGEP(Type::getInt8PtrTy(Context), ptr, R));
            }
            Value* ptr = Builder.CreateBitCast(L, Type::getInt32PtrTy(Context));
            return Builder.CreateLoad(Type::getInt32Ty(Context), Builder.CreateGEP(Type::getInt32Ty(Context), ptr, R));
        }

        if (binOp->op == "+") {
            auto isStringType = [&](Value* v) {
                if (v->getType()->isPointerTy()) {
                    Type* el = v->getType()->getPointerElementType();
                    if (el->isStructTy() && cast<StructType>(el)->getName().contains("String")) return true;
                    if (el->isIntegerTy(8)) return true;
                }
                return false;
            };

            if (isStringType(L) || isStringType(R)) {
                auto makeString = [&](Value* val) -> Value* {
                    Type* ty = val->getType();
                    if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(ty->getPointerElementType());
                        if (st->getName().contains("String")) return val;

                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        Function* strFunc = TheModule->getFunction(sName + "___str");
                        if (strFunc) {
                            Value* ret = Builder.CreateCall(strFunc, {val});
                            if (ret->getType()->isPointerTy() && ret->getType()->getPointerElementType()->isIntegerTy(8)) {
                                std::vector<Value*> boxArgs = {ret};
                                return structGen->allocateAndInit("String", boxArgs);
                            }
                            return ret;
                        }
                    }
                    if (ty->isPointerTy() && ty->getPointerElementType()->isIntegerTy(8)) {
                        std::vector<Value*> args = {val}; 
                        return structGen->allocateAndInit("String", args);
                    }
                    if (ty->isIntegerTy(32)) { if (auto* f = TheModule->getFunction("Int_str")) return Builder.CreateCall(f, {val}); }
                    if (ty->isDoubleTy()) { if (auto* f = TheModule->getFunction("Double_str")) return Builder.CreateCall(f, {val}); }
                    if (ty->isIntegerTy(1)) { if (auto* f = TheModule->getFunction("Bool_str")) return Builder.CreateCall(f, {val}); }
                    return nullptr; 
                };

                Value* strL = makeString(L);
                Value* strR = makeString(R);

                if (strL && strR) {
                    Function* addFunc = TheModule->getFunction("String___add");
                    if (!addFunc) {
                        Type* strType = strL->getType();
                        if (strType->isPointerTy() && strType->getPointerElementType()->isStructTy()) {
                            FunctionType* ft = FunctionType::get(strType, {strType, strType}, false);
                            addFunc = Function::Create(ft, Function::ExternalLinkage, "String___add", TheModule.get());
                        }
                    }
                    if (addFunc) return Builder.CreateCall(addFunc, {strL, strR});
                }
            }
        }
        return mathGen->generateBinaryOp(binOp->op, L, R);
    }

    if (auto member = dynamic_cast<MemberAccessNode*>(node)) {
        Value* objPtr = handleExpression(member->object.get());
        
        if (objPtr && objPtr->getType()->isPointerTy() && 
            objPtr->getType()->getPointerElementType()->isPointerTy()) {
            objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);
        }
        
        return structGen->generateMemberAccess(objPtr, member->memberName);
    }

    if (auto c = dynamic_cast<ConstructorNode*>(node)) return handleConstructor(c);

    return nullptr;
}

std::string LLVMCodegen::currentCodegenClass = "";