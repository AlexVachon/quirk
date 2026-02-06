#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>
#include <iostream>

#include "VariableGen.cpp"

using namespace llvm;

class ControlFlowGen {
    LLVMContext& Context;
    Module* TheModule;  // Added Module
    IRBuilder<>& Builder;

   public:
    // Update Constructor
    ControlFlowGen(LLVMContext& ctx, Module* mod, IRBuilder<>& build)
        : Context(ctx), TheModule(mod), Builder(build) {}

    void generateIf(IfNode* node,
                    Function* parentFunc,
                    std::function<Value*(Node*)> exprHandler,
                    std::function<void(Node*)> stmtHandler) {
        BasicBlock* thenBB = BasicBlock::Create(Context, "then", parentFunc);
        BasicBlock* mergeBB = BasicBlock::Create(Context, "ifcont");
        BasicBlock* nextBB = node->elIfBranches.empty()
                                 ? (node->elseBranch.empty()
                                        ? mergeBB
                                        : BasicBlock::Create(Context, "else"))
                                 : BasicBlock::Create(Context, "elif_test");
        Builder.CreateCondBr(exprHandler(node->condition.get()), thenBB,
                             nextBB);
        Builder.SetInsertPoint(thenBB);
        for (auto& stmt : node->thenBranch)
            stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator())
            Builder.CreateBr(mergeBB);
        for (size_t i = 0; i < node->elIfBranches.size(); ++i) {
            parentFunc->getBasicBlockList().push_back(nextBB);
            Builder.SetInsertPoint(nextBB);
            Value* eiCond = exprHandler(node->elIfBranches[i].condition.get());
            BasicBlock* eiBody =
                BasicBlock::Create(Context, "elif_body", parentFunc);
            nextBB = (i + 1 < node->elIfBranches.size())
                         ? BasicBlock::Create(Context, "elif_test")
                         : (node->elseBranch.empty()
                                ? mergeBB
                                : BasicBlock::Create(Context, "else"));
            Builder.CreateCondBr(eiCond, eiBody, nextBB);
            Builder.SetInsertPoint(eiBody);
            for (auto& stmt : node->elIfBranches[i].body)
                stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateBr(mergeBB);
        }
        if (!node->elseBranch.empty()) {
            parentFunc->getBasicBlockList().push_back(nextBB);
            Builder.SetInsertPoint(nextBB);
            for (auto& stmt : node->elseBranch)
                stmtHandler(stmt.get());
            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateBr(mergeBB);
        }
        parentFunc->getBasicBlockList().push_back(mergeBB);
        Builder.SetInsertPoint(mergeBB);
    }

    void generateWhile(WhileNode* node,
                       Function* parentFunc,
                       std::function<Value*(Node*)> exprHandler,
                       std::function<void(Node*)> stmtHandler) {
        BasicBlock* condBB =
            BasicBlock::Create(Context, "loopcond", parentFunc);
        BasicBlock* bodyBB =
            BasicBlock::Create(Context, "loopbody", parentFunc);
        BasicBlock* afterBB =
            BasicBlock::Create(Context, "afterloop", parentFunc);
        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(condBB);
        Builder.CreateCondBr(exprHandler(node->condition.get()), bodyBB,
                             afterBB);
        Builder.SetInsertPoint(bodyBB);
        for (auto& stmt : node->body)
            stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator())
            Builder.CreateBr(condBB);
        Builder.SetInsertPoint(afterBB);
    }

    void generateFor(ForNode* node,
                     Function* parentFunc,
                     std::function<Value*(Node*)> exprHandler,
                     std::function<Value*(const std::string&,
                                          std::vector<Value*>&)> initHelper,
                     std::function<void(Node*)> stmtHandler,
                     VariableGen* varGen) {
        // 1. Evaluate the iterable expression (e.g., variable 'f')
        Value* iterable = exprHandler(node->iterable.get());
        if (!iterable)
            return;

        // --- [CRITICAL FIX START] ---
        // Auto-Dereference: If the iterable is a pointer-to-pointer (e.g., a
        // local variable allocated on the stack like %File**), we must load it
        // to get the actual object pointer (%File*) expected by the iterator
        // functions.
        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isPointerTy()) {
            iterable = Builder.CreateLoad(iterable, "iterable_load");
        }
        // --- [CRITICAL FIX END] ---

        // 2. GENERIC STRUCT ITERATOR (via __iter)
        // Now this check will correctly identify 'File*' as a Struct Pointer
        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isStructTy()) {
            StructType* st =
                cast<StructType>(iterable->getType()->getPointerElementType());
            std::string structName = st->getName().str();

            // A. Call __iter() -> returns Iterator Struct
            // Try triple underscore first (standard mangling: Struct + "_" +
            // "__iter")
            Function* iterFunc = TheModule->getFunction(structName + "___iter");

            // Fallback: try double underscore
            if (!iterFunc)
                iterFunc = TheModule->getFunction(structName + "__iter");

            if (!iterFunc) {
                std::cerr << "Error: Struct '" << structName
                          << "' does not implement __iter()." << std::endl;
                exit(1);
            }

            // This call now receives the correct %File* (not %File**)
            Value* iteratorObj =
                Builder.CreateCall(iterFunc, {iterable}, "iter");

            // B. Get iterator type info
            StructType* iterStructType = cast<StructType>(
                iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB =
                BasicBlock::Create(Context, "forcond", parentFunc);
            BasicBlock* bodyBB =
                BasicBlock::Create(Context, "forbody", parentFunc);
            BasicBlock* afterBB =
                BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            // C. Call has_next()
            Function* hasNextFunc =
                TheModule->getFunction(iterName + "___has_next");
            if (!hasNextFunc)
                hasNextFunc = TheModule->getFunction(iterName + "__has_next");

            if (!hasNextFunc) {
                std::cerr << "Error: Iterator '" << iterName
                          << "' missing __has_next() or _has_next()."
                          << std::endl;
                exit(1);
            }
            Value* hasNext =
                Builder.CreateCall(hasNextFunc, {iteratorObj}, "has_next");
            Builder.CreateCondBr(hasNext, bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);

            // D. Call next()
            Function* nextFunc = TheModule->getFunction(iterName + "___next");
            if (!nextFunc)
                nextFunc = TheModule->getFunction(iterName + "__next");

            if (!nextFunc) {
                std::cerr << "Error: Iterator '" << iterName
                          << "' missing __next() or _next()." << std::endl;
                exit(1);
            }
            Value* item = Builder.CreateCall(nextFunc, {iteratorObj}, "item");

            varGen->defineLocalVariable(node->varName, item);

            for (auto& stmt : node->body)
                stmtHandler(stmt.get());

            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateBr(condBB);

            Builder.SetInsertPoint(afterBB);
            return;
        }

        // 3. FALLBACK: RAW STRING ITERATION (Auto-box to String struct)
        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isIntegerTy(8)) {
            // Auto-box: raw string -> String struct
            std::vector<Value*> args = {iterable};
            Value* stringObj = initHelper("String", args);

            // Replicate Iterator Logic for String
            Function* iterFunc = TheModule->getFunction("String___iter");
            if (!iterFunc)
                iterFunc = TheModule->getFunction("String__iter");

            if (!iterFunc) {
                std::cerr << "Error: String struct missing __iter."
                          << std::endl;
                exit(1);
            }

            Value* iteratorObj =
                Builder.CreateCall(iterFunc, {stringObj}, "iter");

            StructType* iterStructType = cast<StructType>(
                iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB =
                BasicBlock::Create(Context, "forcond", parentFunc);
            BasicBlock* bodyBB =
                BasicBlock::Create(Context, "forbody", parentFunc);
            BasicBlock* afterBB =
                BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            Function* hasNextFunc =
                TheModule->getFunction(iterName + "___has_next");
            if (!hasNextFunc)
                hasNextFunc = TheModule->getFunction(iterName + "__has_next");

            Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj});
            Builder.CreateCondBr(hasNext, bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);
            Function* nextFunc = TheModule->getFunction(iterName + "___next");
            if (!nextFunc)
                nextFunc = TheModule->getFunction(iterName + "__next");

            Value* item = Builder.CreateCall(nextFunc, {iteratorObj});

            varGen->defineLocalVariable(node->varName, item);

            for (auto& stmt : node->body)
                stmtHandler(stmt.get());

            if (!Builder.GetInsertBlock()->getTerminator())
                Builder.CreateBr(condBB);
            Builder.SetInsertPoint(afterBB);
            return;
        }

        std::cerr << "Error: Object is not iterable (must implement __iter or "
                  << "be a raw string)." << std::endl;
        exit(1);
    }
};