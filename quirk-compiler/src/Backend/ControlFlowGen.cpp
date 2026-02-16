#include "llvm/IR/IRBuilder.h"
#include "ast.hpp"
#include <functional>
#include <iostream>

#include "VariableGen.cpp"

using namespace llvm;

class ControlFlowGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;

   public:
    ControlFlowGen(LLVMContext& ctx, Module* mod, IRBuilder<>& build)
        : Context(ctx), TheModule(mod), Builder(build) {}

    void generateIf(IfNode* node, Function* parentFunc, std::function<Value*(Node*)> exprHandler, std::function<void(Node*)> stmtHandler) {
        BasicBlock* thenBB = BasicBlock::Create(Context, "then", parentFunc);
        BasicBlock* mergeBB = BasicBlock::Create(Context, "ifcont");
        BasicBlock* nextBB = node->elIfBranches.empty()
                                 ? (node->elseBranch.empty() ? mergeBB : BasicBlock::Create(Context, "else"))
                                 : BasicBlock::Create(Context, "elif_test");
        Builder.CreateCondBr(exprHandler(node->condition.get()), thenBB, nextBB);
        Builder.SetInsertPoint(thenBB);
        for (auto& stmt : node->thenBranch) stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(mergeBB);
        
        for (size_t i = 0; i < node->elIfBranches.size(); ++i) {
            parentFunc->getBasicBlockList().push_back(nextBB);
            Builder.SetInsertPoint(nextBB);
            Value* eiCond = exprHandler(node->elIfBranches[i].condition.get());
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

    void generateWhile(WhileNode* node, Function* parentFunc, std::function<Value*(Node*)> exprHandler, std::function<void(Node*)> stmtHandler) {
        BasicBlock* condBB = BasicBlock::Create(Context, "loopcond", parentFunc);
        BasicBlock* bodyBB = BasicBlock::Create(Context, "loopbody", parentFunc);
        BasicBlock* afterBB = BasicBlock::Create(Context, "afterloop", parentFunc);
        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(condBB);
        Builder.CreateCondBr(exprHandler(node->condition.get()), bodyBB, afterBB);
        Builder.SetInsertPoint(bodyBB);
        for (auto& stmt : node->body) stmtHandler(stmt.get());
        if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);
        Builder.SetInsertPoint(afterBB);
    }

    void generateFor(ForNode* node, Function* parentFunc, std::function<Value*(Node*)> exprHandler, std::function<Value*(const std::string&, std::vector<Value*>&)> initHelper, std::function<void(Node*)> stmtHandler, VariableGen* varGen) {
        Value* iterable = exprHandler(node->iterable.get());
        if (!iterable) return;

        if (iterable->getType()->isPointerTy() && iterable->getType()->getPointerElementType()->isPointerTy()) {
            iterable = Builder.CreateLoad(iterable->getType()->getPointerElementType(), iterable, "iterable_load");
        }

        if (iterable->getType()->isPointerTy() && iterable->getType()->getPointerElementType()->isStructTy()) {
            StructType* st = cast<StructType>(iterable->getType()->getPointerElementType());
            std::string structName = st->getName().str();

            Function* iterFunc = TheModule->getFunction(structName + "___iter");
            if (!iterFunc) iterFunc = TheModule->getFunction(structName + "__iter");

            if (!iterFunc) {
                std::cerr << "Error: Struct '" << structName << "' does not implement __iter()." << std::endl;
                exit(1);
            }

            Value* iteratorObj = Builder.CreateCall(iterFunc, {iterable}, "iter");
            StructType* iterStructType = cast<StructType>(iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB = BasicBlock::Create(Context, "forcond", parentFunc);
            BasicBlock* bodyBB = BasicBlock::Create(Context, "forbody", parentFunc);
            BasicBlock* afterBB = BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            Function* hasNextFunc = TheModule->getFunction(iterName + "___has_next");
            if (!hasNextFunc) hasNextFunc = TheModule->getFunction(iterName + "__has_next");

            if (!hasNextFunc) {
                std::cerr << "Error: Iterator '" << iterName << "' missing __has_next()." << std::endl;
                exit(1);
            }
            Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj}, "has_next");
            Builder.CreateCondBr(hasNext, bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);
            Function* nextFunc = TheModule->getFunction(iterName + "___next");
            if (!nextFunc) nextFunc = TheModule->getFunction(iterName + "__next");

            if (!nextFunc) {
                std::cerr << "Error: Iterator '" << iterName << "' missing __next()." << std::endl;
                exit(1);
            }
            Value* item = Builder.CreateCall(nextFunc, {iteratorObj}, "item");

            varGen->defineLocalVariable(node->varName, item);

            for (auto& stmt : node->body) stmtHandler(stmt.get());

            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);
            Builder.SetInsertPoint(afterBB);
            return;
        }

        if (iterable->getType()->isPointerTy() && iterable->getType()->getPointerElementType()->isIntegerTy(8)) {
            std::vector<Value*> args = {iterable};
            Value* stringObj = initHelper("String", args);

            Function* iterFunc = TheModule->getFunction("String___iter");
            if (!iterFunc) iterFunc = TheModule->getFunction("String__iter");

            if (!iterFunc) {
                std::cerr << "Error: String struct missing __iter." << std::endl;
                exit(1);
            }

            Value* iteratorObj = Builder.CreateCall(iterFunc, {stringObj}, "iter");
            StructType* iterStructType = cast<StructType>(iteratorObj->getType()->getPointerElementType());
            std::string iterName = iterStructType->getName().str();

            BasicBlock* condBB = BasicBlock::Create(Context, "forcond", parentFunc);
            BasicBlock* bodyBB = BasicBlock::Create(Context, "forbody", parentFunc);
            BasicBlock* afterBB = BasicBlock::Create(Context, "forafter", parentFunc);

            Builder.CreateBr(condBB);
            Builder.SetInsertPoint(condBB);

            Function* hasNextFunc = TheModule->getFunction(iterName + "___has_next");
            if (!hasNextFunc) hasNextFunc = TheModule->getFunction(iterName + "__has_next");

            Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj});
            Builder.CreateCondBr(hasNext, bodyBB, afterBB);

            Builder.SetInsertPoint(bodyBB);
            Function* nextFunc = TheModule->getFunction(iterName + "___next");
            if (!nextFunc) nextFunc = TheModule->getFunction(iterName + "__next");

            Value* item = Builder.CreateCall(nextFunc, {iteratorObj});

            varGen->defineLocalVariable(node->varName, item);

            for (auto& stmt : node->body) stmtHandler(stmt.get());

            if (!Builder.GetInsertBlock()->getTerminator()) Builder.CreateBr(condBB);
            Builder.SetInsertPoint(afterBB);
            return;
        }

        std::cerr << "Error: Object is not iterable." << std::endl;
        exit(1);
    }

    // --- NEW: Updated TryCatch to perform Runtime String Type Evaluation ---
    void generateTryCatch(TryCatchNode* node, Function* parentFunc,
                      std::function<void(Node*)> stmtHandler,
                      VariableGen* varGen,
                      std::map<std::string, StructType*>& StructTypes,
                      std::map<std::string, std::vector<std::string>>& structHierarchy) {
                        
        BasicBlock* tryBB = BasicBlock::Create(Context, "try_block", parentFunc);
        BasicBlock* endBB = BasicBlock::Create(Context, "end_try", parentFunc);

        // Initial setup: Get jump buffer and setjmp
        Value* jmpBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_jmp_buf"));
        Value* setjmpRes = Builder.CreateCall(TheModule->getFunction("setjmp"), {jmpBuf});

        // Check if we are entering from setjmp (exception thrown) or normal flow
        Value* isCatch = Builder.CreateICmpNE(setjmpRes, ConstantInt::get(Type::getInt32Ty(Context), 0));
        
        // Create the first evaluation block in the chain
        BasicBlock* firstEvalBB = BasicBlock::Create(Context, "catch_eval_0", parentFunc);
        Builder.CreateCondBr(isCatch, firstEvalBB, tryBB);

        // --- TRY BLOCK ---
        Builder.SetInsertPoint(tryBB);
        for (auto& stmt : node->tryBlock) stmtHandler(stmt.get());
        
        if (!Builder.GetInsertBlock()->getTerminator()) {
            Builder.CreateCall(TheModule->getFunction("quirk_pop_try")); 
            Builder.CreateBr(endBB);
        }

        // --- CATCH BLOCKS CHAIN ---
        BasicBlock* currentEvalBB = firstEvalBB;
        
        // Assuming node->catchBlocks is a std::vector of CatchBlock structs
        // Each CatchBlock has: std::vector<std::string> types, std::string varName, std::vector<std::unique_ptr<Node>> body
        for (size_t i = 0; i < node->catchBlocks.size(); ++i) {
            auto& cb = node->catchBlocks[i];
            Builder.SetInsertPoint(currentEvalBB);

            Value* rawExc = Builder.CreateCall(TheModule->getFunction("quirk_get_exception"));
            
            // Setup type extraction (similar to previous version)
            Type* baseExcType = PointerType::getUnqual(StructTypes["Exception"]);
            Value* baseExc = Builder.CreateBitCast(rawExc, baseExcType);
            Value* typeFieldPtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 0);
            Value* typeStringObj = Builder.CreateLoad(PointerType::getUnqual(StructTypes["String"]), typeFieldPtr);
            Value* bufferPtr = Builder.CreateStructGEP(StructTypes["String"], typeStringObj, 0);
            Value* runtimeTypeCStr = Builder.CreateLoad(Type::getInt8PtrTy(Context), bufferPtr);

            FunctionCallee strcmpFunc = TheModule->getOrInsertFunction("strcmp", 
                FunctionType::get(Type::getInt32Ty(Context), {Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context)}, false));

            // 1. Gather ALL valid types for THIS catch block (including children for each listed type)
            std::vector<std::string> allValidTypes;
            for (const std::string& targetTypeName : cb.types) {
                allValidTypes.push_back(targetTypeName);
                for (const auto& pair : structHierarchy) {
                    std::function<bool(const std::string&)> inheritsFrom = [&](const std::string& t) {
                        if (t == targetTypeName) return true;
                        if (structHierarchy.count(t)) {
                            for (const auto& p : structHierarchy.at(t)) {
                                if (inheritsFrom(p)) return true;
                            }
                        }
                        return false;
                    };
                    if (inheritsFrom(pair.first) && pair.first != targetTypeName) {
                        allValidTypes.push_back(pair.first);
                    }
                }
            }

            // 2. Build the matching condition for this specific block
            Value* isMatch = ConstantInt::getFalse(Context);
            for (const std::string& vType : allValidTypes) {
                Value* targetTypeStr = Builder.CreateGlobalStringPtr(vType);
                Value* cmpRes = Builder.CreateCall(strcmpFunc, {runtimeTypeCStr, targetTypeStr});
                Value* isZero = Builder.CreateICmpEQ(cmpRes, ConstantInt::get(Type::getInt32Ty(Context), 0));
                isMatch = Builder.CreateOr(isMatch, isZero);
            }

            BasicBlock* matchBodyBB = BasicBlock::Create(Context, "catch_match_" + std::to_string(i), parentFunc);
            BasicBlock* nextEvalBB = (i + 1 < node->catchBlocks.size()) 
                                    ? BasicBlock::Create(Context, "catch_eval_" + std::to_string(i+1), parentFunc)
                                    : BasicBlock::Create(Context, "catch_rethrow", parentFunc);

            Builder.CreateCondBr(isMatch, matchBodyBB, nextEvalBB);

            // --- MATCH BODY ---
            Builder.SetInsertPoint(matchBodyBB);
            // Cast to the first type mentioned in the catch for the local variable
            Type* catchTypeLLVM = PointerType::getUnqual(StructTypes[cb.types[0]]);
            Value* castedExc = Builder.CreateBitCast(rawExc, catchTypeLLVM);
            varGen->defineLocalVariable(cb.varName, castedExc);

            for (auto& stmt : cb.body) stmtHandler(stmt.get());
            
            if (!Builder.GetInsertBlock()->getTerminator()) {
                Builder.CreateBr(endBB);
            }

            currentEvalBB = nextEvalBB;
        }

        // --- FINAL RETHROW BLOCK ---
        Builder.SetInsertPoint(currentEvalBB);
        Value* depth = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
        Value* hasParentCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));

        BasicBlock* jumpBB = BasicBlock::Create(Context, "rethrow_jump", parentFunc);
        BasicBlock* crashBB = BasicBlock::Create(Context, "rethrow_crash", parentFunc);
        Builder.CreateCondBr(hasParentCatch, jumpBB, crashBB);

        Builder.SetInsertPoint(jumpBB);
        Value* parentBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
        Builder.CreateCall(TheModule->getFunction("longjmp"), {parentBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(crashBB);
        Builder.CreateCall(TheModule->getFunction("quirk_unhandled_exception"));
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(endBB);
    }

    void generateThrow(ThrowNode* node, Function* parentFunc, std::function<Value*(Node*)> exprHandler) {
        Value* excObj = exprHandler(node->expression.get());
        Value* rawExc = Builder.CreateBitCast(excObj, Type::getInt8PtrTy(Context));
        
        Builder.CreateCall(TheModule->getFunction("quirk_set_exception"), {rawExc});

        Value* depth = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
        Value* hasCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));

        BasicBlock* jumpBB = BasicBlock::Create(Context, "do_longjmp", parentFunc);
        BasicBlock* crashBB = BasicBlock::Create(Context, "do_crash", parentFunc);
        Builder.CreateCondBr(hasCatch, jumpBB, crashBB);

        Builder.SetInsertPoint(jumpBB);
        Value* activeBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
        Builder.CreateCall(TheModule->getFunction("longjmp"), {activeBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
        Builder.CreateUnreachable();

        Builder.SetInsertPoint(crashBB);
        Builder.CreateCall(TheModule->getFunction("quirk_unhandled_exception"));
        Builder.CreateUnreachable();
    }
};