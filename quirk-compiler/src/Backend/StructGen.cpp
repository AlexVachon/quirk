#include <iostream>
#include <map>
#include <string>
#include "ast.hpp"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

class BuiltinGen;  // Forward Declaration

class StructGen {
    LLVMContext& Context;
    Module* TheModule;
    IRBuilder<>& Builder;
    std::map<std::string, StructType*>& StructTypes;
    BuiltinGen* builtinGen;

   public:
    StructGen(LLVMContext& ctx,
              Module* mod,
              IRBuilder<>& builder,
              std::map<std::string, StructType*>& structs)
        : Context(ctx),
          TheModule(mod),
          Builder(builder),
          StructTypes(structs),
          builtinGen(nullptr) {}

    void setBuiltinGen(BuiltinGen* bg) { builtinGen = bg; }

    void registerStructLayout(const std::string& name,
                              const std::vector<std::string>& fields) {
        structLayouts[name] = fields;
    }

    Value* allocateAndInit(const std::string& name, std::vector<Value*>& args) {
        if (!StructTypes.count(name)) {
            std::cerr << "Error: Unknown struct " << name << std::endl;
            return nullptr;
        }

        StructType* st = StructTypes[name];
        Value* allocSize = ConstantExpr::getSizeOf(st);

        Function* mallocFunc = TheModule->getFunction("malloc");
        if (!mallocFunc) {
            std::cerr << "Error: malloc not found" << std::endl;
            return nullptr;
        }

        if (allocSize->getType()->getIntegerBitWidth() < 64)
            allocSize =
                Builder.CreateZExt(allocSize, Type::getInt64Ty(Context));

        Value* rawPtr = Builder.CreateCall(mallocFunc, {allocSize});
        Value* objPtr =
            Builder.CreateBitCast(rawPtr, PointerType::getUnqual(st));

        std::string initName = name + "__init";
        Function* initFunc = TheModule->getFunction(initName);

        if (initFunc) {
            std::vector<Value*> initArgs;
            initArgs.push_back(objPtr);
            for (auto val : args)
                initArgs.push_back(val);
            Builder.CreateCall(initFunc, initArgs);
        } else {
            int idx = 0;
            for (auto val : args) {
                if (idx >= (int)st->getNumElements())
                    break;
                Value* fieldPtr = Builder.CreateStructGEP(st, objPtr, idx++);
                Builder.CreateStore(val, fieldPtr);
            }
        }
        return objPtr;
    }

    // --- ADDED THIS METHOD ---
    Value* generateConstructor(ConstructorNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> args;
        for (auto& arg : node->args) {
            args.push_back(exprHandler(arg.value.get()));
        }
        return allocateAndInit(node->structName, args);
    }
    // -------------------------

    Value* generateMemberAccess(Value* objPtr, const std::string& memberName) {
        Value* ptr = getMemberPtr(objPtr, memberName);
        if (!ptr)
            return nullptr;
        return Builder.CreateLoad(ptr->getType()->getPointerElementType(), ptr);
    }

    Value* getMemberPtr(Value* objPtr, const std::string& memberName) {
        // 1. Safety Check
        if (!objPtr->getType()->isPointerTy() ||
            !objPtr->getType()->getPointerElementType()->isStructTy()) {
            std::cerr << "[StructGen DEBUG] getMemberPtr failed: Not a struct pointer." << std::endl;
            return nullptr;
        }

        StructType* st =
            cast<StructType>(objPtr->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        std::cerr << "[StructGen DEBUG] Looking for member '" << memberName 
                  << "' in LLVM type: '" << structName << "'" << std::endl;

        // 2. Strip "struct." prefix
        if (structName.find("struct.") == 0) {
            structName = structName.substr(7);
            std::cerr << "[StructGen DEBUG] Stripped prefix -> '" << structName << "'" << std::endl;
        }

        // 3. Resolve Name (Fuzzy Lookup)
        std::string matchedName = "";
        
        if (structLayouts.count(structName)) {
            matchedName = structName;
            std::cerr << "[StructGen DEBUG] Found EXACT match for layout: '" << matchedName << "'" << std::endl;
        } else {
            // Fuzzy match logic
            for (auto const& [key, val] : structLayouts) {
                // Check if structName starts with key (e.g., "String.0" starts with "String")
                if (structName.find(key) == 0) {
                    matchedName = key;
                    std::cerr << "[StructGen DEBUG] Fuzzy matched '" << structName 
                              << "' to layout '" << matchedName << "'" << std::endl;
                    break;
                }
            }
        }

        if (matchedName.empty()) {
            std::cerr << "[StructGen ERROR] Layout NOT found for: " << structName << std::endl;
            std::cerr << "[StructGen DEBUG] Available layouts: ";
            for (auto const& [key, val] : structLayouts) std::cerr << key << " ";
            std::cerr << std::endl;
            return nullptr;
        }

        // 4. Find Field Index
        int index = -1;
        const auto& fields = structLayouts[matchedName];
        
        std::cerr << "[StructGen DEBUG] Fields in '" << matchedName << "': ";
        for (const auto& f : fields) std::cerr << f << ", ";
        std::cerr << std::endl;

        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i] == memberName) {
                index = i;
                break;
            }
        }

        if (index == -1) {
             std::cerr << "[StructGen ERROR] Field '" << memberName 
                       << "' not found in struct '" << matchedName << "'" << std::endl;
            return nullptr;
        }
        
        std::cerr << "[StructGen DEBUG] Found '" << memberName << "' at index " << index << std::endl;
        return Builder.CreateStructGEP(st, objPtr, index);
    }

    Value* createListFromValues(std::vector<Value*> values) {
        if (!StructTypes.count("List"))
            return nullptr;

        // 1. Create the buffer for the list items
        Type* voidPtr = Type::getInt8PtrTy(Context);

        // Safety: Allocate at least 8 bytes even if empty
        uint64_t bufSize = values.empty() ? 8 : values.size() * 8;
        Value* size = ConstantInt::get(Type::getInt64Ty(Context), bufSize);

        Function* mallocFunc = TheModule->getFunction("malloc");
        Value* buffer = Builder.CreateCall(mallocFunc, {size});
        Value* bufferPtr =
            Builder.CreateBitCast(buffer, PointerType::getUnqual(voidPtr));

        // 2. Fill the buffer
        for (size_t i = 0; i < values.size(); i++) {
            Value* slot = Builder.CreateGEP(
                voidPtr, bufferPtr,
                ConstantInt::get(Type::getInt32Ty(Context), i));
            Value* v = values[i];

            // Box integers/bools to void*
            if (v->getType()->isIntegerTy()) {
                v = Builder.CreateIntToPtr(v, voidPtr);
            } else if (v->getType() != voidPtr) {
                v = Builder.CreateBitCast(v, voidPtr);
            }
            Builder.CreateStore(v, slot);
        }

        // 3. Initialize the List Object
        // --- FIX: Pass 'initial_cap' argument to match List__init signature
        // ---
        std::vector<Value*> listArgs;
        int cap = values.empty() ? 1 : values.size();
        listArgs.push_back(ConstantInt::get(Type::getInt32Ty(Context), cap));

        Value* listObj = allocateAndInit("List", listArgs);
        // ----------------------------------------------------------------------

        // 4. Manually overwrite fields to point to our pre-filled buffer
        // Note: This technically leaks the empty buffer created inside
        // List__init, but it prevents the segfault and is safe for now.

        // self.data = buffer (Index 0)
        Value* dataPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 0);
        Builder.CreateStore(buffer, dataPtr);

        // self.length = size (Index 1)
        Value* lenPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 1);
        Builder.CreateStore(
            ConstantInt::get(Type::getInt32Ty(Context), values.size()), lenPtr);

        // self.capacity = cap (Index 2)
        Value* capPtr =
            Builder.CreateStructGEP(StructTypes["List"], listObj, 2);
        Builder.CreateStore(ConstantInt::get(Type::getInt32Ty(Context), cap),
                            capPtr);

        return listObj;
    }

    Value* generateListLiteral(ListLiteralNode* node,
                               std::function<Value*(Node*)> exprHandler) {
        std::vector<Value*> values;
        for (auto& elem : node->elements) {
            values.push_back(exprHandler(elem.get()));
        }
        return createListFromValues(values);
    }

    Value* generateMapLiteral(MapLiteralNode* node, 
                              std::function<Value*(Node*)> exprHandler) {
        // 1. Create Empty Map: m = Map()
        // We use allocateAndInit with empty args to trigger Map__init
        std::vector<Value*> emptyArgs;
        Value* mapObj = allocateAndInit("Map", emptyArgs);

        if (!mapObj) return nullptr;

        // 2. Get Map_put function
        Function* putFunc = TheModule->getFunction("Map_put");
        if (!putFunc) {
            std::cerr << "Error: Map_put not found. Cannot generate map literal." << std::endl;
            return mapObj;
        }

        // 3. Populate Map
        for (auto& pair : node->elements) {
            Value* key = exprHandler(pair.first.get());
            Value* val = exprHandler(pair.second.get());

            // --- A. Auto-box KEY (cstring -> String) ---
            if (key->getType()->isPointerTy() && 
                key->getType()->getPointerElementType()->isIntegerTy(8)) {
                std::vector<Value*> args = {key};
                key = allocateAndInit("String", args);
            }

            // --- B. Auto-box VALUE (Int/Ptr -> Void*) ---
            Type* voidPtr = Type::getInt8PtrTy(Context);
            
            if (val->getType()->isIntegerTy()) {
                // Int -> Void* (Relies on Runtime Small Int Hack)
                val = Builder.CreateIntToPtr(val, voidPtr);
            } 
            else if (val->getType()->isDoubleTy()) {
                // Double -> String -> Void* (Safety wrapper)
                Function* f2s = TheModule->getFunction("_float_to_str");
                if (f2s) {
                    Value* rawStr = Builder.CreateCall(f2s, {val});
                    std::vector<Value*> sArgs = {rawStr};
                    Value* strObj = allocateAndInit("String", sArgs);
                    val = Builder.CreateBitCast(strObj, voidPtr);
                } else {
                    // Fallback: dangerous bitcast
                    val = Builder.CreateBitCast(val, voidPtr); 
                }
            }
            else if (val->getType()->isPointerTy() && val->getType() != voidPtr) {
                // String* / List* -> Void*
                val = Builder.CreateBitCast(val, voidPtr);
            }

            // 4. Call put(map, key, val)
            Builder.CreateCall(putFunc, {mapObj, key, val});
        }

        return mapObj;
    }

    Value* generateStrCall(Value* obj, const std::string& structName) {
        std::string funcName = structName + "___str";
        Function* f = TheModule->getFunction(funcName);
        if (f)
            return Builder.CreateCall(f, {obj});

        return generateReprCall(obj, structName);
    }
    
    Value* generateReprCall(Value* obj, const std::string& structName) {
        std::string funcName = structName + "___repr";
        Function* f = TheModule->getFunction(funcName);
        if (f)
            return Builder.CreateCall(f, {obj});
        return nullptr;
    }

   private:
    std::map<std::string, std::vector<std::string>> structLayouts;
};