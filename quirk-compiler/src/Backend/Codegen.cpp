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
#include <set>
#include <sstream>
#include <vector>

#include "ast.hpp"

#include "TypeGen.hpp"
#include "VariableGen.hpp"
#include "MathGen.hpp"
#include "StructGen.hpp"
#include "BuiltinGen.hpp"
#include "TypeExtensions.hpp"
#include "ControlFlowGen.hpp"


using namespace llvm;

class LLVMCodegen {
    LLVMContext Context;
    std::unique_ptr<Module> TheModule;
    IRBuilder<> Builder;

    bool verbose = false;

    std::map<std::string, bool> variadicFunctions;
    std::map<std::string, FunctionNode*> functionDeclarations;
    std::map<std::string, StructType*> StructTypes;

    std::map<std::string, std::vector<std::string>> structHierarchy;

    std::map<std::string, std::vector<std::string>> enumVariants;  // name -> ordered variants
    std::map<std::string, std::string> varEnumTypes;               // varName -> enum type name

    std::map<std::string, std::string> sourceMap;
    std::string currentFilePath;

    std::unique_ptr<TypeGen> typeGen;
    std::unique_ptr<ControlFlowGen> flowGen;
    std::unique_ptr<StructGen> structGen;
    std::unique_ptr<BuiltinGen> builtinGen;
    std::unique_ptr<VariableGen> varGen;
    std::unique_ptr<MathGen> mathGen;
    std::unique_ptr<TypeExtensions> typeExtensions;

   public:
    static std::string currentCodegenClass;
    std::map<std::string, std::string> activeModuleAliases;
    std::map<std::string, std::string> activeTriggers;
    std::map<std::string, std::string> callableReturnTypes; // varName → inferred return type

    void setVerbose(bool v) { verbose = v; }

    void setSourceMap(const std::map<std::string, std::string>& sm) { sourceMap = sm; }

    [[noreturn]] void fatalError(const std::string& msg, int line, int col) {
        std::cerr << "\033[1;31m[ERROR]\033[0m " << msg << "\n";

        if (line > 0 && !currentFilePath.empty() && sourceMap.count(currentFilePath)) {
            const std::string& src = sourceMap.at(currentFilePath);
            std::cerr << " --> " << currentFilePath << ":" << line << ":" << col << "\n\n";

            // Extract the requested line from the source
            int cur = 1;
            std::string lineText;
            std::istringstream ss(src);
            while (std::getline(ss, lineText)) {
                if (cur++ == line) break;
            }

            std::cerr << "    " << lineText << "\n";
            int caretOff = (col > 0) ? col - 1 : 0;
            std::cerr << "    " << std::string(caretOff, ' ') << "\033[1;33m^--- Here\033[0m\n\n";
        } else if (line > 0) {
            std::cerr << " --> line " << line << ", col " << col << "\n\n";
        }

        exit(1);
    }

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
    }

    void compile(const std::vector<std::unique_ptr<Node>>& nodes, raw_ostream& out = errs()) {
        if (verbose) std::cerr << "[Codegen] compile() started — " << nodes.size() << " top-level node(s)\n";
        builtinGen->Initialize();

        // --- NEW: Process Top-Level Uses globally ---
        for (const auto& node : nodes) {
            if (auto u = dynamic_cast<UseNode*>(node.get())) handleUse(u);
        }
        // --------------------------------------------

        if (verbose) std::cerr << "[Codegen] Pass 1: Registering opaque struct types\n";
        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                if (!StructTypes.count(s->name)) StructTypes[s->name] = StructType::create(Context, s->name);
            }
        }
        // Built-in Type struct (for self.__class)
        if (!StructTypes.count("Type")) StructTypes["Type"] = StructType::create(Context, "struct.Type");
        // Built-in Callable struct (for lambdas)
        if (!StructTypes.count("Callable")) StructTypes["Callable"] = StructType::create(Context, "struct.Callable");

        // Pass 1b: Register enum variant lists (body generation deferred to after Pass 3)
        for (const auto& node : nodes) {
            if (auto* e = dynamic_cast<EnumNode*>(node.get())) {
                enumVariants[e->name] = e->variants;
            }
        }

        if (verbose) std::cerr << "[Codegen] Pass 2: Filling struct bodies and resolving inheritance\n";
        for (const auto& node : nodes) {
            if (auto s = dynamic_cast<StructNode*>(node.get())) {
                StructType* st = StructTypes[s->name];
                if (!st->isOpaque()) continue;

                if (verbose) std::cerr << "[Codegen]   Filling struct: " << s->name << "\n";

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

        // Fill built-in Type struct body: { String*, String* } for { name, parent }
        if (StructTypes.count("Type") && StructTypes["Type"]->isOpaque() && StructTypes.count("String")) {
            Type* strPtrTy = PointerType::getUnqual(StructTypes["String"]);
            StructTypes["Type"]->setBody({strPtrTy, strPtrTy});
            structGen->registerStructLayout("Type", {"name", "parent"});
        }
        // Fill built-in Callable struct body: { i8* fn, i8* env }
        if (StructTypes.count("Callable") && StructTypes["Callable"]->isOpaque()) {
            Type* i8PtrTy = Type::getInt8PtrTy(Context);
            StructTypes["Callable"]->setBody({i8PtrTy, i8PtrTy});
            structGen->registerStructLayout("Callable", {"fn", "env"});
        }

        if (verbose) std::cerr << "[Codegen] Pass 3: Declaring function prototypes\n";
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                functionDeclarations[func->name] = func;

                // --- NEW: Register variadic functions (params declared as ...name: List) ---
                // We check if any parameter name starts with "..." which is how the parser
                // marks spread/variadic parameters (e.g. `...args: List`).
                for (const auto& param : func->parameters) {
                    if (param.isVariadic) {
                        variadicFunctions[func->name] = true;
                        if (verbose) std::cerr << "[Codegen]   Registered variadic function: " << func->name << "\n";
                        break;
                    }
                }
                // --------------------------------------------------------------------------
                
                if (func->name == "main" || builtinGen->isBuiltin(func->name)) continue;
                if (verbose) std::cerr << "[Codegen]   Declaring prototype: " << func->name << "\n";
                Type* retTy = typeGen->getFunctionReturnType(func->returnType);
                // C ABI: Bool return values are int (i32), not i1.
                // Widen i1 return types for extern functions to avoid truncation.
                if (func->isExtern && retTy->isIntegerTy(1))
                    retTy = Type::getInt32Ty(Context);
                std::vector<Type*> argTypes;
                if (!func->cls.empty() && !func->isStatic) argTypes.push_back(typeGen->getLLVMType(func->cls));
                for (const auto& param : func->parameters) {
                    Type* t = typeGen->getLLVMType(param.type);
                    // C ABI: Bool params are passed as int (i32), not i1.
                    // Widen i1 in extern declarations to match the runtime.
                    if (func->isExtern && t->isIntegerTy(1))
                        t = Type::getInt32Ty(Context);
                    argTypes.push_back(t);
                }
                FunctionType* FT = FunctionType::get(retTy, argTypes, false);
                
                // --- NEW: LINKAGE NAME INJECTION ---
                std::string llvmName = func->linkageName.empty() ? func->name : func->linkageName;
                Function::Create(FT, Function::ExternalLinkage, llvmName, TheModule.get());

                // --- NEW: Also register variadic functions by their LLVM linkage name ---
                // processCallArgs looks up by func->getName().str() which returns the LLVM
                // name (e.g. "Core_String_String_format"), not the Quirk name ("format").
                // Both must be in the map for the lookup on line 556 to work correctly.
                if (variadicFunctions.count(func->name) && !llvmName.empty() && llvmName != func->name) {
                    variadicFunctions[llvmName] = true;
                    if (verbose) std::cerr << "[Codegen]   Registered variadic LLVM name: " << llvmName << "\n";
                }
                // ------------------------------------------------------------------------

                // Populate structInitMap so StructGen can find renamed extern __init.
                // e.g. String__init in libs/core/string.qk -> "Core_String_String___init"
                if (func->isExtern && !func->cls.empty() &&
                    func->name.find("__init") != std::string::npos) {
                    structGen->registerStructInit(func->cls, llvmName);
                }
            }
        }

        // Pass 3b: Generate __EnumName_str(i32)->String* helpers
        // Runs after Pass 3 so String's body and __init prototype are both declared.
        {
            // Ensure make_String is declared (external runtime helper i8*->i8*)
            Function* makeStrFn = TheModule->getFunction("make_String");
            if (!makeStrFn) {
                FunctionType* mkFT = FunctionType::get(
                    Type::getInt8PtrTy(Context), {Type::getInt8PtrTy(Context)}, false);
                makeStrFn = Function::Create(mkFT, Function::ExternalLinkage, "make_String", TheModule.get());
            }
            Type* strPtrTy = StructTypes.count("String")
                ? (Type*)PointerType::getUnqual(StructTypes["String"])
                : (Type*)Type::getInt8PtrTy(Context);

            for (const auto& node : nodes) {
                if (auto* e = dynamic_cast<EnumNode*>(node.get())) {
                    std::string fnName = "__" + e->name + "_str";
                    FunctionType* ft = FunctionType::get(strPtrTy, {Type::getInt32Ty(Context)}, false);
                    Function* strFn = Function::Create(ft, Function::InternalLinkage, fnName, TheModule.get());

                    BasicBlock* entry = BasicBlock::Create(Context, "entry", strFn);
                    BasicBlock* dflt  = BasicBlock::Create(Context, "default", strFn);
                    Builder.SetInsertPoint(entry);
                    Value* arg = strFn->arg_begin();
                    SwitchInst* sw = Builder.CreateSwitch(arg, dflt, (unsigned)e->variants.size());

                    auto makeStr = [&](const std::string& text) -> Value* {
                        Value* rawPtr = Builder.CreateGlobalStringPtr(text);
                        Value* strVal = Builder.CreateCall(makeStrFn, {rawPtr});
                        return strPtrTy->isPointerTy() && strPtrTy != Type::getInt8PtrTy(Context)
                            ? Builder.CreateBitCast(strVal, strPtrTy)
                            : strVal;
                    };

                    for (int i = 0; i < (int)e->variants.size(); i++) {
                        BasicBlock* bb = BasicBlock::Create(Context, "case_" + e->variants[i], strFn);
                        sw->addCase(ConstantInt::get(Type::getInt32Ty(Context), i), bb);
                        Builder.SetInsertPoint(bb);
                        Builder.CreateRet(makeStr(e->variants[i]));
                    }

                    Builder.SetInsertPoint(dflt);
                    Builder.CreateRet(makeStr("<unknown>"));
                }
            }
        }

        if (verbose) std::cerr << "[Codegen] Pass 4: Compiling function bodies\n";
        for (const auto& node : nodes) {
            if (!node->filePath.empty()) currentFilePath = node->filePath;
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
        
        if (verbose) std::cerr << "[Codegen] Pass 5: Compiling 'main' body\n";
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
            if (!node->filePath.empty()) currentFilePath = node->filePath;
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
        if (verbose) std::cerr << "[Codegen] compile() finished — emitting IR\n";
        TheModule->print(out, nullptr);
    }

    void compileFunction(FunctionNode* node) {
        std::string prevClass = currentCodegenClass;
        currentCodegenClass = node->cls;

        // --- NEW: LINKAGE NAME LOOKUP ---
        std::string llvmName = node->linkageName.empty() ? node->name : node->linkageName;
        if (verbose) {
            std::cerr << "[Codegen] compileFunction: " << node->name;
            if (!node->cls.empty()) std::cerr << " (class: " << node->cls << ")";
            std::cerr << " -> LLVM name: " << llvmName << "\n";
        }
        Function* F = TheModule->getFunction(llvmName);
        
        if (!F || node->isExtern) {
            if (verbose) std::cerr << "[Codegen]   Skipping (extern or not found in module)\n";
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
            // Auto-stamp self.type for Exception __init methods.
            // Since __init is compiled once per struct, currentCodegenClass is the
            // ACTUAL subclass (e.g. "TypeError") — not the parent.  This ensures
            // that calling super().__init() doesn't silently overwrite the type.
            bool isInitMethod = node->name.find("__init") != std::string::npos && !node->cls.empty();
            if (isInitMethod && StructTypes.count("Exception") && StructTypes.count("String")) {
                std::function<bool(const std::string&)> inheritsException = [&](const std::string& c) -> bool {
                    if (c == "Exception") return true;
                    if (structHierarchy.count(c))
                        for (const auto& p : structHierarchy.at(c))
                            if (inheritsException(p)) return true;
                    return false;
                };
                if (inheritsException(node->cls)) {
                    Value* selfVal = &*F->arg_begin();
                    Value* excPtr  = Builder.CreateBitCast(selfVal, PointerType::getUnqual(StructTypes["Exception"]));
                    Value* typeFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], excPtr, 0);
                    Value* rawPtr  = Builder.CreateGlobalStringPtr(currentCodegenClass);
                    Value* strObj  = structGen->allocateAndInit("String", std::vector<Value*>{rawPtr});
                    Builder.CreateStore(strObj, typeFieldPtr);
                }
            }

            FunctionCallee popFrame = TheModule->getOrInsertFunction("quirk_pop_frame", Type::getVoidTy(Context));
            Builder.CreateCall(popFrame);

            Type* retTy = F->getReturnType();
            if (retTy->isVoidTy())
                Builder.CreateRetVoid();
            else if (retTy->isIntegerTy(32))
                Builder.CreateRet(ConstantInt::get(retTy, 0));
            else
                Builder.CreateRet(UndefValue::get(retTy));
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
    int lambdaCounter = 0;

    Value* handleExpression(Node* node);

    // Box any Value* to i8* for storage in a lambda env or return from lambda
    Value* boxToVoidPtr(Value* val) {
        Type* i8PtrTy = Type::getInt8PtrTy(Context);
        if (!val || val->getType()->isVoidTy())
            return ConstantPointerNull::get(cast<PointerType>(i8PtrTy));
        if (val->getType() == i8PtrTy)
            return val;
        if (val->getType()->isPointerTy())
            return Builder.CreateBitCast(val, i8PtrTy);
        if (val->getType()->isIntegerTy())
            return Builder.CreateIntToPtr(val, i8PtrTy);
        if (val->getType()->isDoubleTy()) {
            Value* asInt = Builder.CreateBitCast(val, Type::getInt64Ty(Context));
            return Builder.CreateIntToPtr(asInt, i8PtrTy);
        }
        return ConstantPointerNull::get(cast<PointerType>(i8PtrTy));
    }

    // Unbox an i8* argument to the expected type based on the lambda parameter annotation
    Value* unboxLambdaParam(Value* raw, const std::string& type) {
        if (type == "Int" || type == "Int32")
            return Builder.CreatePtrToInt(raw, Type::getInt32Ty(Context));
        if (type == "Bool")
            return Builder.CreateICmpNE(raw, ConstantPointerNull::get(cast<PointerType>(raw->getType())), "lambdabool");
        if (type == "Float" || type == "Double") {
            Value* asInt = Builder.CreatePtrToInt(raw, Type::getInt64Ty(Context));
            return Builder.CreateBitCast(asInt, Type::getDoubleTy(Context));
        }
        if (!type.empty() && StructTypes.count(type))
            return Builder.CreateBitCast(raw, PointerType::getUnqual(StructTypes[type]));
        return raw; // pass through as i8*
    }

    // Collect free variable names referenced in the lambda body
    void collectFreeVars(LambdaNode* node, std::vector<std::string>& freeVars) {
        std::set<std::string> paramSet;
        for (const auto& p : node->params) paramSet.insert(p.name);
        std::set<std::string> seen;

        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n) return;
            if (auto lit = dynamic_cast<LiteralNode*>(n)) {
                const std::string& name = lit->value;
                if (name.empty() || std::isdigit((unsigned char)name[0])) return;
                if (name[0] == '"' || name[0] == '\'') return;
                if (name == "true" || name == "false" || name == "null" || name == "super") return;
                if (paramSet.count(name) || seen.count(name)) return;
                if (varGen->exists(name)) { seen.insert(name); freeVars.push_back(name); }
                return;
            }
            if (auto b = dynamic_cast<BinaryOpNode*>(n)) { walk(b->left.get()); walk(b->right.get()); }
            else if (auto c = dynamic_cast<CallNode*>(n)) {
                walk(c->callee.get());
                for (auto& a : c->args) walk(a.value.get());
            }
            else if (auto m = dynamic_cast<MemberAccessNode*>(n)) { walk(m->object.get()); }
            else if (auto v = dynamic_cast<VarDeclNode*>(n)) { walk(v->expression.get()); }
            else if (auto r = dynamic_cast<ReturnNode*>(n)) { walk(r->expression.get()); }
            else if (auto i = dynamic_cast<IfNode*>(n)) {
                walk(i->condition.get());
                for (auto& s : i->thenBranch) walk(s.get());
                for (auto& s : i->elseBranch) walk(s.get());
            }
            else if (auto lm = dynamic_cast<LambdaNode*>(n)) {
                // Nested lambda: only capture outer refs (don't recurse into inner params)
                if (lm->isExpression) walk(lm->exprBody.get());
                else for (auto& s : lm->stmtBody) walk(s.get());
            }
        };

        if (node->isExpression) walk(node->exprBody.get());
        else for (auto& s : node->stmtBody) walk(s.get());
    }

    Value* handleLambda(LambdaNode* node) {
        int idx = lambdaCounter++;
        Type* i8PtrTy = Type::getInt8PtrTy(Context);

        // 1. Collect captures
        std::vector<std::string> captureNames;
        collectFreeVars(node, captureNames);

        // Resolve current values of captured variables
        std::vector<Value*> captureValues;
        std::vector<Type*>  captureTypes;
        for (const auto& name : captureNames) {
            Value* val = varGen->resolveVariable(name);
            if (val) {
                captureValues.push_back(val);
                captureTypes.push_back(val->getType());
            }
        }

        // 2. Create env struct type (may be empty if no captures)
        StructType* envType = StructType::create(
            Context, captureTypes, "__lambda_env_" + std::to_string(idx));

        // 3. Build lambda function: i8*(i8* env, i8* arg0, ...)
        std::vector<Type*> fnParamTypes = {i8PtrTy}; // env
        for (size_t i = 0; i < node->params.size(); i++)
            fnParamTypes.push_back(i8PtrTy);

        FunctionType* lambdaFnTy = FunctionType::get(i8PtrTy, fnParamTypes, false);
        Function* lambdaFn = Function::Create(
            lambdaFnTy, Function::InternalLinkage,
            "__lambda_" + std::to_string(idx), TheModule.get());

        BasicBlock* entryBB = BasicBlock::Create(Context, "entry", lambdaFn);

        // Save outer state
        auto savedVars      = varGen->snapshot();
        BasicBlock* savedBB = Builder.GetInsertBlock();
        std::string savedClass = currentCodegenClass;

        // Switch into the lambda function
        Builder.SetInsertPoint(entryBB);
        varGen->clear();

        // 4. Load captures from env
        auto argIt = lambdaFn->arg_begin();
        Value* envArg = &*argIt++;
        envArg->setName("env");

        if (!captureNames.empty()) {
            Value* envPtr = Builder.CreateBitCast(
                envArg, PointerType::getUnqual(envType), "env_ptr");
            for (size_t i = 0; i < captureNames.size(); i++) {
                Value* fieldPtr = Builder.CreateStructGEP(
                    envType, envPtr, (unsigned)i, captureNames[i] + "_ptr");
                Value* loaded = Builder.CreateLoad(captureTypes[i], fieldPtr, captureNames[i]);
                varGen->defineArgument(captureNames[i], loaded);
            }
        }

        // 5. Bind lambda params (unboxed from i8*)
        for (size_t i = 0; i < node->params.size(); i++) {
            Value* rawArg = &*argIt++;
            rawArg->setName(node->params[i].name);
            Value* typed = unboxLambdaParam(rawArg, node->params[i].type);
            varGen->defineArgument(node->params[i].name, typed);
        }

        // 6. Generate body and return
        if (node->isExpression) {
            Value* result = handleExpression(node->exprBody.get());
            Value* boxed  = boxToVoidPtr(result);
            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateRet(boxed);
        } else {
            for (auto& stmt : node->stmtBody)
                handleStatement(stmt.get(), lambdaFn);
            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateRet(ConstantPointerNull::get(cast<PointerType>(i8PtrTy)));
        }

        // Restore outer state
        Builder.SetInsertPoint(savedBB);
        varGen->restore(savedVars);
        currentCodegenClass = savedClass;

        // 7. Allocate env struct and store captured values
        FunctionCallee gcMallocFn = TheModule->getOrInsertFunction(
            "GC_malloc", i8PtrTy, Type::getInt64Ty(Context));

        Value* envAlloc;
        if (captureNames.empty()) {
            envAlloc = ConstantPointerNull::get(cast<PointerType>(i8PtrTy));
        } else {
            uint64_t envSz = TheModule->getDataLayout().getTypeAllocSize(envType);
            Value* envRaw = Builder.CreateCall(
                gcMallocFn, {ConstantInt::get(Type::getInt64Ty(Context), envSz)}, "env_alloc");
            Value* envCast = Builder.CreateBitCast(envRaw, PointerType::getUnqual(envType));
            for (size_t i = 0; i < captureNames.size(); i++) {
                Value* fieldPtr = Builder.CreateStructGEP(envType, envCast, (unsigned)i);
                Builder.CreateStore(captureValues[i], fieldPtr);
            }
            envAlloc = envRaw;
        }

        // 8. Allocate and populate Callable struct
        StructType* callableTy = StructTypes["Callable"];
        uint64_t callSz = TheModule->getDataLayout().getTypeAllocSize(callableTy);
        Value* callableRaw = Builder.CreateCall(
            gcMallocFn, {ConstantInt::get(Type::getInt64Ty(Context), callSz)}, "callable");
        Value* callablePtr = Builder.CreateBitCast(
            callableRaw, PointerType::getUnqual(callableTy));

        Value* fnField  = Builder.CreateStructGEP(callableTy, callablePtr, 0);
        Value* envField = Builder.CreateStructGEP(callableTy, callablePtr, 1);
        Builder.CreateStore(Builder.CreateBitCast(lambdaFn, i8PtrTy), fnField);
        Builder.CreateStore(envAlloc, envField);

        return callablePtr;
    }

    // Emit an indirect call through a Callable* with boxed arguments
    Value* emitCallableCall(Value* callablePtr, const std::vector<Value*>& argVals) {
        Type* i8PtrTy = Type::getInt8PtrTy(Context);
        StructType* callableTy = StructTypes["Callable"];

        Value* fnField  = Builder.CreateStructGEP(callableTy, callablePtr, 0);
        Value* envField = Builder.CreateStructGEP(callableTy, callablePtr, 1);
        Value* fnPtr    = Builder.CreateLoad(i8PtrTy, fnField, "fn_ptr");
        Value* envPtr   = Builder.CreateLoad(i8PtrTy, envField, "env_ptr");

        // Build call type: i8*(i8* env, i8* arg, ...)
        std::vector<Type*> callTypes = {i8PtrTy};
        for (size_t i = 0; i < argVals.size(); i++) callTypes.push_back(i8PtrTy);
        FunctionType* callTy = FunctionType::get(i8PtrTy, callTypes, false);

        Value* fnCast = Builder.CreateBitCast(fnPtr, PointerType::getUnqual(callTy));

        std::vector<Value*> callArgs = {envPtr};
        for (Value* v : argVals) callArgs.push_back(boxToVoidPtr(v));

        return Builder.CreateCall(callTy, fnCast, callArgs, "lambda_result");
    }

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
            if (verbose) std::cerr << "[Codegen]     handleCall: callee = " << lit->value << "\n";

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

            Function* func = resolveFunction(lit->value, currentCodegenClass);
            if (func) return generateGlobalCall(func, call);

            // Check if identifier resolves to a Callable variable (lambda stored in variable)
            if (varGen->exists(lit->value)) {
                Value* val = varGen->resolveVariable(lit->value);
                if (val && val->getType()->isPointerTy() &&
                    val->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(val->getType()->getPointerElementType());
                    if (st == StructTypes["Callable"]) {
                        std::vector<Value*> argVals;
                        for (auto& a : call->args) argVals.push_back(handleExpression(a.value.get()));
                        Value* raw = emitCallableCall(val, argVals);
                        // Unbox the result based on the tracked return type
                        std::string retType = callableReturnTypes.count(lit->value)
                                              ? callableReturnTypes.at(lit->value) : "";
                        if (retType == "Int" || retType == "Int32")
                            return Builder.CreatePtrToInt(raw, Type::getInt32Ty(Context));
                        if (retType == "Bool")
                            return Builder.CreateICmpNE(raw, ConstantPointerNull::get(cast<PointerType>(raw->getType())));
                        if (retType == "Double" || retType == "Float") {
                            Value* asInt = Builder.CreatePtrToInt(raw, Type::getInt64Ty(Context));
                            return Builder.CreateBitCast(asInt, Type::getDoubleTy(Context));
                        }
                        if (!retType.empty() && StructTypes.count(retType))
                            return Builder.CreateBitCast(raw, PointerType::getUnqual(StructTypes[retType]));
                        return raw;
                    }
                }
            }

            fatalError("Unknown function '" + lit->value + "'", call->line, call->col);
        }

        if (auto member = dynamic_cast<MemberAccessNode*>(call->callee.get())) {
            if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                if (verbose) std::cerr << "[Codegen]     handleCall: " << lit->value << "." << member->memberName << "\n";
                if (activeModuleAliases.count(lit->value)) {
                    std::string memberName = member->memberName;
                    if (StructTypes.count(memberName)) {
                        std::vector<Value*> args;
                        for (auto& a : call->args) args.push_back(handleExpression(a.value.get()));
                        return structGen->allocateAndInit(memberName, args);
                    }
                    
                    Function* func = resolveFunction(memberName);
                    if (!func)
                        fatalError("Unknown function '" + memberName + "'", member->line, member->col);
                    return generateGlobalCall(func, call);
                }
            }

            // Enum .str() / .name()
            if (member->memberName == "str" || member->memberName == "name") {
                if (auto* objLit = dynamic_cast<LiteralNode*>(member->object.get())) {
                    std::string enumType;
                    if (varEnumTypes.count(objLit->value))
                        enumType = varEnumTypes[objLit->value];
                    else if (enumVariants.count(objLit->value))
                        enumType = objLit->value; // e.g. Direction.str() — unlikely but handle it
                    if (!enumType.empty()) {
                        Function* strFn = TheModule->getFunction("__" + enumType + "_str");
                        if (strFn) {
                            Value* val = handleExpression(member->object.get());
                            if (val) return Builder.CreateCall(strFn, {val});
                        }
                    }
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

                    // Any* — unbox to String for method dispatch (most common case)
                    // For full dynamic dispatch, a switch on tag would be needed.
                    if (typeName == "Any") {
                        Function* anyStr = TheModule->getFunction("Core_Primitives_Any_to_str");
                        if (anyStr) {
                            objPtr = Builder.CreateCall(anyStr, {objPtr});
                            typeName = "String";
                        }
                    }
                } else if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isPointerTy()) {
                    // Double pointer — load once more and retry
                    objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);
                    if (objPtr->getType()->isPointerTy() && objPtr->getType()->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(objPtr->getType()->getPointerElementType());
                        typeName = st->getName().str();
                        if (typeName.find("struct.") == 0) typeName = typeName.substr(7);
                    } else {
                        if (verbose) std::cerr << "[Codegen] WARNING: cannot resolve type for method call '." << member->memberName << "' after double-pointer load\n";
                        return nullptr;
                    }
                } else {
                    if (verbose) {
                        std::string typStr;
                        llvm::raw_string_ostream rso(typStr);
                        objPtr->getType()->print(rso);
                        std::cerr << "[Codegen] WARNING: unhandled object type '" << rso.str() << "' for method call '." << member->memberName << "' — returning nullptr\n";
                    }
                    return nullptr;
                }
            }

            if (objPtr) {
                Value* extResult = typeExtensions->tryHandleMethod(typeName, member->memberName, objPtr, call->args, [this](Node* n) { return this->handleExpression(n); });
                if (extResult) return extResult;
            }

            // --- Lambda-aware List functional methods ---
            // These are handled via normal function resolution (the methods are declared
            // as extern in list.qk so resolveFunction returns the correct linkage name).
            // We only intercept here to evaluate the Callable* argument correctly.
            static const std::set<std::string> listFunctionalMethods = {
                "map", "filter", "each", "reduce", "any", "all", "find"
            };
            if (objPtr && listFunctionalMethods.count(member->memberName) &&
                typeName.find("List") != std::string::npos && !call->args.empty()) {
                Type* i8PtrTy   = Type::getInt8PtrTy(Context);
                Type* listPtrTy = objPtr->getType();
                StructType* callTy = StructTypes["Callable"];
                Type* callPtrTy = PointerType::getUnqual(callTy);

                // Find the function via the normal resolution path (handles linkage names)
                Function* fn = resolveFunction(typeName + "_" + member->memberName);
                if (!fn) fn = TheModule->getFunction(typeName + "_" + member->memberName);

                if (member->memberName == "reduce" && call->args.size() >= 2) {
                    Value* initial  = handleExpression(call->args[0].value.get());
                    Value* callable = handleExpression(call->args[1].value.get());
                    if (callable && callable->getType() != callPtrTy)
                        callable = Builder.CreateBitCast(callable, callPtrTy);
                    if (fn) return Builder.CreateCall(fn, {objPtr, boxToVoidPtr(initial), callable});
                } else {
                    Value* callable = handleExpression(call->args[0].value.get());
                    if (callable && callable->getType() != callPtrTy)
                        callable = Builder.CreateBitCast(callable, callPtrTy);
                    if (fn) {
                        Value* result = Builder.CreateCall(fn, {objPtr, callable});
                        // any/all: widen i1 to Bool (return from i32 C function handled by compiler)
                        return result;
                    }
                }
            }
            // --- end lambda List methods ---

            std::string funcName = typeName + "_" + member->memberName;
            Function* func = TheModule->getFunction(funcName);

            // Fallback: try triple-underscore operator convention (e.g. List___get, Map___get)
            if (!func) func = TheModule->getFunction(typeName + "___" + member->memberName);

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
                            if (!foundFunc) foundFunc = TheModule->getFunction(parentName + "___" + member->memberName);
                            if (foundFunc) return foundFunc;
                            foundFunc = searchHierarchy(parentName);
                            if (foundFunc) return foundFunc;
                        }
                    }
                    return nullptr;
                };
                func = searchHierarchy(typeName);
            }

            // --- LINKAGE NAME FALLBACK ---
            // Extern struct methods may be registered under a full linkage name
            // (e.g. "Core_String_String_contains") — try resolveFunction for both
            // the plain and triple-underscore key forms.
            if (!func) func = resolveFunction(typeName + "_"   + member->memberName);
            if (!func) func = resolveFunction(typeName + "___" + member->memberName);

                        // --- METHOD ALIAS TABLE ---
            // Maps Quirk method names to their actual LLVM function names.
            // Needed when the Quirk name differs from the compiled name
            // (e.g. Exception.traceback() -> Exception_print_traceback).
            if (!func) {
                static const std::map<std::string, std::map<std::string, std::string>> methodAliases = {
                    {"Exception", {
                        {"traceback", "Exception_print_traceback"},
                    }},
                };
                auto typeIt = methodAliases.find(typeName);
                if (typeIt != methodAliases.end()) {
                    auto aliasIt = typeIt->second.find(member->memberName);
                    if (aliasIt != typeIt->second.end()) {
                        func = TheModule->getFunction(aliasIt->second);
                    }
                }
                // Also search parent types for the alias
                if (!func && structHierarchy.count(typeName)) {
                    for (const auto& parent : structHierarchy[typeName]) {
                        auto parentIt = methodAliases.find(parent);
                        if (parentIt != methodAliases.end()) {
                            auto aliasIt = parentIt->second.find(member->memberName);
                            if (aliasIt != parentIt->second.end()) {
                                func = TheModule->getFunction(aliasIt->second);
                                if (func) break;
                            }
                        }
                    }
                }
            }
            // --------------------------

            if (!func) {
                // Fallback: if the name is an actual struct field (e.g. str.length()),
                // return the field value directly. This handles field-as-method calls.
                // Only do this when objPtr is known and the field exists in structLayouts.
                if (objPtr) {
                    Value* fieldVal = structGen->generateMemberAccess(objPtr, member->memberName);
                    if (fieldVal) return fieldVal;
                }
                fatalError("Unknown method '" + typeName + "." + member->memberName + "'",
                           member->line, member->col);
            }

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

    // Resolves a Quirk function name to an LLVM Function*, consulting
    // functionDeclarations for the stored linkageName before falling back
    // to a direct module lookup.
    Function* resolveFunction(const std::string& name,
                              const std::string& classPrefix = "") {
        if (functionDeclarations.count(name)) {
            const std::string& ln = functionDeclarations[name]->linkageName;
            Function* f = TheModule->getFunction(ln.empty() ? name : ln);
            if (f) return f;
        }
        Function* f = TheModule->getFunction(name);
        if (f) return f;
        if (!classPrefix.empty())
            f = TheModule->getFunction(classPrefix + "_" + name);
        return f;
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

                // Auto-box to Any* when function expects it
                if (expectedType->isPointerTy() && expectedType->getPointerElementType()->isStructTy()) {
                    std::string pName = cast<StructType>(expectedType->getPointerElementType())->getName().str();
                    if (pName == "Any" || pName == "struct.Any") {
                        argVal = emitBox(argVal);
                        if (argVal->getType() != expectedType)
                            argVal = Builder.CreateBitCast(argVal, expectedType);
                        finalArgs.push_back(argVal);
                        continue;
                    }
                }

                if (argVal->getType()->isIntegerTy(1) && expectedType->isIntegerTy(32)) {
                    argVal = Builder.CreateZExt(argVal, Type::getInt32Ty(Context));
                }

                if (argVal->getType()->isPointerTy() &&
                    argVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                    expectedType->isPointerTy() &&
                    expectedType->getPointerElementType()->isStructTy()) {
                    
                    StructType* st = cast<StructType>(expectedType->getPointerElementType());
                    
                    if (st->getName() == "String") {
                        argVal = Builder.CreateBitCast(argVal, expectedType);
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
                // --- FIX: Convert Objects to Strings before passing to Variadic Functions ---
                Type* ty = vArg->getType();
                if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(ty->getPointerElementType());
                    // If it's not already a String, call __str()
                    if (st->getName().str().find("String") == std::string::npos) {
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        
                        Function* strFunc = TheModule->getFunction(sName + "___str");
                        if (strFunc) {
                            vArg = Builder.CreateCall(strFunc, {vArg});
                            // Ensure the result is treated as a String Object (pointer), not raw i8*
                            if (vArg->getType()->isPointerTy() && 
                                vArg->getType()->getPointerElementType()->isIntegerTy(8)) {
                                std::vector<Value*> boxArgs = {vArg};
                                vArg = structGen->allocateAndInit("String", boxArgs);
                            }
                        }
                    }
                }
                // ---------------------------------------------------------------------------

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

        // Identify node type for logging
        auto nodeTypeName = [&]() -> std::string {
            if (dynamic_cast<VarDeclNode*>(node))   return "VarDecl";
            if (dynamic_cast<CallNode*>(node))       return "Call";
            if (dynamic_cast<IfNode*>(node))         return "If";
            if (dynamic_cast<WhileNode*>(node))      return "While";
            if (dynamic_cast<ForNode*>(node))        return "For";
            if (dynamic_cast<ReturnNode*>(node))     return "Return";
            if (dynamic_cast<BreakNode*>(node))      return "Break";
            if (dynamic_cast<ContinueNode*>(node))   return "Continue";
            if (dynamic_cast<ThrowNode*>(node))      return "Throw";
            if (dynamic_cast<TryCatchNode*>(node))   return "TryCatch";
            if (dynamic_cast<WithNode*>(node))       return "With";
            if (dynamic_cast<TriggerNode*>(node))    return "Trigger";
            if (dynamic_cast<UseNode*>(node))        return "Use";
            return "Unknown";
        };
        if (verbose) std::cerr << "[Codegen]   handleStatement: " << nodeTypeName() << "\n";

        if (auto u = dynamic_cast<UseNode*>(node)) handleUse(u);
        else if (auto vdecl = dynamic_cast<VarDeclNode*>(node)) handleVarDecl(vdecl);
        else if (auto call = dynamic_cast<CallNode*>(node)) handleCall(call);
        else if (auto i = dynamic_cast<IfNode*>(node)) handleIf(i, parentFunc);
        else if (auto w = dynamic_cast<WhileNode*>(node)) handleWhile(w, parentFunc);
        else if (dynamic_cast<BreakNode*>(node)) {
            if (!flowGen->breakStack.empty()) {
                Builder.CreateBr(flowGen->breakStack.back());
                // Create a dead block so subsequent IR in this branch has an insert point
                BasicBlock* dead = BasicBlock::Create(Context, "after_break", parentFunc);
                Builder.SetInsertPoint(dead);
            }
        }
        else if (dynamic_cast<ContinueNode*>(node)) {
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
                varGen.get(),
                // Linkage-aware resolver: tries module first, then functionDeclarations
                [this](const std::string& shortName) -> Function* {
                    // Try the short name first (for Quirk-defined iterators)
                    if (Function* f = TheModule->getFunction(shortName)) return f;
                    // Try functionDeclarations linkageName fallback
                    if (functionDeclarations.count(shortName)) {
                        const std::string& ln = functionDeclarations[shortName]->linkageName;
                        if (!ln.empty()) return TheModule->getFunction(ln);
                    }
                    return nullptr;
                });
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
        if (auto lhs = dynamic_cast<LiteralNode*>(vdecl->lhs.get()))
            if (verbose) std::cerr << "[Codegen]     handleVarDecl: " << lhs->value << " (op: " << vdecl->op << ")\n";

        // Track Callable return types for direct lambda invocation
        if (auto lambda = dynamic_cast<LambdaNode*>(vdecl->expression.get())) {
            if (!lambda->inferredReturnType.empty()) {
                if (auto lhsLit = dynamic_cast<LiteralNode*>(vdecl->lhs.get()))
                    callableReturnTypes[lhsLit->value] = lambda->inferredReturnType;
            }
        }

        Value* val = handleExpression(vdecl->expression.get());
        if (!val || val->getType()->isVoidTy()) return;

        if (!vdecl->typeAnnotation.empty()) {
            // Target type is explicitly Any — box the value
            if (vdecl->typeAnnotation == "Any") {
                val = emitBox(val);
            } else {
                Type* targetType = typeGen->getLLVMType(vdecl->typeAnnotation);

                // Source is Any* and target is a concrete type — unbox
                if (isAnyType(val)) {
                    val = emitUnboxToType(val, targetType);
                }
                // Source is type-erased i8* (Map_get/List_get/lambda result)
                else if (val->getType()->isPointerTy() &&
                         val->getType()->getPointerElementType()->isIntegerTy(8)) {
                    if (targetType->isIntegerTy())
                        // Unbox tagged-pointer integer: ptrtoint(i8*, i32)
                        val = Builder.CreatePtrToInt(val, targetType);
                    else if (targetType->isPointerTy() && targetType->getPointerElementType()->isStructTy())
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
        }

        // --- NEW: Helper to safely apply +=, -=, etc. including String concatenation ---
        auto applyCompoundAssignment = [&](std::string op, Value* wasVal, Value* newVal) -> Value* {
            if (op == "+=") {
                // 1. Check if the target is already a String struct
                if (wasVal->getType()->isPointerTy() && wasVal->getType()->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(wasVal->getType()->getPointerElementType());
                    if (st->getName().contains("String")) {
                        Function* addFunc = TheModule->getFunction("Core_String_String___add");
                        if (addFunc) return Builder.CreateCall(addFunc, {wasVal, newVal});
                    }
                }
                
                // 2. NEW FIX: Target is a raw C-String (i8*). Box it safely!
                if (wasVal->getType()->isPointerTy() && wasVal->getType()->getPointerElementType()->isIntegerTy(8)) {
                    std::vector<Value*> boxArgs = {wasVal};
                    Value* boxedStr = structGen->allocateAndInit("String", boxArgs);
                    Function* addFunc = TheModule->getFunction("Core_String_String___add");
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

            // Track enum type for .str() calls
            if (auto* rhsMember = dynamic_cast<MemberAccessNode*>(vdecl->expression.get())) {
                if (auto* rhsLit = dynamic_cast<LiteralNode*>(rhsMember->object.get())) {
                    if (enumVariants.count(rhsLit->value)) {
                        varEnumTypes[lhs->value] = rhsLit->value;
                    }
                }
            }

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
                    // Linkage name fallback: e.g. "Map___set" → "Core_Collections_Map_Map___set"
                    if (!func) {
                        std::string key = structName + "___set";
                        if (functionDeclarations.count(key)) {
                            const std::string& ln = functionDeclarations[key]->linkageName;
                            if (!ln.empty()) func = TheModule->getFunction(ln);
                        }
                    }
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
        if (verbose) std::cerr << "[Codegen]     handleIf\n";
        flowGen->generateIf(node, parentFunc,
            [this](Node* n) -> Value* {
                Value* cond = this->handleExpression(n);
                if (!cond) {
                    if (verbose) std::cerr << "[Codegen] WARNING: if-condition evaluated to nullptr, defaulting to false\n";
                    return ConstantInt::getFalse(Context);
                }
                return cond;
            },
            [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); });
    }

    // ===================================================
    //  Any helpers
    // ===================================================

    bool isAnyType(Value* v) {
        if (!v || !v->getType()->isPointerTy()) return false;
        Type* el = v->getType()->getPointerElementType();
        if (!el->isStructTy()) return false;
        std::string name = cast<StructType>(el)->getName().str();
        return name == "Any" || name == "struct.Any";
    }

    // Emit a box_* call wrapping val into Any*
    Value* emitBox(Value* v) {
        if (!v) return Constant::getNullValue(Type::getInt8PtrTy(Context));
        if (isAnyType(v)) return v; // already boxed

        Type* ty = v->getType();

        auto callBox = [&](const std::string& fname, std::vector<Value*> args) -> Value* {
            Function* f = TheModule->getFunction(fname);
            if (f) return Builder.CreateCall(f, args);
            return Constant::getNullValue(PointerType::getUnqual(StructTypes.count("Any") ? (Type*)StructTypes["Any"] : (Type*)Type::getInt8Ty(Context)));
        };

        if (ty->isIntegerTy(1)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("Core_Primitives_Any_box_bool", {ext});
        }
        if (ty->isIntegerTy(8)) {
            Value* ext = Builder.CreateZExt(v, Type::getInt32Ty(Context));
            return callBox("Core_Primitives_Any_box_char", {ext});
        }
        if (ty->isIntegerTy()) {
            Value* c = Builder.CreateIntCast(v, Type::getInt32Ty(Context), true);
            return callBox("Core_Primitives_Any_box_int", {c});
        }
        if (ty->isDoubleTy()) return callBox("Core_Primitives_Any_box_double", {v});

        if (ty->isPointerTy()) {
            Type* el = ty->getPointerElementType();
            if (el->isStructTy()) {
                std::string name = cast<StructType>(el)->getName().str();
                if (name.find("struct.") == 0) name = name.substr(7);
                // Already Any* — pass through
                if (name == "Any") return v;
                // box_* are declared as i8*(i8*) — must bitcast struct ptr to i8* first
                Value* asPtr = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
                if (name.find("String") != std::string::npos) return callBox("Core_Primitives_Any_box_string", {asPtr});
                if (name.find("List")   != std::string::npos) return callBox("Core_Primitives_Any_box_list",   {asPtr});
                if (name.find("Map")    != std::string::npos) return callBox("Core_Primitives_Any_box_map",    {asPtr});
                // Other struct — call __str, fall back to __repr, then box as String
                std::string strMethod = name + "___str";
                Function* strFunc = TheModule->getFunction(strMethod);
                if (!strFunc) strFunc = TheModule->getFunction(name + "__str");
                if (!strFunc) strFunc = TheModule->getFunction(name + "___repr");
                if (!strFunc) strFunc = TheModule->getFunction(name + "__repr");
                if (strFunc) {
                    Value* strObj = Builder.CreateCall(strFunc, {v});
                    if (strObj->getType()->isPointerTy() &&
                        strObj->getType()->getPointerElementType()->isIntegerTy(8)) {
                        std::vector<Value*> args = {strObj};
                        strObj = structGen->allocateAndInit("String", args);
                    }
                    return callBox("Core_Primitives_Any_box_string", {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
                }
                return callBox("Core_Primitives_Any_box_ptr", {asPtr});
            }
            if (el->isIntegerTy(8)) {
                // raw i8* — may be String*, Any*, or tagged integer; convert safely
                Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                if (!opaqueToStr) {
                    Type* retTy = StructTypes.count("String")
                        ? (Type*)PointerType::getUnqual(StructTypes["String"])
                        : (Type*)Type::getInt8PtrTy(Context);
                    FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                    opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule.get());
                }
                Value* strObj = Builder.CreateCall(opaqueToStr, {v});
                return callBox("Core_Primitives_Any_box_string", {Builder.CreateBitCast(strObj, Type::getInt8PtrTy(Context))});
            }
        }
        return callBox("Core_Primitives_Any_box_null", {});
    }

    // Emit unboxing from Any* to a specific LLVM target type
    Value* emitUnboxToType(Value* anyPtr, Type* targetType) {
        // Ensure we have Any* not i8*
        if (anyPtr->getType()->isPointerTy() &&
            anyPtr->getType()->getPointerElementType()->isIntegerTy(8) &&
            StructTypes.count("Any")) {
            anyPtr = Builder.CreateBitCast(anyPtr, PointerType::getUnqual(StructTypes["Any"]));
        }

        if (targetType->isIntegerTy(32)) {
            Function* f = TheModule->getFunction("Core_Primitives_Any_to_int");
            if (f) return Builder.CreateCall(f, {anyPtr});
        }
        if (targetType->isDoubleTy()) {
            Function* f = TheModule->getFunction("Core_Primitives_Any_to_float");
            if (f) return Builder.CreateCall(f, {anyPtr});
        }
        if (targetType->isPointerTy() && targetType->getPointerElementType()->isStructTy()) {
            std::string name = cast<StructType>(targetType->getPointerElementType())->getName().str();
            if (name.find("struct.") == 0) name = name.substr(7);
            if (name.find("String") != std::string::npos) {
                Function* f = TheModule->getFunction("Core_Primitives_Any_to_str");
                if (f) return Builder.CreateCall(f, {anyPtr});
            }
        }
        return Builder.CreateBitCast(anyPtr, targetType);
    }

    void handleWhile(WhileNode* node, Function* parentFunc) {
        if (verbose) std::cerr << "[Codegen]     handleWhile\n";
        flowGen->generateWhile(node, parentFunc,
            [this](Node* n) -> Value* {
                Value* cond = this->handleExpression(n);
                if (!cond) {
                    if (verbose) std::cerr << "[Codegen] WARNING: while-condition evaluated to nullptr, defaulting to false\n";
                    return ConstantInt::getFalse(Context);
                }
                return cond;
            },
            [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); });
    }

    Value* handleConstructor(ConstructorNode* node) {
        if (verbose) std::cerr << "[Codegen]     handleConstructor\n";
        return structGen->generateConstructor(node, [this](Node* n) { return this->handleExpression(n); });
    }

    void handleWith(WithNode* node, Function* parentFunc) {
        if (verbose) std::cerr << "[Codegen]     handleWith: var = " << node->varName << "\n";
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

        // Only emit __exit cleanup if the body didn't already terminate (e.g. via return)
        if (!Builder.GetInsertBlock()->getTerminator()) {
            std::string exitName = typeName + "___exit";
            Function* exitFunc = TheModule->getFunction(exitName);
            if (exitFunc) Builder.CreateCall(exitFunc, {resource});
        }
    }
};

Value* LLVMCodegen::handleExpression(Node* node) {
    if (auto call   = dynamic_cast<CallNode*>(node))   return handleCall(call);
    if (auto lambda = dynamic_cast<LambdaNode*>(node)) return handleLambda(lambda);

    if (auto lit = dynamic_cast<LiteralNode*>(node)) {
        if (lit->value == "true") return ConstantInt::getTrue(Context);
        if (lit->value == "false") return ConstantInt::getFalse(Context);

        if (lit->value == "super") {
            Value* selfVal = varGen->resolveVariable("self");
            if (!selfVal || structHierarchy[currentCodegenClass].empty()) return nullptr;
            std::string parentName = structHierarchy[currentCodegenClass][0];
            return Builder.CreateBitCast(selfVal, PointerType::getUnqual(StructTypes[parentName]));
        }

        if (std::isdigit(lit->value[0])) {
            if (lit->value.find('.') != std::string::npos) return ConstantFP::get(Context, APFloat(std::stod(lit->value)));
            return ConstantInt::get(Type::getInt32Ty(Context), std::stoi(lit->value));
        }
        
        if (lit->value.size() >= 2 && lit->value.front() == '"') {
            std::string rawStr = unescapeString(lit->value.substr(1, lit->value.size() - 2));
            Value* rawPtr = Builder.CreateGlobalStringPtr(rawStr);
            std::vector<Value*> args = {rawPtr};
            return structGen->allocateAndInit("String", args);
        }

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

        // ── `val is TypeName` ─────────────────────────────────────────────────
        // RHS is always a LiteralNode whose value is the bare type name string.
        // Emits: Core_Primitives_Any_isinstance(val_as_i8*, type_name_String*)
        if (binOp->op == "is") {
            Value* lval = handleExpression(binOp->left.get());
            if (!lval) return ConstantInt::getFalse(Context);

            std::string typeName;
            if (auto* lit = dynamic_cast<LiteralNode*>(binOp->right.get()))
                typeName = lit->value;

            // Build a null-terminated C string constant for the type name
            Constant* typeNameCStr = Builder.CreateGlobalStringPtr(typeName, "type_name_cstr");

            // Get or declare make_String
            Function* makeStrFn = TheModule->getFunction("make_String");
            if (!makeStrFn) {
                // Declare as (i8*) -> i8* (opaque pointer to String)
                FunctionType* ft = FunctionType::get(
                    Type::getInt8PtrTy(Context), {Type::getInt8PtrTy(Context)}, false);
                makeStrFn = Function::Create(ft, Function::ExternalLinkage, "make_String", TheModule.get());
            }
            Value* typeStrVal = Builder.CreateCall(makeStrFn, {typeNameCStr}, "type_str");

            // Cast lval to i8* for isinstance
            Value* lvalI8 = lval->getType()->isPointerTy()
                ? Builder.CreateBitCast(lval, Type::getInt8PtrTy(Context))
                : Builder.CreateIntToPtr(lval, Type::getInt8PtrTy(Context));

            // Get or declare Core_Primitives_Any_isinstance(i8*, i8*) -> i32
            Function* isinstanceFn = TheModule->getFunction("Core_Primitives_Quirk_isinstance");
            if (!isinstanceFn) {
                FunctionType* ft = FunctionType::get(
                    Type::getInt32Ty(Context),
                    {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)}, false);
                isinstanceFn = Function::Create(ft, Function::ExternalLinkage, "Core_Primitives_Quirk_isinstance", TheModule.get());
            }
            Value* result = Builder.CreateCall(isinstanceFn, {lvalI8, typeStrVal}, "isinstance_result");
            return Builder.CreateICmpNE(result, ConstantInt::get(Type::getInt32Ty(Context), 0), "is_bool");
        }
        // ── end `is` ─────────────────────────────────────────────────────────

        // ── `elem in list` ───────────────────────────────────────────────────
        if (binOp->op == "in") {
            Value* elem = handleExpression(binOp->left.get());
            Value* list = handleExpression(binOp->right.get());
            if (!elem || !list) return ConstantInt::getFalse(Context);

            Type* i8PtrTy = Type::getInt8PtrTy(Context);
            Value* elemI8 = elem->getType() == i8PtrTy
                ? elem
                : (elem->getType()->isPointerTy()
                    ? Builder.CreateBitCast(elem, i8PtrTy)
                    : Builder.CreateIntToPtr(elem, i8PtrTy));

            Value* listI8 = list->getType() == i8PtrTy
                ? list
                : Builder.CreateBitCast(list, i8PtrTy);

            Function* containsFn = TheModule->getFunction("Core_Collections_List_List_contains");
            if (!containsFn) {
                FunctionType* ft = FunctionType::get(
                    Type::getInt32Ty(Context), {i8PtrTy, i8PtrTy}, false);
                containsFn = Function::Create(ft, Function::ExternalLinkage,
                    "Core_Collections_List_List_contains", TheModule.get());
            }
            Value* result = Builder.CreateCall(containsFn, {listI8, elemI8}, "in_result");
            return Builder.CreateICmpNE(result, ConstantInt::get(Type::getInt32Ty(Context), 0), "in_bool");
        }
        // ── end `in` ─────────────────────────────────────────────────────────

        Value* L = handleExpression(binOp->left.get());
        Value* R = handleExpression(binOp->right.get());
        if (!L || !R) return nullptr;

        if (binOp->op == "[]") {
            // ... (Keep existing [] logic exactly as is) ...
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
                // Linkage name fallback: e.g. "Map___get" → "Core_Collections_Map_Map___get"
                {
                    Function* func = nullptr;
                    std::string key = sName + "___get";
                    if (functionDeclarations.count(key)) {
                        const std::string& ln = functionDeclarations[key]->linkageName;
                        if (!ln.empty()) func = TheModule->getFunction(ln);
                    }
                    if (func) {
                        if (func->arg_size() >= 2) {
                            Type* keyType = func->getFunctionType()->getParamType(1);
                            if (R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isIntegerTy(8) &&
                                keyType->isPointerTy() && keyType->getPointerElementType()->isStructTy()) {
                                std::vector<Value*> args = {R};
                                R = structGen->allocateAndInit("String", args);
                            }
                            if (R->getType() != keyType && R->getType()->isPointerTy() && keyType->isPointerTy())
                                R = Builder.CreateBitCast(R, keyType);
                        }
                        return Builder.CreateCall(func, {L, R});
                    }
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
                    if (el->isIntegerTy(8)) return true; // Any / i8*
                }
                return false;
            };

            if (isStringType(L) || isStringType(R)) {
                auto makeString = [&](Value* val) -> Value* {
                    Type* ty = val->getType();
                    // 1. Already a String Struct
                    if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(ty->getPointerElementType());
                        if (st->getName().contains("String")) return val;
                        // ... (keep __str call logic) ...
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        // Try short name first, then search all functions for a matching ___str
                        Function* strFunc = TheModule->getFunction(sName + "___str");
                        if (!strFunc) {
                            // Search module for any function ending in <StructName>___str
                            std::string suffix = sName + "___str";
                            for (auto& F : *TheModule) {
                                if (F.getName().endswith(suffix)) { strFunc = &F; break; }
                            }
                        }
                        if (strFunc) {
                            Value* ret = Builder.CreateCall(strFunc, {val});
                            if (ret->getType()->isPointerTy() && ret->getType()->getPointerElementType()->isIntegerTy(8)) {
                                std::vector<Value*> boxArgs = {ret};
                                return structGen->allocateAndInit("String", boxArgs);
                            }
                            return ret;
                        }
                        // Last resort: call GC_malloc + init an empty string
                        return structGen->allocateAndInit("String", std::vector<Value*>{Builder.CreateGlobalStringPtr("[object]")});
                    }
                    
                    // Opaque i8* (Any-typed return from map.get etc.) — may be String* or tagged int.
                    // quirk_opaque_to_string handles both cases correctly.
                    if (ty->isPointerTy() && ty->getPointerElementType()->isIntegerTy(8)) {
                        StructType* strTy = StructTypes["String"];
                        Function* toStrFn = TheModule->getFunction("quirk_opaque_to_string");
                        if (!toStrFn) {
                            FunctionType* ft = FunctionType::get(
                                PointerType::getUnqual(strTy), {Type::getInt8PtrTy(Context)}, false);
                            toStrFn = Function::Create(ft, Function::ExternalLinkage,
                                "quirk_opaque_to_string", TheModule.get());
                        }
                        return Builder.CreateCall(toStrFn, {val});
                    }

                    if (ty->isIntegerTy(32)) { if (auto* f = TheModule->getFunction("Core_Primitives_Int_str")) return Builder.CreateCall(f, {val}); }
                    if (ty->isDoubleTy()) { if (auto* f = TheModule->getFunction("Core_Primitives_Double_str")) return Builder.CreateCall(f, {val}); }
                    if (ty->isIntegerTy(1)) {
                        if (auto* f = TheModule->getFunction("Core_Primitives_Bool_str")) {
                            Value* ext = Builder.CreateZExt(val, Type::getInt32Ty(Context));
                            return Builder.CreateCall(f, {ext});
                        }
                    }
                    return nullptr; 
                };

                Value* strL = makeString(L);
                Value* strR = makeString(R);

                if (strL && strR) {
                    Function* addFunc = TheModule->getFunction("Core_String_String___add");
                    if (addFunc) return Builder.CreateCall(addFunc, {strL, strR});
                }
                // If makeString failed for one side, don't fall through to integer add
                return nullptr;
            }
        }
        // Struct operator overloading: ==, !=, <, <=, >, >=
        static const std::map<std::string,std::string> opToMagic = {
            {"==","__eq"}, {"!=","__ne"}, {"<","__lt"}, {"<=","__le"}, {">","__gt"},{">=","__ge"}
        };
        if (opToMagic.count(binOp->op) &&
            L->getType()->isPointerTy() && R->getType()->isPointerTy()) {
            Type* elTy = L->getType()->getPointerElementType();
            if (elTy->isStructTy()) {
                std::string sName = cast<StructType>(elTy)->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);

                auto resolveFunc = [&](const std::string& suffix) -> Function* {
                    Function* f = TheModule->getFunction(sName + "___" + suffix.substr(2));
                    if (!f) for (auto& F : *TheModule)
                        if (F.getName().endswith("___" + suffix.substr(2))) { f = &F; break; }
                    return f;
                };

                std::string magicName = opToMagic.at(binOp->op);
                Function* opFunc = resolveFunc(magicName);
                // !=  falls back to !__eq if __ne is not defined
                bool negated = false;
                if (!opFunc && binOp->op == "!=") {
                    opFunc = resolveFunc("__eq");
                    negated = true;
                }
                if (opFunc) {
                    Value* result = Builder.CreateCall(opFunc, {L, R}, "cmp_result");
                    if (negated) result = Builder.CreateNot(result, "cmp_ne");
                    return result;
                }
            }
        }
        return mathGen->generateBinaryOp(binOp->op, L, R);
    }

    if (auto member = dynamic_cast<MemberAccessNode*>(node)) {
        // Enum variant access: Direction.North
        if (auto* lit = dynamic_cast<LiteralNode*>(member->object.get())) {
            if (enumVariants.count(lit->value)) {
                const auto& variants = enumVariants[lit->value];
                auto it = std::find(variants.begin(), variants.end(), member->memberName);
                if (it != variants.end()) {
                    int idx = (int)std::distance(variants.begin(), it);
                    return ConstantInt::get(Type::getInt32Ty(Context), idx);
                }
            }
        }

        // Magic attribute: __name → struct name string
        // If accessed on a Type* instance (e.g. self.__class.__name), read field 0.
        // Otherwise return the compile-time enclosing class name.
        if (member->memberName == "__name") {
            Value* obj = handleExpression(member->object.get());
            if (obj && obj->getType()->isPointerTy() && StructTypes.count("Type")) {
                llvm::Type* elTy = obj->getType()->getPointerElementType();
                if (elTy == StructTypes["Type"]) {
                    Value* fieldPtr = Builder.CreateStructGEP(StructTypes["Type"], obj, 0, "type_name_ptr");
                    return Builder.CreateLoad(PointerType::getUnqual(StructTypes["String"]), fieldPtr, "type_name");
                }
            }
            Value* rawPtr = Builder.CreateGlobalStringPtr(currentCodegenClass);
            return structGen->allocateAndInit("String", std::vector<Value*>{rawPtr});
        }

        // Magic attribute: __parent → parent struct name string (only valid on Type* instances)
        if (member->memberName == "__parent") {
            Value* obj = handleExpression(member->object.get());
            if (obj && obj->getType()->isPointerTy() && StructTypes.count("Type")) {
                llvm::Type* elTy = obj->getType()->getPointerElementType();
                if (elTy == StructTypes["Type"]) {
                    Value* fieldPtr = Builder.CreateStructGEP(StructTypes["Type"], obj, 1, "type_parent_ptr");
                    return Builder.CreateLoad(PointerType::getUnqual(StructTypes["String"]), fieldPtr, "type_parent");
                }
            }
            // Fallback: return compile-time parent name from structHierarchy
            std::string parentName = (!structHierarchy[currentCodegenClass].empty())
                ? structHierarchy[currentCodegenClass][0] : "";
            Value* rawPtr = Builder.CreateGlobalStringPtr(parentName);
            return structGen->allocateAndInit("String", std::vector<Value*>{rawPtr});
        }

        // Magic attribute: self.__class → Type{ name, parent } instance
        if (member->memberName == "__class") {
            StructType* typeST = StructTypes.count("Type") ? StructTypes["Type"] : nullptr;
            if (!typeST || typeST->isOpaque()) return nullptr;

            std::string parentName = (!structHierarchy[currentCodegenClass].empty())
                ? structHierarchy[currentCodegenClass][0] : "";

            const auto& DL = TheModule->getDataLayout();
            uint64_t sz = DL.getTypeAllocSize(typeST);
            if (sz == 0) sz = 1;
            FunctionCallee mallocFn = TheModule->getOrInsertFunction("GC_malloc",
                FunctionType::get(Type::getInt8PtrTy(Context), {Type::getInt64Ty(Context)}, false));
            Value* raw    = Builder.CreateCall(mallocFn, {ConstantInt::get(Type::getInt64Ty(Context), sz)});
            Value* typePtr = Builder.CreateBitCast(raw, PointerType::getUnqual(typeST));

            auto makeStr = [&](const std::string& s) {
                return structGen->allocateAndInit("String", std::vector<Value*>{Builder.CreateGlobalStringPtr(s)});
            };
            Builder.CreateStore(makeStr(currentCodegenClass), Builder.CreateStructGEP(typeST, typePtr, 0));
            Builder.CreateStore(makeStr(parentName),          Builder.CreateStructGEP(typeST, typePtr, 1));
            return typePtr;
        }

        Value* objPtr = handleExpression(member->object.get());

        // Handle double pointer dereference
        if (objPtr && objPtr->getType()->isPointerTy() && 
            objPtr->getType()->getPointerElementType()->isPointerTy()) {
            objPtr = Builder.CreateLoad(objPtr->getType()->getPointerElementType(), objPtr);
        }

        // --- FIX 2: Enable Member Access on Any (Assume String) ---
        if (objPtr && objPtr->getType()->isPointerTy() && 
            objPtr->getType()->getPointerElementType()->isIntegerTy(8)) {
            // It's Any (i8*). Cast it to String* so we can find members like .to_int()
            if (StructTypes.count("String")) {
                objPtr = Builder.CreateBitCast(objPtr, PointerType::getUnqual(StructTypes["String"]));
            }
        }
        // ----------------------------------------------------------
        
        return structGen->generateMemberAccess(objPtr, member->memberName);
    }

    if (auto c = dynamic_cast<ConstructorNode*>(node)) return handleConstructor(c);

    return nullptr;
}
std::string LLVMCodegen::currentCodegenClass = "";