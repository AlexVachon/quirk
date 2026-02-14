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
            iterable = Builder.CreateLoad(iterable->getType()->getPointerElementType(), iterable, "iterable_load");
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

    void generateTryCatch(TryCatchNode* node, Function* parentFunc,
                          std::function<void(Node*)> stmtHandler,
                          VariableGen* varGen,
                          std::map<std::string, StructType*>& StructTypes) {
        
        BasicBlock* tryBB = BasicBlock::Create(Context, "try_block", parentFunc);
        BasicBlock* catchBB = BasicBlock::Create(Context, "catch_block", parentFunc);
        BasicBlock* endBB = BasicBlock::Create(Context, "end_try", parentFunc);

        // 1. Get Jump Buffer and call setjmp
        Value* jmpBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_jmp_buf"));
        Value* setjmpRes = Builder.CreateCall(TheModule->getFunction("setjmp"), {jmpBuf});

        // 2. If setjmp returns 0, we are starting the try block. If != 0, an exception occurred.
        Value* isCatch = Builder.CreateICmpNE(setjmpRes, ConstantInt::get(Type::getInt32Ty(Context), 0));
        Builder.CreateCondBr(isCatch, catchBB, tryBB);

        // --- TRY BLOCK ---
        Builder.SetInsertPoint(tryBB);
        for (auto& stmt : node->tryBlock) stmtHandler(stmt.get());
        
        if (!Builder.GetInsertBlock()->getTerminator()) {
            Builder.CreateCall(TheModule->getFunction("quirk_pop_try")); // Success! Pop buffer
            Builder.CreateBr(endBB);
        }

        // --- CATCH BLOCK ---
        Builder.SetInsertPoint(catchBB);
        Value* rawExc = Builder.CreateCall(TheModule->getFunction("quirk_get_exception"));
        
        // Cast raw i8* to the expected Exception Struct pointer
        Type* catchType = PointerType::getUnqual(StructTypes[node->catchType]);
        Value* castedExc = Builder.CreateBitCast(rawExc, catchType);
        
        varGen->defineLocalVariable(node->catchVar, castedExc);

        for (auto& stmt : node->catchBlock) stmtHandler(stmt.get());
        
        if (!Builder.GetInsertBlock()->getTerminator()) {
            Builder.CreateBr(endBB);
        }

        Builder.SetInsertPoint(endBB);
    }

    void generateThrow(ThrowNode* node, Function* parentFunc, std::function<Value*(Node*)> exprHandler) {
        Value* excObj = exprHandler(node->expression.get());
        Value* rawExc = Builder.CreateBitCast(excObj, Type::getInt8PtrTy(Context));
        
        // 1. Store the exception globally
        Builder.CreateCall(TheModule->getFunction("quirk_set_exception"), {rawExc});

        // 2. Check if we are inside a try block
        Value* depth = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
        Value* hasCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));

        BasicBlock* jumpBB = BasicBlock::Create(Context, "do_longjmp", parentFunc);
        BasicBlock* crashBB = BasicBlock::Create(Context, "do_crash", parentFunc);
        Builder.CreateCondBr(hasCatch, jumpBB, crashBB);

        // 3. Jump to nearest catch
        Builder.SetInsertPoint(jumpBB);
        Value* activeBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
        Builder.CreateCall(TheModule->getFunction("longjmp"), {activeBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        // 4. Crash if unhandled
        Builder.SetInsertPoint(crashBB);
        Builder.CreateCall(TheModule->getFunction("quirk_unhandled_exception"));
        Builder.CreateUnreachable();
    }
};