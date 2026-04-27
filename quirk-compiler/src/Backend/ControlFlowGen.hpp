#pragma once
#include <functional>
#include <iostream>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"
#include "VariableGen.hpp"

using namespace llvm;

class ControlFlowGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;

   public:
    std::vector<BasicBlock*> breakStack;
    std::vector<BasicBlock*> continueStack;

    ControlFlowGen(LLVMContext& ctx, Module* mod, IRBuilder<>& build)
        : Context(ctx), TheModule(mod), Builder(build) {}

    Value* toBool(Value* v) {
        if (!v) return ConstantInt::getFalse(Context);
        if (v->getType()->isIntegerTy(1)) return v;
        if (v->getType()->isPointerTy()) {
            // Prefer __bool magic method on structs
            Type* elTy = v->getType()->getPointerElementType();
            if (elTy->isStructTy()) {
                std::string sName = cast<StructType>(elTy)->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                Function* boolFunc = TheModule->getFunction(sName + "___bool");
                if (!boolFunc) {
                    std::string suffix = sName + "___bool";
                    for (auto& F : *TheModule)
                        if (F.getName().endswith(suffix)) { boolFunc = &F; break; }
                }
                if (boolFunc) return Builder.CreateCall(boolFunc, {v}, "obj_bool");
            }
            return Builder.CreateICmpNE(v, Constant::getNullValue(v->getType()), "ptr_bool");
        }
        if (v->getType()->isDoubleTy())
            return Builder.CreateFCmpONE(v, ConstantFP::get(Context, APFloat(0.0)), "dbl_bool");
        if (v->getType()->isIntegerTy())
            return Builder.CreateICmpNE(v, ConstantInt::get(v->getType(), 0), "int_bool");
        return ConstantInt::getFalse(Context);
    }

    void generateIf(IfNode* node, Function* parentFunc,
                    std::function<Value*(Node*)> exprHandler,
                    std::function<void(Node*)> stmtHandler) {
        BasicBlock* thenBB  = BasicBlock::Create(Context, "then", parentFunc);
        BasicBlock* mergeBB = BasicBlock::Create(Context, "ifcont");
        BasicBlock* nextBB  = node->elIfBranches.empty()
                                  ? (node->elseBranch.empty() ? mergeBB : BasicBlock::Create(Context, "else"))
                                  : BasicBlock::Create(Context, "elif_test");
        Builder.CreateCondBr(toBool(exprHandler(node->condition.get())), thenBB, nextBB);
        Builder.SetInsertPoint(thenBB);
        for (auto& stmt : node->thenBranch) stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(mergeBB);

        for (size_t i = 0; i < node->elIfBranches.size(); ++i) {
            parentFunc->getBasicBlockList().push_back(nextBB);
            Builder.SetInsertPoint(nextBB);
            Value* eiCond = toBool(exprHandler(node->elIfBranches[i].condition.get()));
            BasicBlock* eiBody = BasicBlock::Create(Context, "elif_body", parentFunc);
            nextBB = (i + 1 < node->elIfBranches.size())
                         ? BasicBlock::Create(Context, "elif_test")
                         : (node->elseBranch.empty() ? mergeBB : BasicBlock::Create(Context, "else"));
            Builder.CreateCondBr(eiCond, eiBody, nextBB);
            Builder.SetInsertPoint(eiBody);
            for (auto& stmt : node->elIfBranches[i].body) stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(mergeBB);
        }
        if (!node->elseBranch.empty()) {
            parentFunc->getBasicBlockList().push_back(nextBB);
            Builder.SetInsertPoint(nextBB);
            for (auto& stmt : node->elseBranch) stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(mergeBB);
        }
        parentFunc->getBasicBlockList().push_back(mergeBB);
        Builder.SetInsertPoint(mergeBB);
    }

    void generateWhile(WhileNode* node, Function* parentFunc,
                       std::function<Value*(Node*)> exprHandler,
                       std::function<void(Node*)> stmtHandler) {
        BasicBlock* condBB  = BasicBlock::Create(Context, "loopcond",  parentFunc);
        BasicBlock* bodyBB  = BasicBlock::Create(Context, "loopbody",  parentFunc);
        BasicBlock* afterBB = BasicBlock::Create(Context, "afterloop", parentFunc);

        breakStack.push_back(afterBB);
        continueStack.push_back(condBB);

        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(condBB);
        Builder.CreateCondBr(toBool(exprHandler(node->condition.get())), bodyBB, afterBB);
        Builder.SetInsertPoint(bodyBB);
        for (auto& stmt : node->body) stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);

        breakStack.pop_back();
        continueStack.pop_back();

        Builder.SetInsertPoint(afterBB);
    }

    void generateFor(ForNode* node, Function* parentFunc,
                     std::function<Value*(Node*)> exprHandler,
                     std::function<Value*(const std::string&, std::vector<Value*>&)> initHelper,
                     std::function<void(Node*)> stmtHandler,
                     VariableGen* varGen,
                     std::function<Function*(const std::string&)> funcResolver = nullptr) {
        auto resolveFunc = [&](const std::string& name) -> Function* {
            Function* f = TheModule->getFunction(name);
            if (!f && funcResolver) f = funcResolver(name);
            return f;
        };

        Value* iterable = exprHandler(node->iterable.get());
        if (!iterable) return;

        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isPointerTy())
            iterable = Builder.CreateLoad(iterable->getType()->getPointerElementType(), iterable, "iterable_load");

        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isStructTy()) {
            StructType* st = cast<StructType>(iterable->getType()->getPointerElementType());
            std::string structName = st->getName().str();

            Function* iterFunc = resolveFunc(structName + "___iter");
            if (!iterFunc) iterFunc = resolveFunc(structName + "__iter");
            if (!iterFunc) { std::cerr << "Error: Struct '" << structName << "' does not implement __iter()." << std::endl; exit(1); }

            Value* iteratorObj = Builder.CreateCall(iterFunc, {iterable}, "iter");
            StructType* iterStructType = cast<StructType>(iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB  = BasicBlock::Create(Context, "forcond",  parentFunc);
            BasicBlock* bodyBB  = BasicBlock::Create(Context, "forbody",  parentFunc);
            BasicBlock* afterBB = BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            Function* hasNextFunc = resolveFunc(iterName + "___has_next");
            if (!hasNextFunc) hasNextFunc = resolveFunc(iterName + "__has_next");
            if (!hasNextFunc) { std::cerr << "Error: Iterator '" << iterName << "' missing __has_next()." << std::endl; exit(1); }

            Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj}, "has_next");
            Builder.CreateCondBr(toBool(hasNext), bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);
            Function* nextFunc = resolveFunc(iterName + "___next");
            if (!nextFunc) nextFunc = resolveFunc(iterName + "__next");
            if (!nextFunc) { std::cerr << "Error: Iterator '" << iterName << "' missing __next()." << std::endl; exit(1); }

            Value* item = Builder.CreateCall(nextFunc, {iteratorObj}, "item");
            varGen->defineLocalVariable(node->varName, item);

            breakStack.push_back(afterBB);
            continueStack.push_back(condBB);
            for (auto& stmt : node->body) stmtHandler(stmt.get());
            breakStack.pop_back();
            continueStack.pop_back();

            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);
            Builder.SetInsertPoint(afterBB);
            return;
        }

        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isIntegerTy(8)) {
            std::vector<Value*> args = {iterable};
            Value* stringObj = initHelper("String", args);

            Function* iterFunc = resolveFunc("String___iter");
            if (!iterFunc) iterFunc = resolveFunc("String__iter");
            if (!iterFunc) { std::cerr << "Error: String struct missing __iter." << std::endl; exit(1); }

            Value* iteratorObj = Builder.CreateCall(iterFunc, {stringObj}, "iter");
            StructType* iterStructType = cast<StructType>(iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB  = BasicBlock::Create(Context, "forcond",  parentFunc);
            BasicBlock* bodyBB  = BasicBlock::Create(Context, "forbody",  parentFunc);
            BasicBlock* afterBB = BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            Function* hasNextFunc = resolveFunc(iterName + "___has_next");
            if (!hasNextFunc) hasNextFunc = resolveFunc(iterName + "__has_next");

            Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj});
            Builder.CreateCondBr(toBool(hasNext), bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);
            Function* nextFunc = resolveFunc(iterName + "___next");
            if (!nextFunc) nextFunc = resolveFunc(iterName + "__next");

            Value* item = Builder.CreateCall(nextFunc, {iteratorObj});
            varGen->defineLocalVariable(node->varName, item);

            breakStack.push_back(afterBB);
            continueStack.push_back(condBB);
            for (auto& stmt : node->body) stmtHandler(stmt.get());
            breakStack.pop_back();
            continueStack.pop_back();

            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);
            Builder.SetInsertPoint(afterBB);
            return;
        }

        std::cerr << "Error: Object is not iterable." << std::endl;
        exit(1);
    }

    // Emit print_traceback on the active exception then terminate.
    // Called at every "unhandled exception" site instead of the bare runtime stub.
    void emitUnhandledException(std::map<std::string, StructType*>& StructTypes) {
        Value* rawExc = Builder.CreateCall(TheModule->getFunction("quirk_get_exception"));
        Function* printFn = TheModule->getFunction("Exception_print_traceback");
        if (printFn && StructTypes.count("Exception")) {
            Type* selfTy = printFn->getFunctionType()->getParamType(0);
            Value* casted = Builder.CreateBitCast(rawExc, selfTy);
            Builder.CreateCall(printFn, {casted});
        }
        Builder.CreateCall(TheModule->getFunction("quirk_unhandled_exception"));
        Builder.CreateUnreachable();
    }

    void generateTryCatch(TryCatchNode* node, Function* parentFunc,
                          std::function<void(Node*)> stmtHandler,
                          VariableGen* varGen,
                          std::map<std::string, StructType*>& StructTypes,
                          std::map<std::string, std::vector<std::string>>& structHierarchy) {
        BasicBlock* tryBB = BasicBlock::Create(Context, "try_block", parentFunc);
        BasicBlock* endBB = BasicBlock::Create(Context, "end_try",   parentFunc);

        Value* jmpBuf   = Builder.CreateCall(TheModule->getFunction("quirk_get_jmp_buf"));
        Value* setjmpRes = Builder.CreateCall(TheModule->getFunction("_setjmp"), {jmpBuf});
        Value* isCatch  = Builder.CreateICmpNE(setjmpRes, ConstantInt::get(Type::getInt32Ty(Context), 0));

        BasicBlock* firstEvalBB = BasicBlock::Create(Context, "catch_eval_0", parentFunc);
        Builder.CreateCondBr(isCatch, firstEvalBB, tryBB);

        // Helper: emit finally statements inline at any exit point.
        auto emitFinally = [&]() {
            for (auto& stmt : node->finallyBlock) stmtHandler(stmt.get());
        };

        Builder.SetInsertPoint(tryBB);
        for (auto& stmt : node->tryBlock) stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator()) {
            Builder.CreateCall(TheModule->getFunction("quirk_pop_try"));
            emitFinally();
            Builder.CreateBr(endBB);
        }

        BasicBlock* currentEvalBB = firstEvalBB;

        for (size_t i = 0; i < node->catchBlocks.size(); ++i) {
            auto& cb = node->catchBlocks[i];
            Builder.SetInsertPoint(currentEvalBB);

            Value* rawExc = Builder.CreateCall(TheModule->getFunction("quirk_get_exception"));
            Type* baseExcType  = PointerType::getUnqual(StructTypes["Exception"]);
            Value* baseExc     = Builder.CreateBitCast(rawExc, baseExcType);
            Value* typeFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 0);
            Value* typeStringObj = Builder.CreateLoad(PointerType::getUnqual(StructTypes["String"]), typeFieldPtr);
            Value* bufferPtr    = Builder.CreateStructGEP(StructTypes["String"], typeStringObj, 0);
            Value* runtimeTypeCStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufferPtr);

            FunctionCallee strcmpFunc = TheModule->getOrInsertFunction(
                "strcmp", FunctionType::get(Type::getInt32Ty(Context),
                                            {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)}, false));

            std::vector<std::string> allValidTypes;
            for (const std::string& targetTypeName : cb.types) {
                allValidTypes.push_back(targetTypeName);
                for (const auto& pair : structHierarchy) {
                    std::function<bool(const std::string&)> inheritsFrom = [&](const std::string& t) {
                        if (t == targetTypeName) return true;
                        if (structHierarchy.count(t))
                            for (const auto& p : structHierarchy.at(t))
                                if (inheritsFrom(p)) return true;
                        return false;
                    };
                    if (inheritsFrom(pair.first) && pair.first != targetTypeName)
                        allValidTypes.push_back(pair.first);
                }
            }

            Value* isMatch = ConstantInt::getFalse(Context);
            for (const std::string& vType : allValidTypes) {
                Value* targetTypeStr = Builder.CreateGlobalStringPtr(vType);
                Value* cmpRes = Builder.CreateCall(strcmpFunc, {runtimeTypeCStr, targetTypeStr});
                Value* isZero = Builder.CreateICmpEQ(cmpRes, ConstantInt::get(Type::getInt32Ty(Context), 0));
                isMatch = Builder.CreateOr(isMatch, isZero);
            }

            BasicBlock* matchBodyBB = BasicBlock::Create(Context, "catch_match_" + std::to_string(i), parentFunc);
            BasicBlock* nextEvalBB  = (i + 1 < node->catchBlocks.size())
                                          ? BasicBlock::Create(Context, "catch_eval_" + std::to_string(i + 1), parentFunc)
                                          : BasicBlock::Create(Context, "catch_rethrow", parentFunc);

            Builder.CreateCondBr(isMatch, matchBodyBB, nextEvalBB);
            Builder.SetInsertPoint(matchBodyBB);

            Type* catchTypeLLVM = PointerType::getUnqual(StructTypes[cb.types[0]]);
            Value* castedExc = Builder.CreateBitCast(rawExc, catchTypeLLVM);
            if (!cb.varName.empty()) varGen->defineLocalVariable(cb.varName, castedExc);

            for (auto& stmt : cb.body) stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator()) {
                emitFinally();
                Builder.CreateBr(endBB);
            }
            currentEvalBB = nextEvalBB;
        }

        Builder.SetInsertPoint(currentEvalBB);
        emitFinally();
        Value* depth = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
        Value* hasParentCatch = Builder.CreateICmpSGT(depth, ConstantInt::get(Type::getInt32Ty(Context), -1));

        BasicBlock* jumpBB  = BasicBlock::Create(Context, "rethrow_jump",  parentFunc);
        BasicBlock* crashBB = BasicBlock::Create(Context, "rethrow_crash", parentFunc);
        Builder.CreateCondBr(hasParentCatch, jumpBB, crashBB);

        Builder.SetInsertPoint(jumpBB);
        Value* parentBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
        Builder.CreateCall(TheModule->getFunction("longjmp"),
                           {parentBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(crashBB);
        emitUnhandledException(StructTypes);

        Builder.SetInsertPoint(endBB);
    }

    void generateThrow(ThrowNode* node, Function* parentFunc,
                       std::function<Value*(Node*)> exprHandler,
                       std::map<std::string, StructType*>& StructTypes,
                       std::function<Value*(const std::string&, std::vector<Value*>&)> initHelper) {
        // Bare throw: re-raise the currently active exception unchanged.
        if (!node->expression) {
            Value* depth    = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
            Value* hasCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));

            BasicBlock* jumpBB  = BasicBlock::Create(Context, "rethrow_jump",  parentFunc);
            BasicBlock* crashBB = BasicBlock::Create(Context, "rethrow_crash", parentFunc);
            Builder.CreateCondBr(hasCatch, jumpBB, crashBB);

            Builder.SetInsertPoint(jumpBB);
            Value* activeBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
            Builder.CreateCall(TheModule->getFunction("longjmp"),
                               {activeBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
            Builder.CreateUnreachable();

            Builder.SetInsertPoint(crashBB);
            emitUnhandledException(StructTypes);
            return;
        }

        Value* excObj = exprHandler(node->expression.get());

        if (StructTypes.count("Exception")) {
            Type*  baseExcType = PointerType::getUnqual(StructTypes["Exception"]);
            Value* baseExc     = Builder.CreateBitCast(excObj, baseExcType);

            Value* fileFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 2);
            std::string currentModule = node->moduleName.empty() ? "unknown_file" : node->moduleName;
            Value* rawFileName = Builder.CreateGlobalStringPtr(currentModule);
            std::vector<Value*> fileArgs = {rawFileName};
            Builder.CreateStore(initHelper("String", fileArgs), fileFieldPtr);

            Value* lineFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 3);
            Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), node->line), lineFieldPtr);

            Value* calleeFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 4);
            Value* rawCalleeName  = Builder.CreateGlobalStringPtr(parentFunc->getName().str());
            std::vector<Value*> calleeArgs = {rawCalleeName};
            Builder.CreateStore(initHelper("String", calleeArgs), calleeFieldPtr);
        }

        Value* rawExc = Builder.CreateBitCast(excObj, Type::getInt8PtrTy(Context));

        // Capture the shadow-stack traceback at throw time (Python-style)
        FunctionCallee captureFn = TheModule->getOrInsertFunction(
            "quirk_capture_traceback",
            FunctionType::get(Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false));
        Builder.CreateCall(captureFn, {rawExc});

        Builder.CreateCall(TheModule->getFunction("quirk_set_exception"), {rawExc});

        if (node->cause) {
            Value* causeObj = exprHandler(node->cause.get());
            Function* withCauseFunc = TheModule->getFunction("Exception_with_cause");
            if (withCauseFunc) {
                Type* expectedSelfTy  = withCauseFunc->getFunctionType()->getParamType(0);
                Type* expectedCauseTy = withCauseFunc->getFunctionType()->getParamType(1);
                if (excObj->getType() != expectedSelfTy)   excObj   = Builder.CreateBitCast(excObj,   expectedSelfTy);
                if (causeObj->getType() != expectedCauseTy) causeObj = Builder.CreateBitCast(causeObj, expectedCauseTy);
                excObj = Builder.CreateCall(withCauseFunc, {excObj, causeObj});
            }
        }

        Value* depth    = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
        Value* hasCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));

        BasicBlock* jumpBB  = BasicBlock::Create(Context, "do_longjmp", parentFunc);
        BasicBlock* crashBB = BasicBlock::Create(Context, "do_crash",   parentFunc);
        Builder.CreateCondBr(hasCatch, jumpBB, crashBB);

        Builder.SetInsertPoint(jumpBB);
        Value* activeBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
        Builder.CreateCall(TheModule->getFunction("quirk_pop_try"));
        Builder.CreateCall(TheModule->getFunction("longjmp"),
                           {activeBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(crashBB);
        emitUnhandledException(StructTypes);
    }

    void generateMatch(MatchNode* node, Function* parentFunc,
                       std::function<Value*(Node*)> exprHandler,
                       std::function<void(Node*)> stmtHandler) {
        Value* scrutVal = exprHandler(node->scrutinee.get());
        if (!scrutVal) return;

        BasicBlock* mergeBB = BasicBlock::Create(Context, "match_cont");

        for (size_t i = 0; i < node->arms.size(); ++i) {
            auto& arm = node->arms[i];
            BasicBlock* bodyBB = BasicBlock::Create(Context, "case_body", parentFunc);
            BasicBlock* nextBB = (i + 1 < node->arms.size())
                                 ? BasicBlock::Create(Context, "case_next")
                                 : mergeBB;

            if (arm.isWildcard) {
                Builder.CreateBr(bodyBB);
            } else {
                // OR together equality checks for all patterns in this arm
                Value* cond = nullptr;
                for (auto& patNode : arm.patterns) {
                    Value* patVal = exprHandler(patNode.get());
                    Value* eq = toBool(emitMatchEq(scrutVal, patVal));
                    cond = cond ? Builder.CreateOr(cond, eq, "match_or") : eq;
                }
                if (!cond) cond = ConstantInt::getFalse(Context);
                Builder.CreateCondBr(cond, bodyBB, nextBB);
            }

            Builder.SetInsertPoint(bodyBB);
            for (auto& stmt : arm.body) stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateBr(mergeBB);

            if (nextBB != mergeBB) {
                parentFunc->getBasicBlockList().push_back(nextBB);
                Builder.SetInsertPoint(nextBB);
            }
        }

        parentFunc->getBasicBlockList().push_back(mergeBB);
        Builder.SetInsertPoint(mergeBB);
    }

private:
    // Emit scrutinee == pattern, handling int, double, bool, String*, and enum (i32)
    Value* emitMatchEq(Value* L, Value* R) {
        if (!L || !R) return ConstantInt::getFalse(Context);

        Type* lt = L->getType();
        Type* rt = R->getType();

        // Coerce int widths so ICmpEQ doesn't blow up
        if (lt->isIntegerTy() && rt->isIntegerTy()) {
            if (lt->getIntegerBitWidth() != rt->getIntegerBitWidth()) {
                unsigned w = std::max(lt->getIntegerBitWidth(), rt->getIntegerBitWidth());
                L = Builder.CreateIntCast(L, Type::getIntNTy(Context, w), true, "eq_cast");
                R = Builder.CreateIntCast(R, Type::getIntNTy(Context, w), true, "eq_cast");
            }
            return Builder.CreateICmpEQ(L, R, "match_eq");
        }

        if (lt->isDoubleTy() && rt->isDoubleTy())
            return Builder.CreateFCmpOEQ(L, R, "match_eq");

        // Struct pointer: call StructName___eq magic method
        if (lt->isPointerTy() && lt->getPointerElementType()->isStructTy()) {
            std::string sName = cast<StructType>(lt->getPointerElementType())->getName().str();
            if (sName.find("struct.") == 0) sName = sName.substr(7);
            Function* eqFn = TheModule->getFunction(sName + "___eq");
            if (!eqFn) {
                std::string suffix = "___eq";
                for (auto& F : *TheModule)
                    if (F.getName().endswith(suffix)) { eqFn = &F; break; }
            }
            if (eqFn) {
                // Ensure R has the same type as L (both String* for example)
                if (R->getType() != L->getType())
                    R = Builder.CreateBitCast(R, L->getType(), "eq_cast");
                return Builder.CreateCall(eqFn, {L, R}, "match_eq");
            }
        }

        // Fallback: raw pointer / opaque comparison
        if (lt->isPointerTy() && rt->isPointerTy()) {
            if (lt != rt) R = Builder.CreateBitCast(R, lt, "eq_cast");
            return Builder.CreateICmpEQ(L, R, "match_eq");
        }

        return ConstantInt::getFalse(Context);
    }
};
