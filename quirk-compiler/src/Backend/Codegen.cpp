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
    std::map<std::string, FunctionNode*> functionDeclarations;

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
    
    // --- NEW: Track active module aliases (e.g., "sys" -> "core.sys") ---
    std::map<std::string, std::string> activeModuleAliases;

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

    void compile(const std::vector<std::unique_ptr<Node>>& nodes, raw_ostream& out = errs()) {
        builtinGen->Initialize();
        moduleRegistry->registerAll(Context, StructTypes, structGen.get(), TheModule.get());

        // PASS 0: User Structs
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
                for (const auto& field : s->fields) {
                    Type* t = typeGen->getLLVMType(field.type);
                    if (t->isStructTy()) t = PointerType::getUnqual(t);
                    elementTypes.push_back(t);
                    fieldNames.push_back(field.name);
                }
                st->setBody(elementTypes);
                structGen->registerStructLayout(s->name, fieldNames);
            }
        }

        // PASS 1: Function Declarations
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                functionDeclarations[func->name] = func;
                
                if (func->name == "main" || builtinGen->isBuiltin(func->name)) continue;
                Type* retTy = typeGen->getFunctionReturnType(func->returnType);
                std::vector<Type*> argTypes;
                if (!func->cls.empty() && !func->isStatic) argTypes.push_back(typeGen->getLLVMType(func->cls));
                for (const auto& param : func->parameters) argTypes.push_back(typeGen->getLLVMType(param.type));
                FunctionType* FT = FunctionType::get(retTy, argTypes, false);
                Function::Create(FT, Function::ExternalLinkage, func->name, TheModule.get());
            }
        }

        // PASS 2: Compile Bodies
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name != "main") compileFunction(func);
            }
        }

        // PASS 3: Main
        FunctionType* mainType = FunctionType::get(Type::getInt32Ty(Context), {}, false);
        Function* mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", TheModule.get());
        Builder.SetInsertPoint(BasicBlock::Create(Context, "entry", mainFunc));
        
        // Clear aliases for main
        activeModuleAliases.clear();
        
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name == "main") {
                    for (const auto& stmt : func->body) handleStatement(stmt.get(), mainFunc);
                }
            } else if (!dynamic_cast<StructNode*>(node.get()) && !dynamic_cast<FunctionNode*>(node.get())) {
                 // Top-level statements (global scope / main script)
                 handleStatement(node.get(), mainFunc);
            }
        }
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
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
        activeModuleAliases.clear(); // Clear aliases for new function scope

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

    // --- NEW: Handle Use Statements ---
    void handleUse(UseNode* node) {
        // Only handle "use sys" (empty filter list means namespace import)
        if (node->filterList.empty()) {
            std::string alias = node->moduleName;
            
            // Extract last segment (e.g. "core.sys" -> "sys")
            size_t lastDot = alias.rfind('.');
            if (lastDot == std::string::npos) lastDot = alias.rfind('/');
            if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
            
            activeModuleAliases[alias] = node->moduleName;
            std::cerr << "[Codegen] Registered Module Alias: " << alias << " -> " << node->moduleName << std::endl;
        }
    }

    Value* handleCall(CallNode* call) 
    {
        // 1. Builtin Functions
        if (auto lit = dynamic_cast<LiteralNode*>(call->callee.get())) {
            if (builtinGen->isBuiltin(lit->value)) {
                return builtinGen->handleBuiltin(
                    lit->value, call,
                    [this](Node* n) { return this->handleExpression(n); });
            }

            // 2. Struct Constructors (Direct call: Vector2(...))
            if (StructTypes.count(lit->value)) {
                std::vector<Value*> args;
                for (auto& a : call->args)
                    args.push_back(handleExpression(a.value.get()));
                return structGen->allocateAndInit(lit->value, args);
            }

            // 3. Global Functions
            Function* func = TheModule->getFunction(lit->value);
            if (!func && !currentCodegenClass.empty())
                func = TheModule->getFunction(currentCodegenClass + "_" + lit->value);

            if (func) return generateGlobalCall(func, call);
        }

        // 4. Method / Module Calls
        if (auto member = dynamic_cast<MemberAccessNode*>(call->callee.get())) {
            
            // A. Module Call (e.g. math.Vector2 or sys.getenv)
            if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                if (activeModuleAliases.count(lit->value)) {
                    std::string memberName = member->memberName;

                    // --- FIX START: Check for Struct Constructor via Module ---
                    // Example: math.Vector2(1, 2)
                    if (StructTypes.count(memberName)) {
                        std::vector<Value*> args;
                        for (auto& a : call->args)
                            args.push_back(handleExpression(a.value.get()));
                        return structGen->allocateAndInit(memberName, args);
                    }
                    // --- FIX END --------------------------------------------

                    // It wasn't a struct, so it must be a Function (sys.getenv)
                    Function* func = TheModule->getFunction(memberName);
                    if (!func) {
                        std::cerr << "Error: Module function or struct '" << memberName << "' not found." << std::endl;
                        return nullptr;
                    }
                    return generateGlobalCall(func, call);
                }
            }

            // B. Method Call (obj.method())
            Value* objPtr = handleExpression(member->object.get());
            std::string typeName;

            if (!objPtr) { // Static
                if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                    if (StructTypes.count(lit->value)) typeName = lit->value;
                }
                if (typeName.empty()) return nullptr;
            } else { // Instance
                // Auto-unbox pointers
                if (objPtr->getType()->isStructTy()) {
                    Value* mem = Builder.CreateAlloca(objPtr->getType());
                    Builder.CreateStore(objPtr, mem);
                    objPtr = mem;
                }
                if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isPointerTy()) {
                    objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);
                }
                // C-String Auto-box
                if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isIntegerTy(8)) {
                    std::vector<Value*> ctorArgs = {objPtr};
                    objPtr = structGen->allocateAndInit("String", ctorArgs);
                }

                // Type Check
                if (objPtr->getType()->isIntegerTy(1)) typeName = "Bool";
                else if (objPtr->getType()->isIntegerTy()) typeName = "Int";
                else if (objPtr->getType()->isDoubleTy()) typeName = "Double";
                else if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(objPtr->getType()->getPointerElementType());
                    typeName = st->getName().str();
                } else {
                    return nullptr;
                }
            }

            // Extension Methods (String.format)
            if (objPtr) {
                Value* extResult = typeExtensions->tryHandleMethod(
                    typeName, member->memberName, objPtr, call->args,
                    [this](Node* n) { return this->handleExpression(n); });
                if (extResult) return extResult;
            }

            std::string funcName = typeName + "_" + member->memberName;
            Function* func = TheModule->getFunction(funcName);
            if (!func) return nullptr;

            std::vector<Value*> args;
            if (objPtr) args.push_back(objPtr);
            processCallArgs(func, call->args, args, (objPtr ? 1 : 0));
            return Builder.CreateCall(func, args);
        }
        return nullptr;
    }
    
    // Helper to generate arguments for standard global function calls
    Value* generateGlobalCall(Function* func, CallNode* call) {
        std::vector<Value*> finalArgs;
        processCallArgs(func, call->args, finalArgs, 0);
        return Builder.CreateCall(func, finalArgs);
    }

    // Helper to process arguments and handle casting/boxing
    // Helper to process arguments and handle casting/boxing/named arguments
    void processCallArgs(Function* func, const std::vector<Arg>& astArgs, std::vector<Value*>& finalArgs, size_t offset) {
        std::string funcName = func->getName().str();
        FunctionNode* funcNode = functionDeclarations[funcName];
        
        bool isVariadic = variadicFunctions.count(funcName);
        size_t fixedArgCount = func->arg_size();
        size_t requiredFixedCount = isVariadic ? (fixedArgCount - 1) : fixedArgCount;

        // Buffer to hold matched arguments before casting
        std::vector<Value*> matchedArgs(fixedArgCount, nullptr);
        std::vector<Value*> variadicBundle;

        // ==========================================
        // PASS 1: Map Provided Arguments to Slots
        // ==========================================
        size_t positionalIdx = offset;

        for (const auto& arg : astArgs) {
            if (!arg.name.empty()) {
                // --- NAMED ARGUMENT ---
                bool found = false;
                if (funcNode) {
                    for (size_t i = offset; i < requiredFixedCount; ++i) {
                        size_t paramIdx = i - offset;
                        if (paramIdx < funcNode->parameters.size() && funcNode->parameters[paramIdx].name == arg.name) {
                            if (matchedArgs[i] != nullptr) {
                                std::cerr << "Error: Argument '" << arg.name << "' passed multiple times." << std::endl;
                                return;
                            }
                            matchedArgs[i] = handleExpression(arg.value.get());
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    std::cerr << "Error: Unknown parameter '" << arg.name << "' in call to " << funcName << std::endl;
                    return;
                }
            } else {
                // --- POSITIONAL ARGUMENT ---
                // Skip any slots that were already filled by named arguments
                while (positionalIdx < requiredFixedCount && matchedArgs[positionalIdx] != nullptr) {
                    positionalIdx++;
                }

                if (positionalIdx < requiredFixedCount) {
                    matchedArgs[positionalIdx] = handleExpression(arg.value.get());
                    positionalIdx++;
                } else {
                    // Overflow arguments go to the Variadic Bundle
                    variadicBundle.push_back(handleExpression(arg.value.get()));
                }
            }
        }

        // ==========================================
        // PASS 2: Apply Defaults & Type Casting
        // ==========================================
        for (size_t i = offset; i < fixedArgCount; i++) {
            size_t astIdx = i - offset;
            Value* argVal = matchedArgs[i];

            // 1. Fallback to Default Value if slot is empty
            if (!argVal && funcNode && astIdx < funcNode->parameters.size() && funcNode->parameters[astIdx].defaultValue) {
                argVal = handleExpression(funcNode->parameters[astIdx].defaultValue.get());
            }

            // 2. Error if a required slot is STILL empty
            if (!argVal && i < requiredFixedCount) {
                std::string pName = (funcNode && astIdx < funcNode->parameters.size()) ? funcNode->parameters[astIdx].name : std::to_string(astIdx);
                std::cerr << "Error: Missing required argument for parameter '" << pName << "'" << std::endl;
                return;
            }

            // 3. Apply Boxing and Casting (Your existing logic)
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

        // ==========================================
        // PASS 3: Finalize Variadic List Bundle
        // ==========================================
        if (isVariadic) {
            std::vector<Value*> castedVariadic;
            for (Value* vArg : variadicBundle) {
                if (vArg->getType()->isIntegerTy()) {
                    vArg = Builder.CreateIntToPtr(vArg, Type::getInt8PtrTy(Context));
                } else if (!vArg->getType()->isPointerTy()) {
                    vArg = Builder.CreateBitCast(vArg, Type::getInt8PtrTy(Context));
                }
                castedVariadic.push_back(vArg);
            }
            Value* listObj = structGen->createListFromValues(castedVariadic);
            finalArgs.push_back(listObj);
        }
    }
    
    void handleStatement(Node* node, Function* parentFunc) {
        // --- NEW: Dead Code Elimination ---
        // If the current block already has a terminator (from a 'return' or 'throw'),
        // we ignore any subsequent statements in this scope.
        if (Builder.GetInsertBlock()->getTerminator()) {
            return; 
        }

        if (auto u = dynamic_cast<UseNode*>(node)) handleUse(u); // <--- Dispatch UseNode
        else if (auto vdecl = dynamic_cast<VarDeclNode*>(node)) handleVarDecl(vdecl);
        else if (auto call = dynamic_cast<CallNode*>(node)) handleCall(call);
        else if (auto i = dynamic_cast<IfNode*>(node)) handleIf(i, parentFunc);
        else if (auto w = dynamic_cast<WhileNode*>(node)) handleWhile(w, parentFunc);
        else if (auto f = dynamic_cast<ForNode*>(node)) {
            flowGen->generateFor(
                f, parentFunc,
                [this](Node* n) { return this->handleExpression(n); },
                [this](const std::string& s, std::vector<Value*>& v) {
                    return this->structGen->allocateAndInit(s, v);
                },
                [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); },
                varGen.get());
        }
        else if (auto wi = dynamic_cast<WithNode*>(node)) handleWith(wi, parentFunc);
        else if (auto t = dynamic_cast<TryCatchNode*>(node)) {
            flowGen->generateTryCatch(t, parentFunc, 
                [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); },
                varGen.get(), StructTypes);
        }
        else if (auto th = dynamic_cast<ThrowNode*>(node)) {
            flowGen->generateThrow(th, parentFunc, 
                [this](Node* n) { return this->handleExpression(n); });
        }
        else if (auto ret = dynamic_cast<ReturnNode*>(node)) {
            if (ret->expression) {
                Value* retVal = handleExpression(ret->expression.get());

                // Special handling for 'main'
                if (parentFunc->getName() == "main") {
                    if (retVal->getType()->isIntegerTy(32)) {
                        Builder.CreateRet(retVal);
                        return;
                    }
                    Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
                    return;
                }

                // --- TYPE MATCHING LOGIC ---
                Type* expectedType = parentFunc->getReturnType();
                if (retVal->getType() != expectedType) {
                    // 1. Integer casting
                    if (retVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
                        retVal = Builder.CreateIntCast(retVal, expectedType, true);
                    }
                    // 2. Raw C-String to String Object (The correct fix)
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
                    
                    // --- STRIP "struct." PREFIX ---
                    std::string structName = st->getName().str();
                    if (structName.find("struct.") == 0) 
                        structName = structName.substr(7);
                    // -----------------------------

                    Function* func = TheModule->getFunction(structName + "___set");
                    
                    if (func) {
                        // FIX: Auto-box KEY (index) for __set
                        if (func->arg_size() >= 2) {
                            Type* keyType = func->getFunctionType()->getParamType(1);
                            
                            // Check if Key needs boxing (c-string -> String Struct)
                            if (index->getType()->isPointerTy() && 
                                index->getType()->getPointerElementType()->isIntegerTy(8) &&
                                keyType->isPointerTy() && 
                                keyType->getPointerElementType()->isStructTy()) {
                                
                                StructType* paramSt = cast<StructType>(
                                    keyType->getPointerElementType());
                                
                                // Relaxed check: contains "String"
                                if (paramSt->getName().str().find("String") != std::string::npos) {
                                    std::vector<Value*> args = {index};
                                    index = structGen->allocateAndInit("String", args);
                                }
                            }
                        }

                        // Auto-box VALUE (Any)
                        if (func->arg_size() >= 3) {
                            Type* valType = func->getFunctionType()->getParamType(2);
                            
                            if (val->getType() != valType) {
                                if (val->getType()->isIntegerTy() && valType->isPointerTy()) {
                                    val = Builder.CreateIntToPtr(val, valType);
                                } 
                                else if (val->getType()->isPointerTy() && valType->isPointerTy()) {
                                    val = Builder.CreateBitCast(val, valType);
                                }
                            }
                        }
                        
                        Builder.CreateCall(func, {ptr, index, val});
                        return;
                    }
                }
                
                // 2. Fallback for raw arrays
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
    // 1. Function Calls
    if (auto call = dynamic_cast<CallNode*>(node))
        return handleCall(call);

    // 2. Literals
    if (auto lit = dynamic_cast<LiteralNode*>(node)) {
        if (lit->value == "true")
            return ConstantInt::getTrue(Context);
        if (lit->value == "false")
            return ConstantInt::getFalse(Context);
        if (std::isdigit(lit->value[0])) {
            if (lit->value.find('.') != std::string::npos)
                return ConstantFP::get(Context, APFloat(std::stod(lit->value)));
            return ConstantInt::get(Type::getInt32Ty(Context), std::stoi(lit->value));
        }
        if (lit->value.size() >= 2 && lit->value.front() == '"') {
            return Builder.CreateGlobalStringPtr(
                unescapeString(lit->value.substr(1, lit->value.size() - 2)));
        }
        if (varGen->exists(lit->value))
            return varGen->resolveVariable(lit->value);
    }

    // 3. List/Map Literals
    if (auto arr = dynamic_cast<ListLiteralNode*>(node)) {
        return structGen->generateListLiteral(arr, [this](Node* n) { return this->handleExpression(n); });
    }
    if (auto mapLit = dynamic_cast<MapLiteralNode*>(node)) {
        return structGen->generateMapLiteral(mapLit, [this](Node* n) { return this->handleExpression(n); });
    }

    // 4. Binary Operators
    if (auto binOp = dynamic_cast<BinaryOpNode*>(node)) {
        if (binOp->op == "not")
            return mathGen->generateNot(handleExpression(binOp->left.get()));

        if (binOp->op == "and" || binOp->op == "or")
            return mathGen->generateLogicOp(binOp->op, handleExpression(binOp->left.get()),
                binOp->right.get(), [this](Node* n) { return this->handleExpression(n); });

        Value* L = handleExpression(binOp->left.get());
        Value* R = handleExpression(binOp->right.get());
        if (!L || !R) return nullptr;

        // --- Array/Map Access [] ---
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

        // =========================================================
        // D. STRING CONCATENATION (+)
        // =========================================================
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

                    // Case 1: Already a String Struct
                    if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(ty->getPointerElementType());
                        if (st->getName().contains("String")) return val;

                        // Call __str for other structs
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        
                        Function* strFunc = TheModule->getFunction(sName + "___str");
                        if (strFunc) {
                            Value* ret = Builder.CreateCall(strFunc, {val});
                            // Fix: Ensure we return a String Object, not raw char*
                            if (ret->getType()->isPointerTy() && ret->getType()->getPointerElementType()->isIntegerTy(8)) {
                                std::vector<Value*> boxArgs = {ret};
                                return structGen->allocateAndInit("String", boxArgs);
                            }
                            return ret;
                        }
                    }

                    // Case 2: Raw C-String (i8*) -> Box to String Struct
                    if (ty->isPointerTy() && ty->getPointerElementType()->isIntegerTy(8)) {
                        std::vector<Value*> args = {val}; 
                        return structGen->allocateAndInit("String", args);
                    }

                    // Case 3: Primitives
                    if (ty->isIntegerTy(32)) { 
                        if (auto* f = TheModule->getFunction("Int_str")) return Builder.CreateCall(f, {val});
                    }
                    if (ty->isDoubleTy()) { 
                        if (auto* f = TheModule->getFunction("Double_str")) return Builder.CreateCall(f, {val});
                    }
                    if (ty->isIntegerTy(1)) { 
                        if (auto* f = TheModule->getFunction("Bool_str")) return Builder.CreateCall(f, {val});
                    }

                    return nullptr; 
                };

                Value* strL = makeString(L);
                Value* strR = makeString(R);

                if (strL && strR) {
                    Function* addFunc = TheModule->getFunction("String___add");
                    
                    // --- FIX: Declare String___add if missing ---
                    if (!addFunc) {
                        Type* strType = strL->getType();
                        // Only declare if we actually have String objects
                        if (strType->isPointerTy() && strType->getPointerElementType()->isStructTy()) {
                            FunctionType* ft = FunctionType::get(strType, {strType, strType}, false);
                            addFunc = Function::Create(ft, Function::ExternalLinkage, "String___add", TheModule.get());
                        }
                    }
                    // ---------------------------------------------

                    if (addFunc) {
                        return Builder.CreateCall(addFunc, {strL, strR});
                    }
                }
            }
        }

        // E. STANDARD MATH
        return mathGen->generateBinaryOp(binOp->op, L, R);
    }

    // 5. Member Access
    if (auto member = dynamic_cast<MemberAccessNode*>(node)) {
        Value* objPtr = handleExpression(member->object.get());
        return structGen->generateMemberAccess(objPtr, member->memberName);
    }

    // 6. Constructors
    if (auto c = dynamic_cast<ConstructorNode*>(node))
        return handleConstructor(c);

    return nullptr;
}

std::string LLVMCodegen::currentCodegenClass = "";