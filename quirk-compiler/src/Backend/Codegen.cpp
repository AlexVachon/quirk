#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "ast.hpp"

#include "TypeGen.hpp"
#include "BoxInt.hpp"
#include "VariableGen.hpp"
#include "MathGen.hpp"
#include "StructGen.hpp"
#include "BuiltinGen.hpp"
#include "TypeExtensions.hpp"
#include "ControlFlowGen.hpp"
#include "EnumGen.hpp"


using namespace llvm;

class LLVMCodegen {
    std::unique_ptr<LLVMContext> pCtx;  // heap-allocated so ownership can be transferred to JIT
    LLVMContext& Context;
    std::unique_ptr<Module> TheModule;
    IRBuilder<> Builder;

    bool verbose = false;
    // LLVM optimization level (0..3). Default 1 — runs the minimal pass set
    // the JIT needs for correctness. See `runOptimizations` for details.
    int  optLevel = 1;
    // When true, every statement-level handleStatement call emits a hook
    // into the IR that gives the runtime a chance to pause at qdb. Off by
    // default so non-debug runs pay zero overhead.
    bool debugMode = false;

    std::map<std::string, bool> variadicFunctions;
    std::map<std::string, FunctionNode*> functionDeclarations;
    // (moduleName, funcName) → FunctionNode*  — populated alongside
    // functionDeclarations so `csv.write(...)` can resolve to Csv_write
    // even when another module also defines a top-level `write`. The bare-
    // name map can only hold one entry per name; this disambiguates.
    std::map<std::pair<std::string, std::string>, FunctionNode*> moduleFunctionIndex;
    // Per-struct method registry: structName → methodName → FunctionNode.
    // Used by the binary-op overload dispatch to find e.g.
    // `Core_Collections_List_List___add` from a bare-`List` operand
    // type without having to know the module prefix at the call site.
    std::map<std::string, std::map<std::string, FunctionNode*>> structMethodNodes;
    std::map<std::string, StructType*> StructTypes;

    std::map<std::string, std::vector<std::string>> structHierarchy;

    // Virtual dispatch tables
    // Virtual-dispatch state moved into StructGen (v2.3.3 locality
    // refactor, sibling of the EnumGen extraction). Read at this site
    // via `structGen->isVtableEligible(name)` / `getOverrides(...)` /
    // `getTypeId(name)`. The old `structTypeIds` duplicate is gone —
    // `structGen->getTypeId(name)` reads straight from typeIdMap.

    // Enum state — moved into EnumGen (v2.3.2 locality refactor).
    // `enumGen` owns the registries; the reference aliases below let
    // the existing inline dispatch keep using `enumVariants[X]` /
    // `backedEnums[X]` syntax unchanged. Writes go through the EnumGen
    // accessor methods (registerEnum / trackVarEnum); reads via these
    // refs hit the same underlying maps.
    EnumGen enumGen{Context, nullptr, Builder, StructTypes};
    std::map<std::string, std::vector<std::string>>& enumVariants{*enumGen.variantsRegistryPtr()};
    std::map<std::string, std::string>&              varEnumTypes{*enumGen.varEnumTypesPtr()};
    using BackedEnumInfo = EnumGen::BackedEnumInfo;
    std::map<std::string, BackedEnumInfo>&           backedEnums{*enumGen.backedEnumsPtr()};
    // (struct name, field name) → declared field type. Populated in
    // the same Pass-1 walk that registers struct layouts; used so
    // `self.gender.value` (gender: Gender on the struct) can resolve
    // to the right backed-enum runtime helper.
    std::map<std::string, std::map<std::string, std::string>> structFieldTypes;

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
    // True while emitting code for a top-level statement at module scope
    // (i.e. inside the prologue of `main` for a module's free statements).
    // When set, `handleVarDecl` materialises module-level vars as LLVM
    // GlobalVariables so they're visible from other functions.
    bool inModuleScope = false;
    std::map<std::string, std::string> activeModuleAliases;
    std::map<std::string, std::string> callableReturnTypes; // varName → inferred return type
    std::map<std::string, int> variadicCallableStart;       // varName → first variadic arg index (-1 = not variadic)
    std::set<std::string> externBoolReturnFunctions;       // LLVM names widened i1→i32 for C ABI

    // Cache: function name → synthesized `i8*(i8* env, i8*…)` thunk Function*.
    // One thunk per function, reused on every "function-as-value" reference
    // (passing a function name where a Callable is expected, storing it in a
    // variable, returning it). See getOrCreateFunctionThunk().
    std::map<std::string, Function*> functionThunkCache;

    void setVerbose(bool v) { verbose = v; }
    void setOptLevel(int o) { optLevel = (o < 0 ? 0 : o > 3 ? 3 : o); }
    void setDebugMode(bool d) {
        debugMode = d;
        // varGen is built in the ctor before this setter runs, so push the
        // flag through whenever it lands. Module pointer is stable for the
        // life of the codegen.
        if (varGen) varGen->setDebugMode(d, TheModule.get());
    }

    void setSourceMap(const std::map<std::string, std::string>& sm) { sourceMap = sm; }

    [[noreturn]] void fatalError(const std::string& msg, int line, int col) {
        std::cerr << "\033[1;31m[ERROR]\033[0m " << msg << "\n";

        if (line > 0) {
            if (!currentFilePath.empty())
                std::cerr << " --> " << currentFilePath << ":" << line << ":" << col << "\n";
            else
                std::cerr << " --> line " << line << ":" << col << "\n";

            if (!currentFilePath.empty() && sourceMap.count(currentFilePath)) {
                const std::string& src = sourceMap.at(currentFilePath);
                int cur = 1;
                std::string lineText;
                std::istringstream ss(src);
                while (std::getline(ss, lineText)) {
                    if (cur++ == line) break;
                }
                std::string ln = std::to_string(line);
                int caretOff = (col > 1) ? col - 1 : 0;
                std::cerr << std::string(ln.size(), ' ') << " |\n";
                std::cerr << ln << " | " << lineText << "\n";
                std::cerr << std::string(ln.size(), ' ') << " | "
                          << std::string(caretOff, ' ')
                          << "\033[1;33m^--- here\033[0m\n\n";
            }
        }

        exit(1);
    }

    LLVMCodegen() : pCtx(std::make_unique<LLVMContext>()), Context(*pCtx), Builder(Context) {
        TheModule = std::make_unique<Module>("QuirkCompiler", Context);
        enumGen.setModule(TheModule.get());
        typeGen = std::make_unique<TypeGen>(Context, StructTypes);
        flowGen = std::make_unique<ControlFlowGen>(Context, TheModule.get(), Builder);
        structGen = std::make_unique<StructGen>(Context, TheModule.get(), Builder, StructTypes);
        builtinGen = std::make_unique<BuiltinGen>(Context, TheModule.get(), Builder, structGen.get());
        structGen->setBuiltinGen(builtinGen.get());

        typeExtensions = std::make_unique<TypeExtensions>(Context, TheModule.get(), Builder, structGen.get(), builtinGen.get());
        varGen = std::make_unique<VariableGen>(Context, Builder);
        varGen->setDebugMode(debugMode, TheModule.get());
        mathGen = std::make_unique<MathGen>(Context, Builder);
        mathGen->setModule(TheModule.get());
    }

   private:
    // ── run LLVM passes on the current module at the configured opt level ──
    //
    // Opt levels:
    //   -O0  → no passes. Currently unsafe: codegen emits "loose" IR that
    //          depends on at least the mem2reg/instcombine cleanup to be
    //          executable. Kept for future codegen rewrites.
    //   -O1  → minimal correctness pass pipeline (DEFAULT) — mem2reg + early
    //          CSE + instcombine + simplifycfg + DCE. Fixes the IR patterns
    //          the JIT needs without paying for full inlining/loop-opts/etc.
    //          Roughly 3-5× faster than O2 on short-running programs.
    //   -O2/3→ full LLVM pass pipeline. Slower compile, faster runtime —
    //          worth it for compute-heavy code (`--release`).
    void runOptimizations() {
        if (optLevel <= 0) {
            if (verbose) std::cerr << "[Codegen] skipping optimization passes (-O0)\n";
            return;
        }
        legacy::PassManager PM;
        legacy::FunctionPassManager FPM(TheModule.get());

        if (optLevel == 1) {
            if (verbose) std::cerr << "[Codegen] running minimal O1 passes\n";
            // Order is significant. SROA breaks aggregate allocas into
            // scalar pieces and resolves the i8*↔struct* type mismatches
            // our codegen leaves around. mem2reg promotes the resulting
            // scalar allocas to SSA. instcombine+simplifycfg+DCE clean up.
            // A second instcombine pass after CFG simplification catches
            // dead patterns the first pass exposed.
            FPM.add(createSROAPass());
            FPM.add(createPromoteMemoryToRegisterPass());  // mem2reg
            FPM.add(createEarlyCSEPass(/*UseMemorySSA=*/false));
            FPM.add(createInstructionCombiningPass());     // instcombine
            FPM.add(createCFGSimplificationPass());        // simplifycfg
            FPM.add(createInstructionCombiningPass());     // catch newly-folded
            FPM.add(createDeadCodeEliminationPass());      // dce
            FPM.doInitialization();
            for (auto& F : *TheModule) {
                if (!F.isDeclaration()) FPM.run(F);
            }
            FPM.doFinalization();
            return;
        }

        if (verbose) std::cerr << "[Codegen] running O" << optLevel << " passes\n";
        PassManagerBuilder PMB;
        PMB.OptLevel  = optLevel;
        PMB.SizeLevel = 0;
        PMB.Inliner   = createFunctionInliningPass(PMB.OptLevel, PMB.SizeLevel, false);
        PMB.populateModulePassManager(PM);
        PM.run(*TheModule);
    }

    // ── private: build IR from AST (no optimization, no emit) ──────────────
    void buildIR(const std::vector<std::unique_ptr<Node>>& nodes) {
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
        // Body must be { i8*, i8* } (fn ptr + env ptr) — pre-fill before Pass 2 so it is
        // never overwritten from the .quirk field list (emitCallableCall depends on these GEP offsets).
        if (!StructTypes.count("Callable")) StructTypes["Callable"] = StructType::create(Context, "struct.Callable");
        if (StructTypes["Callable"]->isOpaque()) {
            Type* i8PtrTy = Type::getInt8PtrTy(Context);
            StructTypes["Callable"]->setBody({i8PtrTy, i8PtrTy});
            structGen->registerStructLayout("Callable", {"fn", "env"});
        }

        // Pass 1b: Register enum variant lists (body generation deferred
        // to after Pass 3). Delegated to EnumGen — see EnumGen.hpp for
        // the variant-defaulting rule (Int-backed → ordinal index,
        // String-backed / unbacked → variant identifier).
        for (const auto& node : nodes) {
            if (auto* e = dynamic_cast<EnumNode*>(node.get())) {
                enumGen.registerEnum(e);
            }
        }
        // Hand the registry to TypeGen so `define f(l: L)` types `l` as
        // i32 (matching how `L.A` codegens), instead of falling through
        // to the i8* default.
        typeGen->setEnumRegistry(&enumVariants);

        // --- Virtual dispatch pre-scan ---
        // Determine which structs get __type_id (vtable-eligible):
        //   - Has at least one user-defined (non-extern) method with a body
        //   - Does NOT inherit from any all-extern struct (String, List, etc.) or Exception
        // "Callable" and "Type" are built-in special structs, always excluded.
        {
            // Structs whose __init is extern are C-backed (memory layout defined by C runtime).
            // We must NOT add __type_id to those, or the C ABI would break.
            // String, StringIterator, List, Map, Char, etc. all have extern __init.
            std::set<std::string> externOnly = {"Type", "Callable", "Tuple"};
            for (const auto& n : nodes) {
                if (auto f = dynamic_cast<FunctionNode*>(n.get())) {
                    if (!f->cls.empty() && f->isExtern &&
                        f->name.find("__init") != std::string::npos) {
                        externOnly.insert(f->cls);
                    }
                }
            }

            // Preliminary hierarchy from AST parents (structHierarchy built during Pass 2)
            std::map<std::string, std::vector<std::string>> astH;
            for (const auto& n : nodes)
                if (auto s = dynamic_cast<StructNode*>(n.get())) astH[s->name] = s->parents;

            // Recursively check: struct is vtable-eligible iff it and all ancestors
            // are not in externOnly and are not "Exception".
            auto eligible = [&](const std::string& name, std::set<std::string>& vis, auto& self) -> bool {
                if (vis.count(name)) return true;
                vis.insert(name);
                if (externOnly.count(name) || name == "Exception") return false;
                auto it = astH.find(name);
                if (it != astH.end())
                    for (const auto& p : it->second)
                        if (!self(p, vis, self)) return false;
                return true;
            };

            for (const auto& n : nodes) {
                if (auto s = dynamic_cast<StructNode*>(n.get())) {
                    std::set<std::string> vis;
                    if (eligible(s->name, vis, eligible)) structGen->markVtableEligible(s->name);
                }
            }

            // Assign unique type IDs (1-based; 0 = no type / extern)
            int nextId = 1;
            for (const auto& n : nodes)
                if (auto s = dynamic_cast<StructNode*>(n.get()))
                    if (structGen->isVtableEligible(s->name)) structGen->registerTypeId(s->name, nextId++);
        }
        // ---------------------------------

        std::unordered_map<std::string, StructNode*> structNodeMap;
        for (const auto& node : nodes)
            if (auto s = dynamic_cast<StructNode*>(node.get()))
                structNodeMap[s->name] = s;

        // (Auto-infer of struct fields from __init is done in the parser —
        // see Parser.cpp:parseStruct's "Implicit field inference" block.)

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
                        auto it = structNodeMap.find(parentName);
                        if (it != structNodeMap.end())
                            extractRef(it->second, types, names, extractRef);
                    }
                    for (const auto& field : sn->fields) {
                        Type* t = typeGen->getLLVMType(field.type);
                        if (t->isStructTy()) t = PointerType::getUnqual(t);
                        types.push_back(t);
                        names.push_back(field.name);
                        structFieldTypes[sn->name][field.name] = field.type;
                    }
                };

                structHierarchy[s->name] = s->parents;
                structGen->setHierarchy(&structHierarchy);
                typeExtensions->setHierarchy(&structHierarchy, &StructTypes);
                extractFields(s, elementTypes, fieldNames, extractFields);

                // Prepend __type_id (i32) as field 0 for vtable-eligible structs.
                // This provides the runtime type tag needed for virtual dispatch.
                if (structGen->isVtableEligible(s->name)) {
                    elementTypes.insert(elementTypes.begin(), Type::getInt32Ty(Context));
                    fieldNames.insert(fieldNames.begin(), "__type_id");
                }

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

        if (verbose) std::cerr << "[Codegen] Pass 3: Declaring function prototypes\n";
        for (const auto& node : nodes) {
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                functionDeclarations[func->name] = func;
                // Also key by linkageName so processCallArgs (which looks up
                // via the LLVM name = linkage name) can find the FunctionNode
                // for default-arg filling on library functions.
                if (!func->linkageName.empty() && func->linkageName != func->name) {
                    functionDeclarations[func->linkageName] = func;
                }
                // Per-struct method registry. Lets the operator-overload
                // dispatch path resolve `List.__add` to its full linkage
                // name (`Core_Collections_List_List___add`) without
                // walking every function in the module. Without this,
                // dispatch fell back to `getFunction("List___add")`
                // (bare struct + dunder) which only matched user
                // structs defined at module root with no package
                // prefix — every stdlib overload silently routed
                // through raw integer add, SIGSEGV-ing on the
                // dereference of List pointers as ints.
                if (!func->cls.empty()) {
                    // Index by the bare dunder/method name (strip the
                    // `<cls>_` prefix that parser preprends to
                    // func->name). e.g. for List's __add, func->name
                    // is "List___add" and func->cls is "List"; we
                    // store it under "___add" so the operator-overload
                    // dispatch can look it up via the opMethods
                    // table directly.
                    std::string raw = func->name;
                    std::string prefix = func->cls + "_";
                    if (raw.rfind(prefix, 0) == 0) raw = raw.substr(prefix.size());
                    structMethodNodes[func->cls][raw] = func;
                }
                if (!func->moduleName.empty() && func->cls.empty()) {
                    // Original PascalCase key (from computeModulePrefix).
                    moduleFunctionIndex[{func->moduleName, func->name}] = func;
                    // ALSO a lowercased key — `use console` aliases to the
                    // lowercase form the user typed, while the parser
                    // computed "Console". Without this second entry the
                    // module-qualified lookup misses and falls through to
                    // bare-name resolution, which picks the wrong function
                    // when two modules share a name (`console.info` vs
                    // `mylib.info`).
                    auto lowered = func->moduleName;
                    for (auto& ch : lowered) ch = (char)std::tolower((unsigned char)ch);
                    if (lowered != func->moduleName)
                        moduleFunctionIndex[{lowered, func->name}] = func;

                    // Module names from file paths end in ".index" for
                    // foo/index.quirk packages, but `use foo` aliases to "foo".
                    // Index under the trimmed form too so the lookup matches.
                    std::string trimmed = func->moduleName;
                    const std::string idx = ".index";
                    if (trimmed.size() > idx.size() &&
                        trimmed.compare(trimmed.size() - idx.size(), idx.size(), idx) == 0) {
                        trimmed = trimmed.substr(0, trimmed.size() - idx.size());
                        moduleFunctionIndex[{trimmed, func->name}] = func;
                        auto trimmedLower = trimmed;
                        for (auto& ch : trimmedLower) ch = (char)std::tolower((unsigned char)ch);
                        if (trimmedLower != trimmed)
                            moduleFunctionIndex[{trimmedLower, func->name}] = func;
                    }
                }

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
                
                // Skip Pass 3 registration for `main` and for top-level user
                // functions that would shadow a hard builtin (`print`, `write`,
                // `type`, ...). A function counts as user-toplevel only when
                // `linkageName == name` — module-prefixed library functions
                // (e.g. `Csv_write` for `csv.write`) get a distinct linkage
                // name during parsing and ARE registered, so libraries can
                // freely use builtin-shaped names.
                if (func->name == "main") continue;
                if (builtinGen->isBuiltin(func->name) && func->cls.empty()
                    && func->linkageName == func->name) continue;
                if (verbose) std::cerr << "[Codegen]   Declaring prototype: " << func->name << "\n";
                Type* retTy = typeGen->getFunctionReturnType(func->returnType);
                bool retIsBool = func->isExtern && retTy->isIntegerTy(1);
                // C ABI: Bool return values are int (i32), not i1.
                // Widen i1 return types for extern functions to avoid truncation.
                if (retIsBool) retTy = Type::getInt32Ty(Context);
                std::vector<Type*> argTypes;
                if (!func->cls.empty() && !func->isStatic) {
                    Type* selfTy = typeGen->getLLVMType(func->cls);
                    // C ABI: Bool self is passed as int (i32), not i1.
                    if (func->isExtern && selfTy && selfTy->isIntegerTy(1))
                        selfTy = Type::getInt32Ty(Context);
                    argTypes.push_back(selfTy);
                }
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
                if (retIsBool) externBoolReturnFunctions.insert(llvmName);

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
                // e.g. String__init in libs/core/string.quirk -> "Core_String_String___init"
                if (func->isExtern && !func->cls.empty() &&
                    func->name.find("__init") != std::string::npos) {
                    structGen->registerStructInit(func->cls, llvmName);
                }
            }
        }

        // --- Post-Pass-3: Virtual dispatch setup ---
        // Build the override map for type-switch dispatch — for each
        // user-defined method on a vtable-eligible struct, record the
        // chain of ancestor types that also define it. StructGen owns
        // the resulting map and exposes it via `getOverrides(parent,
        // method)` at the dispatch site. Type IDs were already
        // registered in the Pass-1c eligibility pre-scan.
        {
            auto methodSuffix = [](const std::string& fn, const std::string& cls) -> std::string {
                if (fn.size() <= cls.size()) return "";
                std::string s = fn.substr(cls.size());
                // "__init", "__str", etc.: suffix starts with "__"
                if (s.size() >= 2 && s[0] == '_' && s[1] == '_') return s;
                // regular methods: "ClassName_method" → strip the leading "_"
                if (!s.empty() && s[0] == '_') return s.substr(1);
                return "";
            };

            for (const auto& [funcName, funcNode] : functionDeclarations) {
                const std::string& cls = funcNode->cls;
                if (cls.empty() || !structGen->isVtableEligible(cls) || funcNode->isExtern) continue;
                std::string rawMethod = methodSuffix(funcName, cls);
                if (rawMethod.empty()) continue;

                auto checkAncestor = [&](const std::string& parent, auto& self) -> void {
                    if (!structGen->isVtableEligible(parent)) return;
                    std::string parentFn = (rawMethod.size() >= 2 && rawMethod[0] == '_' && rawMethod[1] == '_')
                        ? parent + rawMethod
                        : parent + "_" + rawMethod;
                    if (functionDeclarations.count(parentFn))
                        structGen->recordOverride(parent, rawMethod, cls, structGen->getTypeId(cls));
                    auto hit = structHierarchy.find(parent);
                    if (hit != structHierarchy.end())
                        for (const auto& gp : hit->second) self(gp, self);
                };

                auto hit = structHierarchy.find(cls);
                if (hit != structHierarchy.end())
                    for (const auto& parent : hit->second) checkAncestor(parent, checkAncestor);
            }
        }
        // -------------------------------------------

        // Pass 3b: Per-enum LLVM emission — packed value/name globals,
        // the `__<Enum>_name` C string, and the `__<Enum>_str(i32)`
        // helper that maps ordinal → variant-name String*. All
        // delegated to EnumGen; runs after Pass 3 so String's body
        // and __init prototype are both declared.
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

            enumGen.emitStrFns(nodes, FunctionCallee(makeStrFn), strPtrTy);
            enumGen.emitGlobals();
        }

        // We used to compile non-main function bodies (Pass 4) *before* main,
        // but that left top-level `counter := 0` style declarations invisible
        // to those bodies — the global wasn't registered yet. Emitting main
        // first lets it create the GlobalVariables (via `inModuleScope` in
        // handleVarDecl) so subsequent function bodies see them through
        // varGen's globalVars fallback. `clear()` preserves globalVars.

        FunctionType* mainType = FunctionType::get(
            Type::getInt32Ty(Context),
            {Type::getInt32Ty(Context), Type::getInt8PtrTy(Context)->getPointerTo()},
            false
        );
        Function* mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", TheModule.get());
        Builder.SetInsertPoint(BasicBlock::Create(Context, "entry", mainFunc));

        if (verbose) std::cerr << "[Codegen] Pass 4: Compiling 'main' body (creates module globals first)\n";
        varGen->clear();
        // varEnumTypes tracks `s := SomeEnum(...)` bindings so `s.value`
        // codegens via the enum-accessor path. Clearing alongside
        // varGen avoids leaks across function boundaries — e.g. a
        // top-level `s := Status(404)` from one user file used to
        // make `s.value` in a library helper (Option.unwrap_or's
        // `case Some as s => return s.value`) route into
        // quirk_enum_value_int with a Some* receiver and crash IR.
        varEnumTypes.clear();  // avoid variable table leaking from last compiled function into main
        Value* argc = mainFunc->getArg(0);
        Value* argv = mainFunc->getArg(1);
        
        FunctionCallee runtimeInit = TheModule->getOrInsertFunction("QuirkRuntime_init", 
            FunctionType::get(Type::getVoidTy(Context), {Type::getInt32Ty(Context), Type::getInt8PtrTy(Context)->getPointerTo()}, false));
            
        Builder.CreateCall(runtimeInit, {argc, argv});

        // --- Push 'main' to the shadow stack ---
        // Locate the user's entry-file path so the frame carries a real path
        // (not the literal "main"). Without this the VSCode debugger tries
        // to open a phantom file called "main" when it shows the stack frame.
        std::string mainFile;
        for (const auto& n : nodes) {
            if (auto fn = dynamic_cast<FunctionNode*>(n.get())) {
                if (fn->name == "main" && !fn->filePath.empty()) {
                    mainFile = fn->filePath;
                    break;
                }
            }
        }
        if (mainFile.empty()) {
            // Fall back to the first user node with a path — covers scripts
            // with no explicit main() (top-level code only).
            for (const auto& n : nodes) {
                if (!n->filePath.empty()) { mainFile = n->filePath; break; }
            }
        }
        if (mainFile.empty()) mainFile = "main";  // last-resort sentinel

        FunctionCallee pushFrame = TheModule->getOrInsertFunction("quirk_push_frame",
            Type::getVoidTy(Context), Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context),
            Type::getInt32Ty(Context));

        Value* mainFuncName = Builder.CreateGlobalStringPtr("main");
        Value* mainFileName = Builder.CreateGlobalStringPtr(mainFile);
        Builder.CreateCall(pushFrame, {mainFuncName, mainFileName,
            ConstantInt::get(Type::getInt32Ty(Context), 0)});
        // ---------------------------------------------
        
        for (const auto& node : nodes) {
            if (!node->filePath.empty()) currentFilePath = node->filePath;
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name == "main") {
                    for (const auto& stmt : func->body) handleStatement(stmt.get(), mainFunc);
                }
            } else if (!dynamic_cast<StructNode*>(node.get()) && !dynamic_cast<FunctionNode*>(node.get()) &&
                       !dynamic_cast<TypeAliasNode*>(node.get()) &&
                       !dynamic_cast<InterfaceNode*>(node.get())) {
                // Free top-level statements are emitted into main's prologue
                // and treated as module-scope: a top-level VarDecl becomes a
                // GlobalVariable so other functions see the same storage.
                inModuleScope = true;
                handleStatement(node.get(), mainFunc);
                inModuleScope = false;
            }
        }

        // Pop 'main' from the shadow stack and emit fallthrough return, but only if
        // the current block doesn't already have a terminator (explicit return 0 already
        // popped the frame and emitted ret via ReturnNode handling).
        if (!Builder.GetInsertBlock()->getTerminator()) {
            FunctionCallee popFrame = TheModule->getOrInsertFunction("quirk_pop_frame", Type::getVoidTy(Context));
            Builder.CreateCall(popFrame);
            Builder.CreateRet(ConstantInt::get(Type::getInt32Ty(Context), 0));
        }

        // Pass 5: compile non-main function bodies LAST. By now main has run
        // through any top-level VarDecls and registered the resulting
        // GlobalVariables with varGen, so these bodies can resolve module-
        // scope names via the globals fallback.
        if (verbose) std::cerr << "[Codegen] Pass 5: Compiling non-main function bodies\n";
        for (const auto& node : nodes) {
            if (!node->filePath.empty()) currentFilePath = node->filePath;
            if (auto func = dynamic_cast<FunctionNode*>(node.get())) {
                if (func->name != "main") compileFunction(func);
            }
        }

        if (verbose) std::cerr << "[Codegen] buildIR() done\n";
    }

   public:
    // Compile to IR file (used by --emit-ir and --compile-only)
    void compile(const std::vector<std::unique_ptr<Node>>& nodes, raw_ostream& out = errs()) {
        buildIR(nodes);
        runOptimizations();
        if (verbose) std::cerr << "[Codegen] emitting IR\n";
        TheModule->print(out, nullptr);
    }

    // Build + optimize, then transfer module ownership to the caller (for JIT use).
    // The codegen object must not be used after this call.
    std::pair<std::unique_ptr<Module>, std::unique_ptr<LLVMContext>>
    compileAndRelease(const std::vector<std::unique_ptr<Node>>& nodes) {
        buildIR(nodes);
        runOptimizations();
        return { std::move(TheModule), std::move(pCtx) };
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
        // varEnumTypes tracks `s := SomeEnum(...)` bindings so `s.value`
        // codegens via the enum-accessor path. Clearing alongside
        // varGen avoids leaks across function boundaries — e.g. a
        // top-level `s := Status(404)` from one user file used to
        // make `s.value` in a library helper (Option.unwrap_or's
        // `case Some as s => return s.value`) route into
        // quirk_enum_value_int with a Some* receiver and crash IR.
        varEnumTypes.clear();

        // Push the shadow frame BEFORE binding parameters so that any
        // Debug_register_local calls emitted by defineArgument see the
        // correct frame depth (this function's, not the caller's). The
        // matching pop happens at function exit; pushing earlier within the
        // same body doesn't change pop balance.
        if (!node->isDecoratorWrapper) {
            FunctionCallee pushFrame = TheModule->getOrInsertFunction("quirk_push_frame",
                Type::getVoidTy(Context), Type::getInt8PtrTy(Context), Type::getInt8PtrTy(Context),
                Type::getInt32Ty(Context));

            Value* funcNameVal = Builder.CreateGlobalStringPtr(node->name);
            // Prefer the AST node's filePath (the actual source path) over
            // moduleName (a logical name like "math"). DAP clients open
            // frames by clicking the source link; a bare module name fails
            // the file-exists check in the IDE.
            std::string fnFile = !node->filePath.empty() ? node->filePath : node->moduleName;
            Value* fileNameVal = Builder.CreateGlobalStringPtr(fnFile);
            Value* lineNumVal  = ConstantInt::get(Type::getInt32Ty(Context), node->line);
            Builder.CreateCall(pushFrame, {funcNameVal, fileNameVal, lineNumVal});
        }

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

        // --- where clause guard ---
        if (node->whereClause) {
            Value* cond = handleExpression(node->whereClause.get());
            if (cond) {
                Value* condBool;
                if (cond->getType()->isIntegerTy(1))
                    condBool = cond;
                else if (cond->getType()->isIntegerTy())
                    condBool = Builder.CreateICmpNE(cond, ConstantInt::get(cond->getType(), 0), "where_cond");
                else
                    condBool = cond;

                BasicBlock* failBB = BasicBlock::Create(Context, "where_fail", F);
                BasicBlock* passBB = BasicBlock::Create(Context, "where_pass", F);
                Builder.CreateCondBr(condBool, passBB, failBB);

                Builder.SetInsertPoint(failBB);
                // Throw WhereConditionError — same sequence as generateThrow
                std::string errMsg = "where clause violated in '" + node->name + "'";
                Value* rawMsg = Builder.CreateGlobalStringPtr(errMsg);
                std::vector<Value*> msgArgs = {rawMsg};
                Value* strObj = structGen->allocateAndInit("String", msgArgs);
                std::vector<Value*> excArgs = {strObj};
                Value* excObj = structGen->allocateAndInit("WhereConditionError", excArgs);

                if (StructTypes.count("Exception")) {
                    Type* baseExcTy = PointerType::getUnqual(StructTypes["Exception"]);
                    Value* baseExc  = Builder.CreateBitCast(excObj, baseExcTy);
                    Value* filePtr  = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 2);
                    Value* rawFile  = Builder.CreateGlobalStringPtr(node->moduleName.empty() ? "unknown" : node->moduleName);
                    std::vector<Value*> fileArgs = {rawFile};
                    Builder.CreateStore(structGen->allocateAndInit("String", fileArgs), filePtr);
                    Value* calleePtr = Builder.CreateStructGEP(StructTypes["Exception"], baseExc, 4);
                    Value* rawCallee = Builder.CreateGlobalStringPtr(node->name);
                    std::vector<Value*> calleeArgs = {rawCallee};
                    Builder.CreateStore(structGen->allocateAndInit("String", calleeArgs), calleePtr);
                }

                Value* rawExc = Builder.CreateBitCast(excObj, Type::getInt8PtrTy(Context));
                FunctionCallee captureFn = TheModule->getOrInsertFunction(
                    "quirk_capture_traceback",
                    FunctionType::get(Type::getVoidTy(Context), {Type::getInt8PtrTy(Context)}, false));
                Builder.CreateCall(captureFn, {rawExc});
                Builder.CreateCall(TheModule->getFunction("quirk_set_exception"), {rawExc});

                Value* depth    = Builder.CreateCall(TheModule->getFunction("quirk_get_try_depth"));
                Value* hasCatch = Builder.CreateICmpSGE(depth, ConstantInt::get(Type::getInt32Ty(Context), 0));
                BasicBlock* jumpBB  = BasicBlock::Create(Context, "where_longjmp", F);
                BasicBlock* crashBB = BasicBlock::Create(Context, "where_crash",   F);
                Builder.CreateCondBr(hasCatch, jumpBB, crashBB);

                Builder.SetInsertPoint(jumpBB);
                Value* activeBuf = Builder.CreateCall(TheModule->getFunction("quirk_get_current_jmp_buf"));
                Builder.CreateCall(TheModule->getFunction("quirk_pop_try"));
                Builder.CreateCall(TheModule->getFunction("longjmp"),
                                   {activeBuf, ConstantInt::get(Type::getInt32Ty(Context), 1)});
                Builder.CreateUnreachable();

                Builder.SetInsertPoint(crashBB);
                flowGen->emitUnhandledException(StructTypes);

                Builder.SetInsertPoint(passBB);
            }
        }
        // ---------------------------------

        // ── Decorator wrapper: lazy-init + cached dispatch ───────────────────
        // For `@dec define foo(args) {...}`, the parser produced this wrapper
        // function with `isDecoratorWrapper = true` and stashed the chain
        // expression (`dec(foo__inner__)`) in `decoratorChainExpr`. Emit:
        //
        //   @foo__wrapper = internal global Callable* null
        //
        //   define foo(args):
        //     entry:
        //       %cached = load Callable*, @foo__wrapper
        //       %null   = icmp eq Callable* %cached, null
        //       br %null, label %init, label %ready
        //     init:
        //       %fresh = <evaluate decoratorChainExpr — yields Callable*>
        //       store %fresh, @foo__wrapper
        //       br label %ready
        //     ready:
        //       %wrapper = load Callable*, @foo__wrapper
        //       <boxed args>
        //       %raw = call %wrapper(boxed args...)
        //       <unbox %raw to returnType>
        //       ret <result>
        //
        // The chain runs exactly once over the program's lifetime — captures
        // in stateful decorators (`@cached`, `@retry`) persist across calls.
        if (node->isDecoratorWrapper && node->decoratorChainExpr) {
            StructType* callableTy = StructTypes["Callable"];
            Type* callablePtrTy   = PointerType::getUnqual(callableTy);
            Type* i8PtrTy         = Type::getInt8PtrTy(Context);

            // Module-internal global, one per wrapper. Name keyed on the
            // function so collisions can't happen across files.
            std::string gName = "__qd_wrap_" + (node->linkageName.empty() ? node->name : node->linkageName);
            GlobalVariable* gv = TheModule->getNamedGlobal(gName);
            if (!gv) {
                gv = new GlobalVariable(*TheModule, callablePtrTy, /*isConstant=*/false,
                                        GlobalValue::InternalLinkage,
                                        ConstantPointerNull::get(cast<PointerType>(callablePtrTy)),
                                        gName);
            }

            BasicBlock* initBB  = BasicBlock::Create(Context, "dec_init",  F);
            BasicBlock* readyBB = BasicBlock::Create(Context, "dec_ready", F);

            // Entry block — branch on cache state.
            Value* cached = Builder.CreateLoad(callablePtrTy, gv, "dec_cached");
            Value* isNull = Builder.CreateICmpEQ(cached,
                ConstantPointerNull::get(cast<PointerType>(callablePtrTy)), "dec_uninit");
            Builder.CreateCondBr(isNull, initBB, readyBB);

            // Init block — evaluate the decorator chain once and cache.
            Builder.SetInsertPoint(initBB);
            Value* freshCallable = handleExpression(node->decoratorChainExpr.get());
            if (!freshCallable) {
                fatalError("decorator chain produced no value for '" + node->name + "'",
                           node->line, node->col);
            }
            // The chain's outermost value might come back as i8* (when the
            // final decorator returns a Callable through the Callable ABI).
            // Cast back to Callable* before storing.
            if (freshCallable->getType() == i8PtrTy) {
                freshCallable = Builder.CreateBitCast(freshCallable, callablePtrTy, "dec_cast");
            }
            Builder.CreateStore(freshCallable, gv);
            Builder.CreateBr(readyBB);

            // Ready block — dispatch through the cached wrapper.
            Builder.SetInsertPoint(readyBB);
            Value* wrapper = Builder.CreateLoad(callablePtrTy, gv, "dec_wrapper");
            std::vector<Value*> argVals;
            for (auto& p : node->parameters) {
                if (Value* v = varGen->resolveVariable(p.name)) argVals.push_back(v);
            }
            Value* raw = emitCallableCall(wrapper, argVals);

            // Unbox the i8* result back to whatever the wrapper claims to
            // return. Same policy as the in-line Callable-variable dispatch.
            // No pop_frame here — we skipped the wrapper's push, so the
            // shadow-stack is unchanged across this dispatcher.
            Value* unboxed = raw;
            const std::string& rt = node->returnType;
            if (rt == "void") {
                Builder.CreateRetVoid();
                currentCodegenClass = prevClass;
                if (prevBB) Builder.SetInsertPoint(prevBB);
                return;
            }
            if (rt == "Int" || rt == "Int32") {
                // The wrapped Callable returns i8*; the user-declared
                // return type is Int. Two encodings can show up:
                //   - Legacy tagged int (PtrToInt round-trips)
                //   - Any* heap-box (PtrToInt would return the pointer
                //     address, not the int — e.g. a nonlocal Int
                //     captured in the wrapped closure leaks as
                //     `(int32)(uintptr_t)Any*`).
                // Route through quirk_opaque_to_int which handles both.
                Type* i8p = Type::getInt8PtrTy(Context);
                FunctionCallee toInt = TheModule->getOrInsertFunction(
                    "quirk_opaque_to_int", Type::getInt32Ty(Context), i8p);
                unboxed = Builder.CreateCall(toInt, {raw}, "dec_int");
            } else if (rt == "Bool") {
                unboxed = Builder.CreateICmpNE(raw,
                    ConstantPointerNull::get(cast<PointerType>(raw->getType())), "dec_bool");
            } else if (rt == "Double" || rt == "Float") {
                Type* i8p = Type::getInt8PtrTy(Context);
                FunctionCallee toDbl = TheModule->getOrInsertFunction(
                    "quirk_opaque_to_double", Type::getDoubleTy(Context), i8p);
                unboxed = Builder.CreateCall(toDbl, {raw}, "dec_dbl");
            } else if (!rt.empty() && StructTypes.count(rt)) {
                unboxed = Builder.CreateBitCast(raw,
                    PointerType::getUnqual(StructTypes[rt]), "dec_struct");
            }
            Builder.CreateRet(unboxed);

            currentCodegenClass = prevClass;
            if (prevBB) Builder.SetInsertPoint(prevBB);
            return;
        }

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
                auto inheritsException = [&](const std::string& c, auto& self) -> bool {
                    if (c == "Exception") return true;
                    auto it = structHierarchy.find(c);
                    if (it != structHierarchy.end())
                        for (const auto& p : it->second)
                            if (self(p, self)) return true;
                    return false;
                };
                if (inheritsException(node->cls, inheritsException)) {
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
                    case 'n':  res += '\n'; break;
                    case 't':  res += '\t'; break;
                    case 'r':  res += '\r'; break;
                    case 'a':  res += '\a'; break;
                    case 'b':  res += '\b'; break;
                    case 'f':  res += '\f'; break;
                    case 'v':  res += '\v'; break;
                    case '\\': res += '\\'; break;
                    case '"':  res += '\"'; break;
                    case '\'': res += '\''; break;
                    case 'x': {
                        // \xNN — 2 hex digits (e.g. \x1b for ESC)
                        auto isHex = [](char c) {
                            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                        };
                        if (i + 3 < raw.size() && isHex(raw[i + 2]) && isHex(raw[i + 3])) {
                            char buf[3] = { raw[i + 2], raw[i + 3], '\0' };
                            res += (char)std::strtol(buf, nullptr, 16);
                            i += 2;  // consume the two hex digits in addition to the i++ below
                        } else {
                            res += raw[i]; res += raw[i + 1];
                        }
                        break;
                    }
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
        if (val->getType()->isPointerTy()) {
            // Plain bitcast — the value is already in pointer form,
            // so we just reinterpret as i8*. NOTE: we intentionally
            // do NOT auto-call `__str` on user structs here. An
            // older version did, on the theory that boxToVoidPtr
            // was always feeding `print()`, but it's also the
            // canonical "send a value through a Callable return /
            // generic i8* slot" path. Auto-stringifying a Response
            // before handing it back to the caller of
            // `srv.listen(..., handler)` corrupted the value: the
            // caller bitcast the String* back to Response* and
            // read garbage offsets. The print/format path has its
            // own valueToString helper in BuiltinGen, so removing
            // the auto-str here just routes stringification through
            // the consumer that asked for it.
            return Builder.CreateBitCast(val, i8PtrTy);
        }
        // i1 booleans: box as Any_Bool so quirk_opaque_to_string returns "true"/"false"
        if (val->getType()->isIntegerTy(1)) {
            Function* boxBool = TheModule->getFunction("Core_Primitives_Any_box_bool");
            if (!boxBool) {
                FunctionType* ft = FunctionType::get(
                    Type::getInt8PtrTy(Context),
                    {Type::getInt32Ty(Context)}, false);
                boxBool = Function::Create(ft, Function::ExternalLinkage,
                                           "Core_Primitives_Any_box_bool", TheModule.get());
            }
            Value* ext = Builder.CreateZExt(val, Type::getInt32Ty(Context));
            return Builder.CreateCall(boxBool, {ext});
        }
        if (val->getType()->isIntegerTy())
            return quirk::boxIntToOpaque(Context, TheModule.get(), Builder, val, i8PtrTy);
        if (val->getType()->isDoubleTy()) {
            // Route through `Any_box_double` so the value survives a
            // round-trip through `quirk_opaque_to_string`. The
            // previous `bitcast → inttoptr` trick stored the double's
            // raw bit pattern as a fake pointer; when a Double was
            // stored in a tuple, list, or map and the container was
            // printed later, the renderer tried to dereference that
            // pretend-pointer (3.14's bit pattern reads as a wild
            // memory address) and SIGSEGV'd. Boxing produces a real
            // heap Any* with ANY_DOUBLE tag that the runtime can
            // safely inspect.
            Function* boxDbl = TheModule->getFunction("Core_Primitives_Any_box_double");
            if (!boxDbl) {
                FunctionType* ft = FunctionType::get(
                    i8PtrTy, {Type::getDoubleTy(Context)}, false);
                boxDbl = Function::Create(ft, Function::ExternalLinkage,
                                          "Core_Primitives_Any_box_double",
                                          TheModule.get());
            }
            return Builder.CreateCall(boxDbl, {val});
        }
        return ConstantPointerNull::get(cast<PointerType>(i8PtrTy));
    }

    // Unbox an i8* argument to the expected type based on the lambda parameter annotation
    Value* unboxLambdaParam(Value* raw, const std::string& type) {
        // Two Any encodings can show up here:
        //   - Inline-tag (`IntToPtr(N)`) — fast path used for
        //     non-zero ints. Raw PtrToInt round-trips the value.
        //   - Heap Any* (from box_int / box_double) — used for
        //     zero ints (v3.2.0 BoxInt fix) and any value that
        //     went through `Any_box_*`. Raw PtrToInt would give
        //     the pointer address, not the contained value.
        // Route both through the runtime helpers so the lambda
        // sees the same Int it was conceptually passed.
        if (type == "Int" || type == "Int32") {
            Type* i8p = Type::getInt8PtrTy(Context);
            FunctionCallee toInt = TheModule->getOrInsertFunction(
                "quirk_opaque_to_int", Type::getInt32Ty(Context), i8p);
            return Builder.CreateCall(toInt, {raw}, "lambda_unbox_int");
        }
        if (type == "Bool")
            return Builder.CreateICmpNE(raw, ConstantPointerNull::get(cast<PointerType>(raw->getType())), "lambdabool");
        if (type == "Float" || type == "Double") {
            Type* i8p = Type::getInt8PtrTy(Context);
            FunctionCallee toDbl = TheModule->getOrInsertFunction(
                "quirk_opaque_to_double", Type::getDoubleTy(Context), i8p);
            return Builder.CreateCall(toDbl, {raw}, "lambda_unbox_dbl");
        }
        if (!type.empty() && StructTypes.count(type))
            return Builder.CreateBitCast(raw, PointerType::getUnqual(StructTypes[type]));
        return raw; // pass through as i8*
    }

    // Generate (or fetch from cache) a `i8*(i8* env, i8*…)` thunk that wraps
    // a user-defined function in the Callable ABI: unbox each i8* arg to its
    // declared type, call the real function, box the return back to i8*.
    //
    // The thunk lets `define`d functions be used as first-class Callable
    // values — passed as args, stored in variables, returned. The env pointer
    // is unused (always null) because user-defined functions have no captures.
    //
    // Returns the thunk Function*; the caller allocates the wrapping Callable
    // struct {thunk, null} at the use site.
    Function* getOrCreateFunctionThunk(const std::string& fnName) {
        if (functionThunkCache.count(fnName)) return functionThunkCache[fnName];
        if (!functionDeclarations.count(fnName)) return nullptr;
        FunctionNode* funcNode = functionDeclarations[fnName];
        Function* realFn = resolveFunction(fnName, currentCodegenClass);
        if (!realFn) return nullptr;
        Type* i8PtrTy = Type::getInt8PtrTy(Context);

        std::vector<Type*> thunkParamTypes = {i8PtrTy}; // env (unused)
        for (size_t i = 0; i < funcNode->parameters.size(); i++)
            thunkParamTypes.push_back(i8PtrTy);
        FunctionType* thunkTy = FunctionType::get(i8PtrTy, thunkParamTypes, false);
        Function* thunk = Function::Create(
            thunkTy, Function::InternalLinkage,
            fnName + "__cb_thunk", TheModule.get());

        // Stash current insert point — we'll restore after writing the thunk body.
        BasicBlock* savedBB = Builder.GetInsertBlock();
        BasicBlock* entryBB = BasicBlock::Create(Context, "entry", thunk);
        Builder.SetInsertPoint(entryBB);

        // Unbox each arg according to its declared parameter type.
        std::vector<Value*> realArgs;
        auto argIt = thunk->arg_begin();
        ++argIt; // skip env
        for (size_t i = 0; i < funcNode->parameters.size(); i++, ++argIt) {
            Value* raw = &*argIt;
            raw->setName(funcNode->parameters[i].name);
            realArgs.push_back(unboxLambdaParam(raw, funcNode->parameters[i].type));
        }

        // Call the real function and box the result back to i8*.
        Value* result = Builder.CreateCall(realFn, realArgs);
        if (funcNode->returnType == "void") {
            Builder.CreateRet(ConstantPointerNull::get(cast<PointerType>(i8PtrTy)));
        } else {
            Builder.CreateRet(boxToVoidPtr(result));
        }

        if (savedBB) Builder.SetInsertPoint(savedBB);
        functionThunkCache[fnName] = thunk;
        return thunk;
    }

    // Allocate a Callable struct {thunk, null} pointing at the function `fnName`.
    // The struct is freshly allocated at every use site (cheap — 16 bytes).
    // Returns nullptr if `fnName` isn't a known user-defined function.
    Value* emitFunctionAsCallable(const std::string& fnName) {
        Function* thunk = getOrCreateFunctionThunk(fnName);
        if (!thunk) return nullptr;
        Type* i8PtrTy = Type::getInt8PtrTy(Context);
        StructType* callableTy = StructTypes["Callable"];

        FunctionCallee gcMallocFn = TheModule->getOrInsertFunction(
            "GC_malloc", i8PtrTy, Type::getInt64Ty(Context));
        uint64_t callSz = TheModule->getDataLayout().getTypeAllocSize(callableTy);
        Value* raw = Builder.CreateCall(
            gcMallocFn, {ConstantInt::get(Type::getInt64Ty(Context), callSz)},
            fnName + "_callable");
        Value* callablePtr = Builder.CreateBitCast(raw, PointerType::getUnqual(callableTy));

        Value* fnField  = Builder.CreateStructGEP(callableTy, callablePtr, 0);
        Value* envField = Builder.CreateStructGEP(callableTy, callablePtr, 1);
        Builder.CreateStore(Builder.CreateBitCast(thunk, i8PtrTy), fnField);
        Builder.CreateStore(ConstantPointerNull::get(cast<PointerType>(i8PtrTy)), envField);
        return callablePtr;
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
                // Module-level globals are NOT captured — the lambda reads
                // and writes them directly through their GlobalVariable so
                // every invocation sees the current value. Capturing them
                // here would snapshot the value at lambda-creation time.
                if (varGen->isGlobal(name)) return;
                if (varGen->exists(name)) { seen.insert(name); freeVars.push_back(name); }
                return;
            }
            if (auto b = dynamic_cast<BinaryOpNode*>(n)) { walk(b->left.get()); walk(b->right.get()); }
            else if (auto c = dynamic_cast<CallNode*>(n)) {
                walk(c->callee.get());
                for (auto& a : c->args) walk(a.value.get());
            }
            else if (auto m = dynamic_cast<MemberAccessNode*>(n)) { walk(m->object.get()); }
            else if (auto v = dynamic_cast<VarDeclNode*>(n)) {
                // Always walk the RHS expression. For reassignment
                // (`x = expr`, op="="), also walk the LHS so a
                // write-only nonlocal reference is captured —
                // `fn() => { nonlocal x; x = 7 }` used to silently
                // create a fresh local because `x` only appeared as
                // an assignment target and the walker never saw it.
                // For `:=` declarations the LHS is a brand-new local
                // binding and doesn't need capture.
                walk(v->expression.get());
                if (v->op == "=") walk(v->lhs.get());
            }
            else if (auto r = dynamic_cast<ReturnNode*>(n)) { walk(r->expression.get()); }
            else if (auto i = dynamic_cast<IfNode*>(n)) {
                walk(i->condition.get());
                for (auto& s : i->thenBranch) walk(s.get());
                for (auto& eb : i->elIfBranches) {
                    walk(eb.condition.get());
                    for (auto& s : eb.body) walk(s.get());
                }
                for (auto& s : i->elseBranch) walk(s.get());
            }
            // Loops inside a lambda body need their inner references picked
            // up too — without this, `fn() => { while cond { it() } }` lost
            // the capture of outer-scope `it` and `cond`.
            else if (auto w = dynamic_cast<WhileNode*>(n)) {
                walk(w->condition.get());
                for (auto& s : w->body) walk(s.get());
            }
            else if (auto f = dynamic_cast<ForNode*>(n)) {
                walk(f->iterable.get());
                for (auto& s : f->body) walk(s.get());
            }
            else if (auto m = dynamic_cast<MatchNode*>(n)) {
                walk(m->scrutinee.get());
                for (auto& arm : m->arms) {
                    for (auto& p : arm.patterns) walk(p.get());
                    if (arm.guard) walk(arm.guard.get());
                    for (auto& s : arm.body) walk(s.get());
                }
            }
            else if (auto t = dynamic_cast<TryCatchNode*>(n)) {
                for (auto& s : t->tryBlock) walk(s.get());
                for (auto& cb : t->catchBlocks) for (auto& s : cb.body) walk(s.get());
                for (auto& s : t->finallyBlock) walk(s.get());
            }
            else if (auto th = dynamic_cast<ThrowNode*>(n)) {
                if (th->expression) walk(th->expression.get());
                if (th->cause) walk(th->cause.get());
            }
            else if (auto tup = dynamic_cast<TupleLiteralNode*>(n)) {
                for (auto& e : tup->elements) walk(e.get());
            }
            else if (auto lst = dynamic_cast<ListLiteralNode*>(n)) {
                for (auto& e : lst->elements) walk(e.get());
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
        // For nonlocal (cell-boxed) vars, capture the raw cell i8* so the lambda shares mutation.
        std::vector<Value*> captureValues;
        std::vector<Type*>  captureTypes;
        std::vector<bool>   captureIsCell;
        Type* i8PtrTy_ = Type::getInt8PtrTy(Context);
        for (const auto& name : captureNames) {
            if (varGen->isNonlocal(name)) {
                Value* cellPtr = varGen->getCellPtr(name);
                if (cellPtr) {
                    captureValues.push_back(cellPtr);
                    captureTypes.push_back(i8PtrTy_);
                    captureIsCell.push_back(true);
                }
            } else {
                Value* val = varGen->resolveVariable(name);
                if (val) {
                    captureValues.push_back(val);
                    captureTypes.push_back(val->getType());
                    captureIsCell.push_back(false);
                }
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
        auto savedVars       = varGen->snapshot();
        auto savedNonlocal   = varGen->snapshotNonlocal();
        BasicBlock* savedBB  = Builder.GetInsertBlock();
        std::string savedClass = currentCodegenClass;

        // Switch into the lambda function
        Builder.SetInsertPoint(entryBB);
        varGen->clear();
        // varEnumTypes tracks `s := SomeEnum(...)` bindings so `s.value`
        // codegens via the enum-accessor path. Clearing alongside
        // varGen avoids leaks across function boundaries — e.g. a
        // top-level `s := Status(404)` from one user file used to
        // make `s.value` in a library helper (Option.unwrap_or's
        // `case Some as s => return s.value`) route into
        // quirk_enum_value_int with a Some* receiver and crash IR.
        varEnumTypes.clear();

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
                if (i < captureIsCell.size() && captureIsCell[i]) {
                    // Restore nonlocal cell — lambda shares the heap cell with the outer scope
                    varGen->defineNonlocalCell(captureNames[i], loaded);
                } else {
                    varGen->defineArgument(captureNames[i], loaded);
                }
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

        // Restore outer state (including nonlocal cell metadata)
        Builder.SetInsertPoint(savedBB);
        varGen->restoreWithNonlocal(savedVars, savedNonlocal);
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
        if (!node->alias.empty()) {
            // Explicit alias: from .path as alias
            activeModuleAliases[node->alias] = node->moduleName;
        } else if (node->filterList.empty()) {
            // Derive alias from last path component
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
                // User-defined function with the same name overrides the builtin
                Function* override = resolveFunction(lit->value, currentCodegenClass);
                if (!override)
                    return builtinGen->handleBuiltin(lit->value, call, [this](Node* n) { return this->handleExpression(n); });
                // Fall through to normal function dispatch below
            }

            // Backed-enum value lookup: `Gender("Male")` lowers to a
            // runtime helper call against the packed values blob. Sema
            // already validated arity and argument type, so codegen just
            // wires the IR. Result is the ordinal (i32), which is the
            // same shape as `Gender.Male` — both flow through the rest
            // of codegen identically.
            if (backedEnums.count(lit->value) && !call->args.empty()) {
                const BackedEnumInfo& info = backedEnums[lit->value];
                Value* query = handleExpression(call->args[0].value.get());
                Type* i32 = Type::getInt32Ty(Context);
                Type* i8p = Type::getInt8PtrTy(Context);
                GlobalVariable* packedGv = TheModule->getNamedGlobal("__" + lit->value + "_packed");
                GlobalVariable* nameGv   = TheModule->getNamedGlobal("__" + lit->value + "_name");
                Value* packedPtr = Builder.CreateBitCast(packedGv, i8p);
                Value* namePtr   = Builder.CreateBitCast(nameGv,   i8p);
                Value* count     = ConstantInt::get(i32, info.values.size());

                if (info.backingType == "Int") {
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_lookup_int",
                        FunctionType::get(i32, {i32, i8p, i32, i8p}, false));
                    // Coerce a boxed-int (i8*) query to i32 if needed.
                    if (query->getType()->isPointerTy())
                        query = Builder.CreatePtrToInt(query, i32);
                    return Builder.CreateCall(fn, {query, packedPtr, count, namePtr});
                }
                if (info.backingType == "Double") {
                    Type* dbl = Type::getDoubleTy(Context);
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_lookup_double",
                        FunctionType::get(i32, {dbl, PointerType::getUnqual(dbl), i32, i8p}, false));
                    // Coerce Int → Double if the user passed an int literal.
                    if (query->getType()->isIntegerTy())
                        query = Builder.CreateSIToFP(query, dbl);
                    Value* packedD = Builder.CreateBitCast(packedGv, PointerType::getUnqual(dbl));
                    return Builder.CreateCall(fn, {query, packedD, count, namePtr});
                }
                // String backing
                FunctionCallee fn = TheModule->getOrInsertFunction(
                    "quirk_enum_lookup_str",
                    FunctionType::get(i32,
                        {PointerType::getUnqual(StructTypes["String"]), i8p, i32, i8p}, false));
                // String literals come in as String*; opaque i8* (e.g.
                // a Callable return) gets unboxed first.
                if (query->getType()->isPointerTy() &&
                    query->getType()->getPointerElementType()->isIntegerTy(8)) {
                    Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                    if (!opaqueToStr) {
                        FunctionType* ft = FunctionType::get(
                            PointerType::getUnqual(StructTypes["String"]), {i8p}, false);
                        opaqueToStr = Function::Create(ft, Function::ExternalLinkage,
                                                       "quirk_opaque_to_string", TheModule.get());
                    }
                    query = Builder.CreateCall(opaqueToStr, {query});
                }
                return Builder.CreateCall(fn, {query, packedPtr, count, namePtr});
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

            // v3.6.0: cross-package overload disambiguation. When two
            // packages export the same top-level function name (e.g.
            // `html.input` and `console.input`), Sema's `lookupTopLevel`
            // picks the right candidate at type-check time and stamps
            // its linkage name onto the CallNode. Codegen's
            // `functionDeclarations` map is last-write-wins by name —
            // honour Sema's choice when present, fall back to the
            // historical name-based lookup otherwise.
            Function* func = nullptr;
            if (!call->resolvedLinkageName.empty()) {
                func = TheModule->getFunction(call->resolvedLinkageName);
            }
            if (!func) func = resolveFunction(lit->value, currentCodegenClass);
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

                        // Bundle variadic tail into a List. Elements need to
                        // survive a round-trip through the i8* List slots
                        // and be readable by `quirk_opaque_to_string` at the
                        // other end — that requires an Any* wrap (not a raw
                        // bit-reinterpretation, which would crash for double
                        // and lose type info for bool). emitBox wraps each
                        // value in the right Any_box_* variant.
                        int varStart = variadicCallableStart.count(lit->value)
                                       ? variadicCallableStart.at(lit->value) : -1;
                        if (varStart >= 0 && (int)argVals.size() > varStart) {
                            std::vector<Value*> tail;
                            for (size_t i = (size_t)varStart; i < argVals.size(); i++)
                                tail.push_back(emitBox(argVals[i]));
                            argVals.erase(argVals.begin() + varStart, argVals.end());
                            Value* bundled = structGen->createListFromValues(tail);
                            argVals.push_back(bundled);
                        }

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

            // v2.3.1+: enum accessor methods. Recognised here BEFORE
            // the lit/object literal check, so chained shapes like
            // `Gender.Male.ordinal()` work alongside `g.ordinal()`.
            // Codegen reuses the existing MemberAccess expression
            // handlers for the property cases — we just evaluate
            // the callee as if it were a bare property access.
            // `name()` has no MemberAccess handler so it's
            // synthesised inline via the existing `__<Enum>_str`.
            {
                static const std::set<std::string> enumAccessors = {
                    "value", "name", "ordinal",
                    "values", "names", "variants"
                };
                if (call->args.empty() && enumAccessors.count(member->memberName)) {
                    if (member->memberName == "name") {
                        std::string enumName;
                        if (auto* nameLit = dynamic_cast<LiteralNode*>(member->object.get())) {
                            auto it = varEnumTypes.find(nameLit->value);
                            if (it != varEnumTypes.end()) enumName = it->second;
                            else if (enumVariants.count(nameLit->value)) enumName = nameLit->value;
                        } else if (auto* innerMem = dynamic_cast<MemberAccessNode*>(member->object.get())) {
                            if (auto* innerLit = dynamic_cast<LiteralNode*>(innerMem->object.get())) {
                                if (enumVariants.count(innerLit->value)) enumName = innerLit->value;
                            }
                        }
                        if (!enumName.empty()) {
                            Value* ordinal = handleExpression(member->object.get());
                            if (ordinal->getType()->isPointerTy())
                                ordinal = Builder.CreatePtrToInt(ordinal, Type::getInt32Ty(Context));
                            Function* strFn = TheModule->getFunction("__" + enumName + "_str");
                            if (strFn) return Builder.CreateCall(strFn, {ordinal});
                        }
                    }
                    return handleExpression(call->callee.get());
                }
            }

            if (auto lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                if (verbose) std::cerr << "[Codegen]     handleCall: " << lit->value << "." << member->memberName << "\n";

                // `EnumName.parse(value)` — safe lookup. Returns
                // i8* (boxed-int Any on hit, null on miss). Sema
                // typed this as `EnumName?` so the caller's `case
                // null` / `??` machinery picks it up correctly.
                if (member->memberName == "parse" && backedEnums.count(lit->value)
                                                 && !call->args.empty()) {
                    const BackedEnumInfo& info = backedEnums[lit->value];
                    Value* query = handleExpression(call->args[0].value.get());
                    Type* i32 = Type::getInt32Ty(Context);
                    Type* i8p = Type::getInt8PtrTy(Context);
                    GlobalVariable* packedGv = TheModule->getNamedGlobal("__" + lit->value + "_packed");
                    Value* count = ConstantInt::get(i32, info.values.size());

                    if (info.backingType == "Int") {
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_parse_int",
                            FunctionType::get(i8p, {i32, PointerType::getUnqual(i32), i32}, false));
                        if (query->getType()->isPointerTy())
                            query = Builder.CreatePtrToInt(query, i32);
                        Value* packedPtr = Builder.CreateBitCast(packedGv, PointerType::getUnqual(i32));
                        return Builder.CreateCall(fn, {query, packedPtr, count});
                    }
                    if (info.backingType == "Double") {
                        Type* dbl = Type::getDoubleTy(Context);
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_parse_double",
                            FunctionType::get(i8p, {dbl, PointerType::getUnqual(dbl), i32}, false));
                        if (query->getType()->isIntegerTy())
                            query = Builder.CreateSIToFP(query, dbl);
                        Value* packedPtr = Builder.CreateBitCast(packedGv, PointerType::getUnqual(dbl));
                        return Builder.CreateCall(fn, {query, packedPtr, count});
                    }
                    // String backing
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_parse_str",
                        FunctionType::get(i8p,
                            {PointerType::getUnqual(StructTypes["String"]),
                             i8p, i32}, false));
                    // Unbox opaque i8* → String* if needed.
                    if (query->getType()->isPointerTy() &&
                        query->getType()->getPointerElementType()->isIntegerTy(8)) {
                        Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                        if (!opaqueToStr) {
                            FunctionType* ft = FunctionType::get(
                                PointerType::getUnqual(StructTypes["String"]), {i8p}, false);
                            opaqueToStr = Function::Create(ft, Function::ExternalLinkage,
                                                           "quirk_opaque_to_string", TheModule.get());
                        }
                        query = Builder.CreateCall(opaqueToStr, {query});
                    }
                    Value* packedPtr = Builder.CreateBitCast(packedGv, i8p);
                    return Builder.CreateCall(fn, {query, packedPtr, count});
                }

                // Module-call routing must yield to local-variable scope.
                // Without this, importing a module whose name shadows a
                // function parameter (e.g. `use prompt` in a script that
                // also calls `console.input(prompt: String)`) makes every
                // `prompt.length()` inside that function route to "the
                // prompt module's length()" — there is no such function,
                // and the user gets a confusing "Unknown function" error.
                // Locals always win.
                bool litIsLocal = varGen->exists(lit->value);
                if (!litIsLocal && activeModuleAliases.count(lit->value)) {
                    std::string memberName = member->memberName;
                    if (StructTypes.count(memberName)) {
                        std::vector<Value*> args;
                        for (auto& a : call->args) args.push_back(handleExpression(a.value.get()));
                        return structGen->allocateAndInit(memberName, args);
                    }

                    // Disambiguate via the module index first — if `csv.write`
                    // and `audio.write` both exist, the bare-name lookup picks
                    // whichever was registered last. Module-keyed lookup picks
                    // the right one.
                    Function* func = nullptr;
                    const std::string& modName = activeModuleAliases[lit->value];
                    auto mit = moduleFunctionIndex.find({modName, memberName});
                    if (mit != moduleFunctionIndex.end()) {
                        const std::string& ln = mit->second->linkageName;
                        func = TheModule->getFunction(ln.empty() ? memberName : ln);
                    }
                    if (!func) func = resolveFunction(memberName);
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

            // Safe call: obj?.method(args) — skip the call if obj is null
            if (member->isSafeAccess && objPtr && objPtr->getType()->isPointerTy()) {
                Function* parentFunc = Builder.GetInsertBlock()->getParent();
                BasicBlock* preBB    = Builder.GetInsertBlock();
                BasicBlock* doCallBB = BasicBlock::Create(Context, "safe_call", parentFunc);
                BasicBlock* afterBB  = BasicBlock::Create(Context, "safe_after");
                Builder.CreateCondBr(
                    Builder.CreateICmpEQ(objPtr, Constant::getNullValue(objPtr->getType()), "safe_isnull"),
                    afterBB, doCallBB);

                // Execute the actual call in doCallBB
                Builder.SetInsertPoint(doCallBB);
                member->isSafeAccess = false;
                Value* res = handleCall(call);
                member->isSafeAccess = true;
                BasicBlock* callEndBB = Builder.GetInsertBlock();
                if (!callEndBB->getTerminator()) Builder.CreateBr(afterBB);

                // Merge results via PHI node — preserves actual return type
                parentFunc->getBasicBlockList().push_back(afterBB);
                Builder.SetInsertPoint(afterBB);
                if (res && !res->getType()->isVoidTy()) {
                    PHINode* phi = Builder.CreatePHI(res->getType(), 2, "safe_result");
                    phi->addIncoming(Constant::getNullValue(res->getType()), preBB);
                    phi->addIncoming(res, callEndBB);
                    return phi;
                }
                return nullptr;
            }

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
                    // i8* is a boxed list element (String* or tagged int cast to void*); unbox via quirk_opaque_to_string
                    Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                    if (!opaqueToStr) {
                        Type* retTy = StructTypes.count("String")
                            ? (Type*)PointerType::getUnqual(StructTypes["String"])
                            : (Type*)Type::getInt8PtrTy(Context);
                        FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                        opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule.get());
                    }
                    objPtr = Builder.CreateCall(opaqueToStr, {objPtr}, "deboxed_str");
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
            // as extern in list.quirk so resolveFunction returns the correct linkage name).
            // We only intercept here to evaluate the Callable* argument correctly.
            static const std::set<std::string> listFunctionalMethods = {
                "map", "filter", "each", "reduce", "any", "all", "find"
            };
            if (objPtr && listFunctionalMethods.count(member->memberName) &&
                typeName.find("List") != std::string::npos && !call->args.empty()) {
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

            // Fallback: try triple-underscore operator convention
            // (e.g. List___get, Map___get). This form arises when the
            // method's source name was a dunder like `__get` and the
            // parser mangled it to `<Type>___get` (struct + `_` +
            // `__get` = 3 underscores). Restrict the fallback to
            // member names that *already* start with `_` — without
            // the gate, `.add(...)` happily matches `Set___add` (a
            // user-defined `__add` magic method), and the call's
            // (self, Int) args get jammed into the magic method's
            // (self, Set) signature — Int-as-Set crashes at runtime.
            if (!func && !member->memberName.empty() && member->memberName[0] == '_') {
                func = TheModule->getFunction(typeName + "___" + member->memberName);
            }

            // Constructor fallback: super().__init(v)
            // Parser stores __init under "TypeName__init" (no separator), while
            // the general method-dispatch path produces "TypeName___init" (with _).
            // resolveFunction consults functionDeclarations + linkageName so it
            // correctly reaches extern implementations like Core_String_String___init.
            if (!func && member->memberName == "__init") {
                func = resolveFunction(typeName + "__init");   // user-defined parent path
                if (!func) func = resolveFunction(typeName + "___init"); // extern parent fallback
            }

            // Walk inheritance chain for inherited methods (user-defined parents only;
            // extern parents are resolved via resolveFunction below).
            if (!func && structHierarchy.count(typeName)) {
                auto searchHierarchy = [&](const std::string& currentType, auto& self) -> Function* {
                    auto hit = structHierarchy.find(currentType);
                    if (hit == structHierarchy.end()) return nullptr;
                    for (const std::string& parentName : hit->second) {
                        Function* foundFunc = TheModule->getFunction(parentName + "_" + member->memberName);
                        if (!foundFunc) foundFunc = TheModule->getFunction(parentName + "___" + member->memberName);
                        if (!foundFunc) foundFunc = resolveFunction(parentName + "_"   + member->memberName);
                        if (!foundFunc) foundFunc = resolveFunction(parentName + "___" + member->memberName);
                        if (!foundFunc && member->memberName == "__init") {
                            foundFunc = resolveFunction(parentName + "__init");
                            if (!foundFunc) foundFunc = resolveFunction(parentName + "___init");
                        }
                        if (foundFunc) return foundFunc;
                        foundFunc = self(parentName, self);
                        if (foundFunc) return foundFunc;
                    }
                    return nullptr;
                };
                func = searchHierarchy(typeName, searchHierarchy);
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

            // --- VIRTUAL DISPATCH ---
            // When a method is overridden in a subclass and the receiver is a vtable-eligible
            // struct, generate a switch on __type_id (field 0) instead of a direct call.
            // super() calls are always static — skip dispatch for those.
            {
                bool isSuperCall = false;
                if (auto callExpr = dynamic_cast<CallNode*>(member->object.get()))
                    if (auto lit2 = dynamic_cast<LiteralNode*>(callExpr->callee.get()))
                        isSuperCall = (lit2->value == "super");

                const std::vector<std::pair<std::string,int>>* overridesPtr = nullptr;
                if (!isSuperCall && objPtr && structGen->isVtableEligible(typeName))
                    overridesPtr = structGen->getOverrides(typeName, member->memberName);
                if (overridesPtr) {
                    const auto& overrides = *overridesPtr;

                    // Pre-evaluate non-self arguments once before the switch.
                    std::vector<Value*> rawArgs;
                    for (const auto& cArg : call->args)
                        rawArgs.push_back(handleExpression(cArg.value.get()));

                    // Load __type_id from field 0 of the receiver.
                    StructType* recvST = cast<StructType>(objPtr->getType()->getPointerElementType());
                    Value* tidPtr = Builder.CreateStructGEP(recvST, objPtr, 0, "tid_ptr");
                    Value* tid    = Builder.CreateLoad(Type::getInt32Ty(Context), tidPtr, "tid");

                    Function* curFn   = Builder.GetInsertBlock()->getParent();
                    Type*     retTy   = func->getReturnType();
                    bool      isVoid  = retTy->isVoidTy();

                    BasicBlock* defaultBB = BasicBlock::Create(Context, "vd_def", curFn);
                    BasicBlock* mergeBB   = BasicBlock::Create(Context, "vd_merge", curFn);
                    SwitchInst* sw = Builder.CreateSwitch(tid, defaultBB, (unsigned)overrides.size());

                    std::vector<std::pair<BasicBlock*, Value*>> phiInputs;

                    // Helper to coerce a pre-computed arg to the expected LLVM type.
                    auto coerce = [&](Value* v, Type* expected) -> Value* {
                        if (v->getType() == expected) return v;
                        if (v->getType()->isPointerTy() && expected->isPointerTy())
                            return Builder.CreateBitCast(v, expected);
                        if (v->getType()->isIntegerTy() && expected->isIntegerTy())
                            return Builder.CreateIntCast(v, expected, true);
                        if (v->getType()->isIntegerTy() && expected->isDoubleTy())
                            return Builder.CreateSIToFP(v, expected);
                        return v;
                    };

                    for (const auto& [subName, subId] : overrides) {
                        const std::string& mn = member->memberName;
                        std::string subFnName = (mn.size() >= 2 && mn[0] == '_' && mn[1] == '_')
                            ? subName + mn : subName + "_" + mn;
                        Function* subFn = TheModule->getFunction(subFnName);
                        if (!subFn) subFn = resolveFunction(subFnName);
                        if (!subFn) continue;

                        BasicBlock* caseBB = BasicBlock::Create(Context, "vd_" + subName, curFn);
                        sw->addCase(ConstantInt::get(Type::getInt32Ty(Context), subId), caseBB);
                        Builder.SetInsertPoint(caseBB);

                        Value* castSelf = objPtr;
                        if (StructTypes.count(subName)) {
                            Type* subPtrTy = PointerType::getUnqual(StructTypes[subName]);
                            if (castSelf->getType() != subPtrTy)
                                castSelf = Builder.CreateBitCast(castSelf, subPtrTy, "as_" + subName);
                        }
                        std::vector<Value*> subArgs = {castSelf};
                        for (size_t i = 0; i < rawArgs.size(); i++) {
                            size_t pi = i + 1;
                            subArgs.push_back(pi < subFn->arg_size()
                                ? coerce(rawArgs[i], subFn->getFunctionType()->getParamType(pi))
                                : rawArgs[i]);
                        }
                        Value* subResult = Builder.CreateCall(subFn, subArgs, isVoid ? "" : "vd_r");
                        Builder.CreateBr(mergeBB);
                        if (!isVoid) phiInputs.push_back({caseBB, subResult});
                    }

                    // Default: static dispatch to the statically resolved method.
                    Builder.SetInsertPoint(defaultBB);
                    Value* defSelf = objPtr;
                    if (func->arg_size() > 0) {
                        Type* expSelf = func->getFunctionType()->getParamType(0);
                        if (defSelf->getType() != expSelf &&
                            defSelf->getType()->isPointerTy() && expSelf->isPointerTy())
                            defSelf = Builder.CreateBitCast(defSelf, expSelf, "as_parent");
                    }
                    std::vector<Value*> defArgs = {defSelf};
                    for (size_t i = 0; i < rawArgs.size(); i++) {
                        size_t pi = i + 1;
                        defArgs.push_back(pi < func->arg_size()
                            ? coerce(rawArgs[i], func->getFunctionType()->getParamType(pi))
                            : rawArgs[i]);
                    }
                    Value* defResult = Builder.CreateCall(func, defArgs, isVoid ? "" : "vd_def_r");
                    Builder.CreateBr(mergeBB);

                    Builder.SetInsertPoint(mergeBB);
                    if (!isVoid) {
                        PHINode* phi = Builder.CreatePHI(retTy, (unsigned)(phiInputs.size() + 1), "vd_phi");
                        for (auto& [bb, v] : phiInputs) phi->addIncoming(v, bb);
                        phi->addIncoming(defResult, defaultBB);
                        if (phi->getType()->isIntegerTy(32) &&
                            externBoolReturnFunctions.count(func->getName().str()))
                            return Builder.CreateTrunc(phi, Type::getInt1Ty(Context));
                        return phi;
                    }
                    return nullptr;
                }
            }
            // --- END VIRTUAL DISPATCH ---

            std::vector<Value*> args;
            if (objPtr) {
                // --- FIX: Safely cast 'self' to the parent class type if calling an inherited method ---
                if (func->arg_size() > 0) {
                    Type* expectedSelfType = func->getFunctionType()->getParamType(0);
                    if (objPtr->getType() != expectedSelfType) {
                        if (objPtr->getType()->isPointerTy() && expectedSelfType->isPointerTy()) {
                            objPtr = Builder.CreateBitCast(objPtr, expectedSelfType);
                        } else if (objPtr->getType()->isIntegerTy(1) && expectedSelfType->isIntegerTy(32)) {
                            // Bool self widening: C ABI passes Bool as int (i32)
                            objPtr = Builder.CreateZExt(objPtr, expectedSelfType);
                        }
                    }
                }
                args.push_back(objPtr);
            }

            processCallArgs(func, call->args, args, (objPtr ? 1 : 0));
            Value* result = Builder.CreateCall(func, args);
            // Extern Bool-returning methods are widened to i32 for C ABI — truncate back to i1.
            if (result->getType()->isIntegerTy(32) &&
                externBoolReturnFunctions.count(func->getName().str()))
                return Builder.CreateTrunc(result, Type::getInt1Ty(Context));
            return result;
        }

        // Fallback: callee is an arbitrary expression that hopefully evaluates
        // to a Callable. Handles the `<expr>(args)` case where <expr> is itself
        // a call (e.g. `wrap(double)(5)`), a member access yielding a Callable,
        // or anything else producing a Callable* at runtime.
        if (Value* calleeVal = handleExpression(call->callee.get())) {
            Type* i8PtrTy = Type::getInt8PtrTy(Context);
            // A Callable returned from another Callable call comes back boxed
            // as `i8*` (the Callable ABI returns i8*). Bitcast to Callable*
            // so we can dispatch. Without this, `make_adder()(id_fn)(5)` —
            // where the middle call returns a Callable — silently no-ops.
            if (calleeVal->getType() == i8PtrTy && StructTypes.count("Callable")) {
                calleeVal = Builder.CreateBitCast(
                    calleeVal,
                    PointerType::getUnqual(StructTypes["Callable"]),
                    "boxed_callable");
            }
            if (calleeVal->getType()->isPointerTy()
             && calleeVal->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(calleeVal->getType()->getPointerElementType());
                if (st == StructTypes["Callable"]) {
                    std::vector<Value*> argVals;
                    for (auto& a : call->args) argVals.push_back(handleExpression(a.value.get()));
                    return emitCallableCall(calleeVal, argVals);
                }
            }
        }
        return nullptr;
    }
    
    Value* generateGlobalCall(Function* func, CallNode* call) {
        std::vector<Value*> finalArgs;
        processCallArgs(func, call->args, finalArgs, 0);
        Value* result = Builder.CreateCall(func, finalArgs);
        // Extern Bool-returning functions are widened to i32 for C ABI.
        // Truncate back to i1 so the rest of codegen (print, comparisons, etc.) sees Bool.
        if (result->getType()->isIntegerTy(32) &&
            externBoolReturnFunctions.count(func->getName().str()))
            return Builder.CreateTrunc(result, Type::getInt1Ty(Context));
        return result;
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

        // Find which LLVM param index is the variadic List slot.
        // Use fixedArgCount as a sentinel meaning "no variadic slot" for non-variadic functions.
        size_t variadicSlot = fixedArgCount;  // sentinel: out-of-range
        if (isVariadic) {
            // Default: last param (backward-compat for functions where *args is last).
            variadicSlot = fixedArgCount > 0 ? fixedArgCount - 1 : 0;
            if (funcNode) {
                size_t astOffset = offset;
                for (size_t pi = 0; pi < funcNode->parameters.size(); pi++) {
                    if (funcNode->parameters[pi].isVariadic) {
                        variadicSlot = pi + astOffset;
                        break;
                    }
                }
            }
        }
        size_t requiredFixedCount = isVariadic ? (fixedArgCount - 1) : fixedArgCount;

        std::vector<Value*> matchedArgs(fixedArgCount, nullptr);
        std::vector<Value*> variadicBundle;

        size_t positionalIdx = offset;
        bool hasSeenNamedArg = false;

        // Check for a single spread arg `...list` targeting a variadic function
        // In this case pass the list directly as the variadic List parameter.
        bool usedSpreadDirect = false;
        if (isVariadic && astArgs.size() == 1 && astArgs[0].isSpread) {
            Value* spreadList = handleExpression(astArgs[0].value.get());
            if (spreadList) {
                // Fill any fixed args before the variadic slot with nullptr (no positional args)
                for (size_t i = offset; i < requiredFixedCount; i++)
                    finalArgs.push_back(Constant::getNullValue(func->getFunctionType()->getParamType(i)));
                finalArgs.push_back(spreadList);
                usedSpreadDirect = true;
            }
        }
        if (usedSpreadDirect) return;

        for (const auto& arg : astArgs) {
            if (arg.isSpread) {
                // Spread into variadic bundle: treat the list as individual elements
                // For now just push the whole list value (it IS a List already)
                variadicBundle.push_back(handleExpression(arg.value.get()));
                continue;
            }
            if (!arg.name.empty()) {
                hasSeenNamedArg = true;
                bool found = false;
                if (funcNode) {
                    for (size_t i = offset; i < fixedArgCount; ++i) {
                        if (i == variadicSlot) continue;  // skip the variadic List slot
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
                // Positional args fill fixed slots that appear BEFORE variadicSlot in the
                // parameter list; everything else (including overflow) goes to the bundle.
                // This mirrors Python semantics: def f(*args, sep=" ") — all positional → *args.
                if (isVariadic && hasSeenNamedArg) {
                    variadicBundle.push_back(handleExpression(arg.value.get()));
                } else {
                    while (positionalIdx < variadicSlot && matchedArgs[positionalIdx] != nullptr)
                        positionalIdx++;
                    if (positionalIdx < variadicSlot) {
                        matchedArgs[positionalIdx] = handleExpression(arg.value.get());
                        positionalIdx++;
                    } else {
                        variadicBundle.push_back(handleExpression(arg.value.get()));
                    }
                }
            }
        }

        // Build the variadic List now so we can place it at the correct slot position.
        Value* variadicList = nullptr;
        if (isVariadic) {
            std::vector<Value*> castedVariadic;
            for (Value* vArg : variadicBundle) {
                Type* ty = vArg->getType();
                if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                    StructType* st = cast<StructType>(ty->getPointerElementType());
                    if (st->getName().str().find("String") == std::string::npos) {
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        Function* strFunc = TheModule->getFunction(sName + "___str");
                        if (strFunc) {
                            vArg = Builder.CreateCall(strFunc, {vArg});
                            if (vArg->getType()->isPointerTy() &&
                                vArg->getType()->getPointerElementType()->isIntegerTy(8)) {
                                std::vector<Value*> boxArgs = {vArg};
                                vArg = structGen->allocateAndInit("String", boxArgs);
                            }
                        }
                    }
                }
                // Delegate to the canonical boxer — it knows how to handle
                // i1, integers (IntToPtr), doubles (bitcast→i64→inttoptr),
                // and pointers. Previous hand-rolled chain crashed LLVM
                // with `Invalid bitcast: i8* bitcast (double ... to i8*)`
                // because doubles aren't pointer-sized and bitcast can't
                // bridge across non-pointer types.
                vArg = boxToVoidPtr(vArg);
                castedVariadic.push_back(vArg);
            }
            variadicList = structGen->createListFromValues(castedVariadic);
        }

        // Emit each LLVM param slot in order (offset..fixedArgCount).
        // The variadic slot gets the pre-built List; other slots get matched/default values.
        for (size_t i = offset; i < fixedArgCount; i++) {
            if (isVariadic && i == variadicSlot) {
                finalArgs.push_back(variadicList);
                continue;
            }

            size_t astIdx = i - offset;
            Value* argVal = matchedArgs[i];

            if (!argVal && funcNode && astIdx < funcNode->parameters.size() && funcNode->parameters[astIdx].defaultValue) {
                argVal = handleExpression(funcNode->parameters[astIdx].defaultValue.get());
            }

            if (!argVal) return;  // required arg missing

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
            // Tuple boxing for `Any`-typed extern params whose LLVM
            // signature uses `i8*` (v3.25.0). Without this, a Tuple
            // passed to `xs.append((1, "a"))` stores the raw Tuple*
            // and `quirk_opaque_to_string` can't classify it for
            // display.
            //
            // Pairs with `quirk_opaque_to_struct` (emitted by
            // `emitUnboxToType`) so the symmetric assignment
            // `p: Tuple = xs.get(0)` extracts `Any->ptr` back to a
            // genuine Tuple* — keeps itertools' raw-tuple storage
            // shape working alongside the new boxed-tuple shape.
            if (funcNode && astIdx < funcNode->parameters.size() &&
                funcNode->parameters[astIdx].type == "Any" &&
                expectedType == Type::getInt8PtrTy(Context) &&
                argVal->getType()->isPointerTy() &&
                argVal->getType()->getPointerElementType()->isStructTy()) {
                StructType* st = cast<StructType>(argVal->getType()->getPointerElementType());
                std::string sName = st->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                if (sName == "Tuple" || sName.rfind("Tuple.", 0) == 0) {
                    Value* asPtr = Builder.CreateBitCast(argVal, Type::getInt8PtrTy(Context));
                    Function* boxFn = TheModule->getFunction("Core_Primitives_Any_box_tuple");
                    if (boxFn) {
                        Value* boxed = Builder.CreateCall(boxFn, {asPtr});
                        finalArgs.push_back(boxed);
                        continue;
                    }
                }
            }

            if (argVal->getType()->isIntegerTy(1) && expectedType->isIntegerTy(32)) {
                argVal = Builder.CreateZExt(argVal, Type::getInt32Ty(Context));
            }

            if (argVal->getType()->isPointerTy() &&
                argVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                expectedType->isPointerTy() &&
                expectedType->getPointerElementType()->isStructTy()) {
                // i8* → SomeStruct*. The i8* may be a genuine struct
                // pointer cast down (List/Map/Callable flowing through
                // Any slots) or it may be an Any-laundered value: a
                // tagged-int (inttoptr small_int) or an Any* heap
                // wrapper. A raw bitcast is correct for the first
                // shape but disastrous for the others — passing
                // `Gender.Other` (i32=2) through an Any-typed param
                // into a `default: String` slot lands a String*
                // pointing at address 0x2 and segfaults on the next
                // method dispatch.
                //
                // For String* targets specifically, route through
                // quirk_opaque_to_string — the runtime helper sniffs
                // the three shapes and always returns a real String*.
                // Other struct targets keep the bitcast because the
                // helper only handles strings; their misuse pattern
                // is the same hazard but the fix needs separate
                // per-type helpers (out of scope here).
                StructType* st = cast<StructType>(expectedType->getPointerElementType());
                std::string sName = st->getName().str();
                if (sName.find("struct.") == 0) sName = sName.substr(7);
                if (sName == "String") {
                    // Use the …_or_null variant so a literal-null arg
                    // stays null at the callee, instead of being wrapped
                    // as the 4-char string "null" (which would defeat
                    // any `default != null` guard the callee has).
                    Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string_or_null");
                    if (!opaqueToStr) {
                        Type* retTy = PointerType::getUnqual(StructTypes["String"]);
                        FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                        opaqueToStr = Function::Create(ft, Function::ExternalLinkage,
                                                       "quirk_opaque_to_string_or_null", TheModule.get());
                    }
                    argVal = Builder.CreateCall(opaqueToStr, {argVal}, "arg_unbox_str");
                } else {
                    // For struct types that have a known Any tag (List, Map,
                    // Tuple, Callable), route through the generic unwrap helper.
                    // It returns null on null input, null on mismatched
                    // Any-laundered shapes, and the direct pointer otherwise —
                    // the same safety net the String path has had since 2.2.7.
                    // ANY_LIST=5, MAP=6, TUPLE=9, CALLABLE=10 in types.h.
                    int anyTag = -1;
                    if      (sName == "List")     anyTag = 5;
                    else if (sName == "Map")      anyTag = 6;
                    else if (sName == "Tuple")    anyTag = 9;
                    else if (sName == "Callable") anyTag = 10;
                    if (anyTag >= 0) {
                        Function* unwrap = TheModule->getFunction("quirk_opaque_unwrap_or_null");
                        if (!unwrap) {
                            FunctionType* ft = FunctionType::get(
                                Type::getInt8PtrTy(Context),
                                {Type::getInt8PtrTy(Context),
                                 Type::getInt32Ty(Context),
                                 Type::getInt8PtrTy(Context)},
                                false);
                            unwrap = Function::Create(ft, Function::ExternalLinkage,
                                                     "quirk_opaque_unwrap_or_null", TheModule.get());
                        }
                        Value* tagConst = ConstantInt::get(Type::getInt32Ty(Context), anyTag);
                        Value* nameStr  = Builder.CreateGlobalStringPtr(sName);
                        Value* unwrapped = Builder.CreateCall(unwrap, {argVal, tagConst, nameStr}, "arg_unbox_" + sName);
                        argVal = Builder.CreateBitCast(unwrapped, expectedType);
                    } else {
                        // Struct types without a dedicated AnyTag (Set,
                        // Queue, File, user structs). Route through the
                        // untagged-check helper so Any-laundered Ints /
                        // mismatched Any wraps throw a clean TypeError
                        // instead of crashing on the first dereference.
                        Function* checkFn = TheModule->getFunction("quirk_opaque_check_struct_or_null");
                        if (!checkFn) {
                            FunctionType* ft = FunctionType::get(
                                Type::getInt8PtrTy(Context),
                                {Type::getInt8PtrTy(Context),
                                 Type::getInt8PtrTy(Context)},
                                false);
                            checkFn = Function::Create(ft, Function::ExternalLinkage,
                                                       "quirk_opaque_check_struct_or_null", TheModule.get());
                        }
                        Value* nameStr = Builder.CreateGlobalStringPtr(sName);
                        Value* checked = Builder.CreateCall(checkFn, {argVal, nameStr}, "arg_check_" + sName);
                        argVal = Builder.CreateBitCast(checked, expectedType);
                    }
                }
            } else if (argVal->getType() != expectedType) {
                if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy())
                    argVal = Builder.CreateIntCast(argVal, expectedType, true);
                else if (argVal->getType()->isIntegerTy() && expectedType->isPointerTy())
                    argVal = quirk::boxIntToOpaque(Context, TheModule.get(), Builder, argVal, expectedType);
                else if (argVal->getType()->isPointerTy() && expectedType->isPointerTy())
                    argVal = Builder.CreateBitCast(argVal, expectedType);
                else if (argVal->getType()->isIntegerTy() && expectedType->isDoubleTy())
                    argVal = Builder.CreateSIToFP(argVal, expectedType);
                // Double → i8* (Any-typed param). Sema accepts the
                // call because Any takes anything, but Codegen needs
                // to actually box the Double or LLVM rejects the
                // `call ...(double %x)` against an `i8*` signature.
                // Route through `Core_Primitives_Any_box_double` —
                // matches what nonlocal-cell boxing does for the same
                // shape.
                else if (argVal->getType()->isDoubleTy() &&
                         expectedType == Type::getInt8PtrTy(Context)) {
                    FunctionCallee box = TheModule->getOrInsertFunction(
                        "Core_Primitives_Any_box_double",
                        Type::getInt8PtrTy(Context), Type::getDoubleTy(Context));
                    argVal = Builder.CreateCall(box, {argVal}, "arg_dbl_box");
                }
                else if (argVal->getType()->isDoubleTy() && expectedType->isIntegerTy())
                    argVal = Builder.CreateFPToSI(argVal, expectedType);
                // ptr → int: e.g. a `nonlocal i: Int` cell loads as i8*
                // but gets passed where an i32/i64 is expected
                // (List.get(i)). Route through quirk_opaque_to_int so
                // both encodings — Any* (Int 0/false/Bool false) and
                // legacy tagged-int (nonzero ints, list elements) —
                // resolve correctly.
                else if (argVal->getType()->isPointerTy() && expectedType->isIntegerTy()) {
                    Type* i8p = Type::getInt8PtrTy(Context);
                    FunctionCallee toInt = TheModule->getOrInsertFunction(
                        "quirk_opaque_to_int", Type::getInt32Ty(Context), i8p);
                    Value* casted = (argVal->getType() == i8p)
                        ? argVal
                        : Builder.CreateBitCast(argVal, i8p);
                    Value* asI32 = Builder.CreateCall(toInt, {casted}, "nonlocal_unbox");
                    argVal = expectedType->isIntegerTy(32)
                        ? asI32
                        : Builder.CreateIntCast(asI32, expectedType, /*isSigned=*/true);
                }
            }
            finalArgs.push_back(argVal);
        }
    }
    
    void handleStatement(Node* node, Function* parentFunc) {
        if (Builder.GetInsertBlock()->getTerminator()) return;

        // When --debug is on, give the runtime a chance to pause at each
        // statement. The hook is a no-op unless the user has armed step mode
        // or set a breakpoint, so the only overhead in debug builds is the
        // call itself. We skip Use/Nonlocal — they don't represent runnable
        // user code, and stopping on them would just be noise.
        if (debugMode && node->line > 0
            && !dynamic_cast<UseNode*>(node)
            && !dynamic_cast<NonlocalNode*>(node)) {
            FunctionCallee dbgHook = TheModule->getOrInsertFunction(
                "Debug_step_hook",
                Type::getVoidTy(Context),
                Type::getInt8PtrTy(Context),
                Type::getInt32Ty(Context));
            std::string fileStr = !node->filePath.empty() ? node->filePath
                               : !node->moduleName.empty() ? node->moduleName
                                                           : currentFilePath;
            if (fileStr.empty()) fileStr = "<unknown>";
            Value* fileVal = Builder.CreateGlobalStringPtr(fileStr);
            Builder.CreateCall(dbgHook,
                {fileVal, ConstantInt::get(Type::getInt32Ty(Context), node->line)});
        }

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
            if (dynamic_cast<MatchNode*>(node))       return "Match";
            if (dynamic_cast<WithNode*>(node))       return "With";
            if (dynamic_cast<UseNode*>(node))        return "Use";
            return "Unknown";
        };
        if (verbose) std::cerr << "[Codegen]   handleStatement: " << nodeTypeName() << "\n";

        if (auto u = dynamic_cast<UseNode*>(node)) handleUse(u);
        else if (auto nl = dynamic_cast<NonlocalNode*>(node)) {
            if (!nl->isGlobal) {
                // For each declared nonlocal var: box current value and move it to a GC heap cell.
                // Both this scope and any lambda that captures it will share the cell pointer.
                FunctionCallee gcMallocFn = TheModule->getOrInsertFunction(
                    "GC_malloc", Type::getInt8PtrTy(Context), Type::getInt64Ty(Context));
                for (const auto& varName : nl->vars) {
                    if (!varGen->exists(varName)) continue;
                    if (varGen->isNonlocal(varName)) continue; // already a heap cell
                    Value* curVal = varGen->resolveVariable(varName);
                    if (!curVal) continue;
                    // Box for the cell — routes Int/Double/Bool through
                    // Any* heap-box so 0/false aren't stored as NULL.
                    Value* boxed = varGen->boxForNonlocalCell(curVal);
                    // Allocate a single-pointer cell (8 bytes)
                    Value* cell = Builder.CreateCall(gcMallocFn,
                        {ConstantInt::get(Type::getInt64Ty(Context), 8)}, varName + "_cell");
                    // Store boxed value into the cell
                    Value* cellI8pp = Builder.CreateBitCast(cell,
                        PointerType::getUnqual(Type::getInt8PtrTy(Context)));
                    Builder.CreateStore(boxed, cellI8pp);
                    // Register as nonlocal cell — replaces the existing variable binding
                    varGen->defineNonlocalCell(varName, cell);
                }
            }
            // global: no-op for now (module-level mutable globals not yet supported)
        }
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
            // Sugar: `for v in EnumName` iterates the variants in
            // declaration order. Mirrors `for v in EnumName.variants`
            // exactly — same runtime helper, same lowering — just
            // saves the user the `.variants` suffix. Recognise the
            // bare-enum-name literal here and rewrite the iterable in
            // place to a MemberAccess so flowGen's normal path takes
            // over.
            if (auto* lit = dynamic_cast<LiteralNode*>(f->iterable.get())) {
                if (backedEnums.count(lit->value)) {
                    auto syntheticObj = std::make_unique<LiteralNode>(*lit);
                    auto memberAccess = std::make_unique<MemberAccessNode>(std::move(syntheticObj), "variants");
                    memberAccess->line = lit->line;
                    memberAccess->col  = lit->col;
                    memberAccess->filePath = lit->filePath;
                    f->iterable = std::move(memberAccess);
                }
            }

            // Special-case: range literal iterable → emit a simple counter loop
            if (auto* range = dynamic_cast<RangeLiteralNode*>(f->iterable.get())) {
                Value* startV = handleExpression(range->start.get());
                Value* endV   = handleExpression(range->end.get());
                if (!startV || !endV) return;
                auto toI32 = [&](Value* v) -> Value* {
                    if (v->getType()->isIntegerTy(32)) return v;
                    if (v->getType()->isIntegerTy()) return Builder.CreateSExt(v, Type::getInt32Ty(Context));
                    if (v->getType()->isPointerTy()) return Builder.CreatePtrToInt(v, Type::getInt32Ty(Context));
                    return v;
                };
                startV = toI32(startV); endV = toI32(endV);

                // Allocate loop counter
                Function* curFn = Builder.GetInsertBlock()->getParent();
                IRBuilder<> entryB(&curFn->getEntryBlock(), curFn->getEntryBlock().begin());
                Value* counterSlot = entryB.CreateAlloca(Type::getInt32Ty(Context), nullptr, f->varName + "_range");
                Builder.CreateStore(startV, counterSlot);

                BasicBlock* condBB  = BasicBlock::Create(Context, "range_cond",  curFn);
                BasicBlock* bodyBB  = BasicBlock::Create(Context, "range_body",  curFn);
                BasicBlock* afterBB = BasicBlock::Create(Context, "range_after", curFn);

                flowGen->breakStack.push_back(afterBB);
                flowGen->continueStack.push_back(condBB);

                Builder.CreateBr(condBB);
                Builder.SetInsertPoint(condBB);
                Value* counter = Builder.CreateLoad(Type::getInt32Ty(Context), counterSlot, f->varName);
                Value* cmp = Builder.CreateICmpSLT(counter, endV, "range_cmp");
                Builder.CreateCondBr(cmp, bodyBB, afterBB);

                Builder.SetInsertPoint(bodyBB);
                varGen->defineLocalVariable(f->varName, counter);
                for (auto& stmt : f->body) handleStatement(stmt.get(), curFn);
                if (!Builder.GetInsertBlock()->getTerminator()) {
                    Value* next = Builder.CreateAdd(
                        Builder.CreateLoad(Type::getInt32Ty(Context), counterSlot),
                        ConstantInt::get(Type::getInt32Ty(Context), 1), "range_next");
                    Builder.CreateStore(next, counterSlot);
                    Builder.CreateBr(condBB);
                }

                flowGen->breakStack.pop_back();
                flowGen->continueStack.pop_back();

                Builder.SetInsertPoint(afterBB);
                return;
            }

            flowGen->generateFor(
                f, parentFunc,
                [this](Node* n) { return this->handleExpression(n); },
                [this](const std::string& s, std::vector<Value*>& v) { return this->structGen->allocateAndInit(s, v); },
                [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); },
                varGen.get(),
                [this](const std::string& shortName) -> Function* {
                    if (Function* f = TheModule->getFunction(shortName)) return f;
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
        else if (auto mt = dynamic_cast<MatchNode*>(node)) {
            flowGen->generateMatch(mt, parentFunc,
                [this](Node* n) { return this->handleExpression(n); },
                [this, parentFunc](Node* n) { this->handleStatement(n, parentFunc); },
                [this](const std::string& name, Value* val) { varGen->defineLocalVariable(name, val); },
                [this](const std::string& typeName) -> int { return structGen->getTypeId(typeName); });
        }
        else if (auto th = dynamic_cast<ThrowNode*>(node)) {
            flowGen->generateThrow(th, parentFunc, 
                [this](Node* n) { return this->handleExpression(n); },
                StructTypes,
                [this](const std::string& s, std::vector<Value*>& v) { return this->structGen->allocateAndInit(s, v); }
            );
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
                    // Int → Double widening on return. Sema already
                    // accepts `Int` where `Double` is declared (the
                    // `isCompatibleTypes` widening rule); Codegen
                    // needs to actually emit the conversion or LLVM
                    // rejects `ret i32` against a `double` signature.
                    else if (retVal->getType()->isIntegerTy() && expectedType->isDoubleTy()) {
                        retVal = Builder.CreateSIToFP(retVal, expectedType, "ret_int_to_dbl");
                    }
                    // Double → Int narrowing on return. Symmetric to
                    // the widening above; lossy but matches how
                    // explicit casts elsewhere behave.
                    else if (retVal->getType()->isDoubleTy() && expectedType->isIntegerTy()) {
                        retVal = Builder.CreateFPToSI(retVal, expectedType, "ret_dbl_to_int");
                    }
                    else if (retVal->getType()->isPointerTy() && retVal->getType()->getPointerElementType()->isIntegerTy(8) &&
                             expectedType->isPointerTy() && expectedType->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(expectedType->getPointerElementType());
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        // Check for `return null` BEFORE the String unbox
                        // branch — otherwise quirk_opaque_to_string(null)
                        // produces the literal string "null", which makes
                        // `case null` arms at the call site silently miss.
                        // Nullable returns (`-> String?`, `-> User?`, etc.)
                        // need to propagate the null pointer as-is.
                        if (isa<ConstantPointerNull>(retVal)) {
                            retVal = ConstantPointerNull::get(cast<PointerType>(expectedType));
                        } else if (sName == "String") {
                            // The i8* here is a boxed value coming back from a
                            // void*-returning callsite (List.get, Map.get, an
                            // Any-typed Callable invocation, etc). It may be a
                            // String*, an Any*, or a tagged int. Wrapping it as
                            // a raw char* would treat the pointer bytes as the
                            // string contents — garbage. quirk_opaque_to_string
                            // sniffs the three shapes and returns a real String*.
                            // String literals never hit this branch: they are
                            // materialised as String* at the LiteralNode site.
                            Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                            if (!opaqueToStr) {
                                Type* retTy = PointerType::getUnqual(StructTypes["String"]);
                                FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                                opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule.get());
                            }
                            retVal = Builder.CreateCall(opaqueToStr, {retVal}, "ret_unbox_str");
                        } else if (sName != "Callable") {
                            // i8* (boxed) → struct*: bitcast (Callable handled
                            // by the boxToVoidPtr branch below for symmetry).
                            retVal = Builder.CreateBitCast(retVal, expectedType);
                        }
                    }
                    // i8* boxed result (from a Callable call OR a nonlocal-
                    // cell read) → unbox to a primitive scalar. Two
                    // encodings can show up:
                    //   - Legacy tagged int (low 32 bits = value) —
                    //     PtrToInt round-trips correctly.
                    //   - Any* heap-box (from v2.3.4 nonlocal-cell
                    //     boxing) — PtrToInt returns the pointer
                    //     address, NOT the contained int. Use
                    //     quirk_opaque_to_int which handles both.
                    // Was a regression in v2.3.4: `return count` from
                    // a lambda that captures a nonlocal Int counter
                    // gave the heap-Any pointer address as the
                    // returned int.
                    else if (retVal->getType()->isPointerTy()
                          && retVal->getType()->getPointerElementType()->isIntegerTy(8)
                          && expectedType->isIntegerTy()) {
                        Type* i8p = Type::getInt8PtrTy(Context);
                        FunctionCallee toInt = TheModule->getOrInsertFunction(
                            "quirk_opaque_to_int",
                            Type::getInt32Ty(Context), i8p);
                        Value* asI32 = Builder.CreateCall(toInt, {retVal}, "ret_unbox_int");
                        retVal = expectedType->isIntegerTy(32)
                            ? asI32
                            : Builder.CreateIntCast(asI32, expectedType, true);
                    }
                    else if (retVal->getType()->isPointerTy()
                          && retVal->getType()->getPointerElementType()->isIntegerTy(8)
                          && expectedType->isDoubleTy()) {
                        // Same Any*-vs-tagged-int issue for Double
                        // returns. quirk_opaque_to_double handles
                        // both encodings.
                        Type* i8p = Type::getInt8PtrTy(Context);
                        FunctionCallee toDbl = TheModule->getOrInsertFunction(
                            "quirk_opaque_to_double",
                            Type::getDoubleTy(Context), i8p);
                        retVal = Builder.CreateCall(toDbl, {retVal}, "ret_unbox_dbl");
                    }
                    // Returning from a lambda (i8* return): box the value
                    else if (expectedType == Type::getInt8PtrTy(Context)) {
                        retVal = boxToVoidPtr(retVal);
                    }
                    // Variant struct → union parent struct. `return
                    // Some(42)` from a function declared `-> Option`
                    // gives a `%Some*` retVal where expectedType is
                    // `%Option*`. Bitcast: the variant's first field
                    // (__type_id) matches the union base layout, so
                    // upcasting is layout-safe and preserves vtable
                    // dispatch. Without this, LLVM's tail-merge
                    // tries to phi `%Some*` and `%None*` against an
                    // `%Option*` result and the verifier rejects it.
                    else if (retVal->getType()->isPointerTy() &&
                             expectedType->isPointerTy() &&
                             retVal->getType()->getPointerElementType()->isStructTy() &&
                             expectedType->getPointerElementType()->isStructTy()) {
                        retVal = Builder.CreateBitCast(retVal, expectedType, "ret_variant_upcast");
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

        // Track Callable return types and variadic info for direct lambda invocation
        if (auto lambda = dynamic_cast<LambdaNode*>(vdecl->expression.get())) {
            if (auto lhsLit = dynamic_cast<LiteralNode*>(vdecl->lhs.get())) {
                if (!lambda->inferredReturnType.empty())
                    callableReturnTypes[lhsLit->value] = lambda->inferredReturnType;
                int varStart = -1;
                for (int pi = 0; pi < (int)lambda->params.size(); pi++) {
                    if (lambda->params[pi].isVariadic) { varStart = pi; break; }
                }
                variadicCallableStart[lhsLit->value] = varStart;
            }
        }

        Value* val = handleExpression(vdecl->expression.get());
        if (!val || val->getType()->isVoidTy()) return;

        // Tuple destructuring: (a, b) := tuple_expr
        // Multi-assign broadcast: a, b, c := value  (assigns same value to all)
        if (auto* tupLhs = dynamic_cast<TupleLiteralNode*>(vdecl->lhs.get())) {
            bool isTupleRhs = val->getType()->isPointerTy() && StructTypes.count("Tuple") &&
                              val->getType()->getPointerElementType() == StructTypes["Tuple"];

            if (!isTupleRhs) {
                // Broadcast: assign the same value to every LHS variable
                for (auto& elem : tupLhs->elements) {
                    auto* nameNode = dynamic_cast<LiteralNode*>(elem.get());
                    if (!nameNode) continue;
                    if (!varGen->exists(nameNode->value)) varGen->defineLocalVariable(nameNode->value, val);
                    else varGen->updateLocalVariable(nameNode->value, val);
                }
                return;
            }

            Function* getFn = TheModule->getFunction("Core_Collections_Tuple_Tuple___get");
            if (!getFn) {
                Type* tuplePtrTy = PointerType::getUnqual(StructTypes["Tuple"]);
                FunctionType* ft = FunctionType::get(Type::getInt8PtrTy(Context),
                    {tuplePtrTy, Type::getInt32Ty(Context)}, false);
                getFn = Function::Create(ft, Function::ExternalLinkage, "Core_Collections_Tuple_Tuple___get", TheModule.get());
            }
            if (getFn) {
                for (int i = 0; i < (int)tupLhs->elements.size(); i++) {
                    auto* nameNode = dynamic_cast<LiteralNode*>(tupLhs->elements[i].get());
                    if (!nameNode) continue;
                    Value* elem = Builder.CreateCall(getFn,
                        {val, ConstantInt::get(Type::getInt32Ty(Context), i)});
                    if (!varGen->exists(nameNode->value)) varGen->defineLocalVariable(nameNode->value, elem);
                    else varGen->updateLocalVariable(nameNode->value, elem);
                }
            }
            return;
        }

        if (!vdecl->typeAnnotation.empty()) {
            // Target type is explicitly Any — box the value
            if (vdecl->typeAnnotation == "Any") {
                val = emitBox(val);
            } else {
                Type* targetType = typeGen->getLLVMType(vdecl->typeAnnotation);

                // Nullable primitive/enum: target is i8* but the
                // *base* (non-nullable) is a value type. Box the
                // value into a heap Any (or pass through if it's
                // already a pointer / a null literal) so the i8*
                // slot can hold null OR a real value. The rest of
                // the existing coercion logic still runs against
                // the boxed pointer; for an i8* target the chain
                // becomes a no-op (val is already i8*).
                bool annoIsNullablePrim = false;
                if (vdecl->typeAnnotation.back() == '?') {
                    std::string base = vdecl->typeAnnotation.substr(
                        0, vdecl->typeAnnotation.size() - 1);
                    annoIsNullablePrim =
                        (base == "int" || base == "Int" ||
                         base == "bool" || base == "Bool" ||
                         base == "double" || base == "Double" ||
                         base == "char" || base == "Char" ||
                         enumVariants.count(base));
                }
                if (annoIsNullablePrim && val) {
                    Type* i8p = Type::getInt8PtrTy(Context);
                    if (val->getType()->isIntegerTy() ||
                        val->getType()->isDoubleTy()) {
                        val = emitBox(val);  // → Any* (struct.Any*)
                    }
                    if (val->getType() != i8p && val->getType()->isPointerTy()) {
                        val = Builder.CreateBitCast(val, i8p);
                    }
                    targetType = i8p;  // skip the non-nullable-target paths below
                }


                // Source is Any* and target is a concrete type — unbox
                if (isAnyType(val)) {
                    val = emitUnboxToType(val, targetType);
                }
                // Source is type-erased i8* (Map_get/List_get/lambda
                // result/Enum.parse(...)).
                else if (val->getType()->isPointerTy() &&
                         val->getType()->getPointerElementType()->isIntegerTy(8)) {
                    if (targetType->isIntegerTy(32) || targetType->isDoubleTy()) {
                        // Route through emitUnboxToType so heap-Any-boxed
                        // values unbox correctly. The previous shortcut
                        // PtrToInt(i8*, i32) only worked for tagged-int
                        // pointers (`inttoptr small_int`); for a real
                        // Any* heap allocation it returned the heap
                        // address as an int — garbage.
                        val = emitUnboxToType(val, targetType);
                    } else if (targetType->isIntegerTy())
                        val = Builder.CreatePtrToInt(val, targetType);
                    else if (targetType->isPointerTy() && targetType->getPointerElementType()->isStructTy()) {
                        // Struct-pointer target from an i8* source (the
                        // canonical shape returned by Map.get / List.get
                        // / lambda return / Any-typed slot). Route
                        // through emitUnboxToType so a heap-Any-boxed
                        // value gets its `ptr` field extracted via
                        // `quirk_opaque_to_struct`; a raw struct
                        // pointer passes through unchanged. Bare
                        // bitcast (the previous behaviour) was wrong
                        // for Any-wrapped values — it aligned the
                        // Any's `tag` field with the target struct's
                        // first field, corrupting every access.
                        val = emitUnboxToType(val, targetType);
                    }
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
            if (op == "%=") return mathGen->generateBinaryOp("%", wasVal, newVal);
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

            // Module-scope declaration: materialise the binding as an LLVM
            // GlobalVariable so every function (not just main) can see it.
            // We only create one on the *first* definition; subsequent
            // `=` assignments in this scope route through updateLocalVariable
            // which now knows how to write through to the global.
            if (inModuleScope && !varGen->exists(lhs->value)) {
                Type* gvTy = val->getType();
                Constant* initVal = Constant::getNullValue(gvTy);
                GlobalVariable* gv = new GlobalVariable(
                    *TheModule, gvTy, /*isConstant=*/false,
                    GlobalValue::InternalLinkage, initVal, lhs->value);
                Builder.CreateStore(val, gv);
                varGen->defineGlobal(lhs->value, gv);
            } else if (!inModuleScope && vdecl->op == ":=" && !varGen->hasLocal(lhs->value)) {
                // Inside a function body, a fresh `name := value`
                // declaration always creates a LOCAL that shadows any
                // module-level same-named binding. Without this guard,
                // a stdlib method whose body does `n := self.length()`
                // ends up writing its Int return through the user's
                // top-level `n := "alex"` global slot, leaving the
                // global typed String* but storing an Int — and
                // downstream `i < n` ICmp-mismatches in the LLVM
                // verifier.
                varGen->defineLocalVariable(lhs->value, val);
            } else if (!varGen->exists(lhs->value)) {
                varGen->defineLocalVariable(lhs->value, val);
            } else {
                varGen->updateLocalVariable(lhs->value, val);
            }

            // Track enum type for .str() / .value calls.
            //   x := Gender.Male      → MemberAccess(Gender, Male)
            //   x := Gender("Male")   → Call(callee=Gender, ...)
            // Both shapes bind x to a Gender ordinal; the codegen needs
            // to remember which enum so `.value` knows which packed
            // global to look up.
            if (auto* rhsMember = dynamic_cast<MemberAccessNode*>(vdecl->expression.get())) {
                if (auto* rhsLit = dynamic_cast<LiteralNode*>(rhsMember->object.get())) {
                    if (enumVariants.count(rhsLit->value)) {
                        varEnumTypes[lhs->value] = rhsLit->value;
                    }
                }
            } else if (auto* rhsCall = dynamic_cast<CallNode*>(vdecl->expression.get())) {
                if (auto* rhsLit = dynamic_cast<LiteralNode*>(rhsCall->callee.get())) {
                    if (backedEnums.count(rhsLit->value)) {
                        varEnumTypes[lhs->value] = rhsLit->value;
                    }
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
                    // Double → Int field. Sema allows the assignment
                    // when the param is declared Double and the field
                    // is Int (technically a narrowing, but it slips
                    // through `isCompatibleTypes`'s widening branch
                    // because either direction goes through "numeric").
                    // Without the FPToSI here, Codegen emits
                    // `store double, i32*` and LLVM's verifier rejects.
                    else if (val->getType()->isDoubleTy() && fieldType->isIntegerTy()) val = Builder.CreateFPToSI(val, fieldType);
                    // i8* (Any-laundered value, e.g. list.get(i)) → Int / Double field.
                    // Without this, Codegen tried `store i8* into i32*` and the
                    // verifier aborted with "Stored value type does not match pointer".
                    // emitUnboxToType handles the Any* heap-wrap path AND the
                    // raw-tagged-int path.
                    else if (val->getType()->isPointerTy() &&
                             val->getType()->getPointerElementType()->isIntegerTy(8) &&
                             (fieldType->isIntegerTy(32) || fieldType->isDoubleTy())) {
                        val = emitUnboxToType(val, fieldType);
                    }
                    // i8* → Bool (i1) field. Route through
                    // quirk_any_as_bool — the same helper used for
                    // truthy-check on i8* values in if/while
                    // conditions. Sema permits the assignment
                    // because Any widens to anything; without this
                    // Codegen tried a raw `bitcast i8* to i1`,
                    // which the verifier rejects.
                    else if (val->getType()->isPointerTy() &&
                             val->getType()->getPointerElementType()->isIntegerTy(8) &&
                             fieldType->isIntegerTy(1)) {
                        Type* i8p = Type::getInt8PtrTy(Context);
                        FunctionCallee toBool = TheModule->getOrInsertFunction(
                            "quirk_any_as_bool",
                            Type::getInt32Ty(Context), i8p);
                        Value* asI32 = Builder.CreateCall(toBool, {val}, "field_unbox_bool");
                        val = Builder.CreateICmpNE(
                            asI32, ConstantInt::get(Type::getInt32Ty(Context), 0));
                    }
                }
                
                Builder.CreateStore(val, memberPtr); 

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
        // Raw i8* (the universal opaque shape — inline-tag int, heap
        // Any*, String*, anything from `__get` / `Map.get` / lambda
        // returns) is already a valid Any payload: every
        // `quirk_opaque_to_*` decoder accepts i8* and dispatches on
        // the runtime tag. Re-boxing as `Any_box_string` (the old
        // fallback for "i8* that isn't a struct") corrupts the
        // value — a tagged-int Int 3 came out as a heap Any-of-String
        // "3", and `ai: Int := a` then quirk_opaque_to_int'd the
        // string-tagged wrapper and returned 0.
        if (v->getType()->isPointerTy() &&
            v->getType()->getPointerElementType()->isIntegerTy(8)) {
            return v;
        }

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
                { size_t d = name.find('.'); if (d != std::string::npos && std::isdigit((unsigned char)name[d+1])) name = name.substr(0, d); }
                // Already Any* — pass through
                if (name == "Any") return v;
                // box_* are declared as i8*(i8*) — must bitcast struct ptr to i8* first
                Value* asPtr = Builder.CreateBitCast(v, Type::getInt8PtrTy(Context));
                if (name.find("String") != std::string::npos) return callBox("Core_Primitives_Any_box_string", {asPtr});
                if (name.find("List")   != std::string::npos) return callBox("Core_Primitives_Any_box_list",   {asPtr});
                if (name.find("Map")    != std::string::npos) return callBox("Core_Primitives_Any_box_map",    {asPtr});
                if (name.find("Tuple")  != std::string::npos) return callBox("Core_Primitives_Any_box_tuple",  {asPtr});
                // Other struct — call __str, walking up the inheritance hierarchy
                auto findStrFunc = [&](const std::string& typeName) -> Function* {
                    Function* f = TheModule->getFunction(typeName + "___str");
                    if (!f) { std::string sfx = typeName + "___str"; for (auto& F : *TheModule) if (F.getName().endswith(sfx)) { f = &F; break; } }
                    if (!f) f = TheModule->getFunction(typeName + "__str");
                    if (!f) f = TheModule->getFunction(typeName + "___repr");
                    if (!f) f = TheModule->getFunction(typeName + "__repr");
                    return f;
                };
                Function* strFunc = findStrFunc(name);
                Value* strSelf = v;
                if (!strFunc) {
                    // Walk inheritance hierarchy
                    std::string current = name;
                    while (!strFunc && structHierarchy.count(current) && !structHierarchy[current].empty()) {
                        current = structHierarchy[current][0];
                        strFunc = findStrFunc(current);
                        if (strFunc && StructTypes.count(current))
                            strSelf = Builder.CreateBitCast(v, PointerType::getUnqual(StructTypes[current]));
                    }
                }
                if (strFunc) {
                    Value* strObj = Builder.CreateCall(strFunc, {strSelf});
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

    // Emit unboxing from Any* to a specific LLVM target type.
    //
    // For Int/Double targets we route through the runtime's
    // quirk_opaque_to_int / quirk_opaque_to_double helpers, which
    // accept any of the three opaque shapes (tagged int, Any* heap
    // wrap, raw value cast) safely. The old code called
    // Core_Primitives_Any_to_int which assumed a real Any*; passing a
    // tagged-int (the common shape returned by list[i] for an Int
    // element) made it dereference a small-int address and SIGSEGV.
    Value* emitUnboxToType(Value* anyPtr, Type* targetType) {
        Type* i8p = Type::getInt8PtrTy(Context);
        Value* asI8 = anyPtr;
        if (anyPtr->getType() != i8p)
            asI8 = Builder.CreateBitCast(anyPtr, i8p);

        if (targetType->isIntegerTy(32)) {
            FunctionCallee f = TheModule->getOrInsertFunction(
                "quirk_opaque_to_int",
                FunctionType::get(Type::getInt32Ty(Context), {i8p}, false));
            return Builder.CreateCall(f, {asI8});
        }
        if (targetType->isDoubleTy()) {
            FunctionCallee f = TheModule->getOrInsertFunction(
                "quirk_opaque_to_double",
                FunctionType::get(Type::getDoubleTy(Context), {i8p}, false));
            return Builder.CreateCall(f, {asI8});
        }
        // Fall-through path uses the original Any*-based logic for
        // struct-pointer targets.
        if (anyPtr->getType()->isPointerTy() &&
            anyPtr->getType()->getPointerElementType()->isIntegerTy(8) &&
            StructTypes.count("Any")) {
            anyPtr = Builder.CreateBitCast(anyPtr, PointerType::getUnqual(StructTypes["Any"]));
        }
        if (targetType->isPointerTy() && targetType->getPointerElementType()->isStructTy()) {
            std::string name = cast<StructType>(targetType->getPointerElementType())->getName().str();
            if (name.find("struct.") == 0) name = name.substr(7);
            if (name.find("String") != std::string::npos) {
                // quirk_opaque_to_string already handles the three
                // shapes we might see at this point — tagged-int Any
                // (stringifies to "N"), heap Any* (dispatches via
                // tag), or raw String* (passes through). The
                // previously-used `Any_to_str` only handled the
                // tagged-Any case; raw String* cast to Any* read
                // garbage from the tag slot.
                FunctionCallee f = TheModule->getOrInsertFunction(
                    "quirk_opaque_to_string",
                    FunctionType::get(targetType,
                                      {Type::getInt8PtrTy(Context)}, false));
                Value* asI8 = anyPtr;
                if (anyPtr->getType() != Type::getInt8PtrTy(Context))
                    asI8 = Builder.CreateBitCast(anyPtr, Type::getInt8PtrTy(Context));
                return Builder.CreateCall(f, {asI8});
            }
            // For non-String struct targets (Tuple, List, Map, Set,
            // Callable, user structs), route through
            // `quirk_opaque_to_struct` which sniffs the runtime shape
            // and either:
            //   - extracts `Any->ptr` when the value is an Any-tagged
            //     wrapper (the case `xs.get(0)` returns when the list
            //     stores Any-boxed elements), or
            //   - returns the value unchanged when it's already a
            //     raw struct pointer.
            //
            // Without this, a bare bitcast Any* → Tuple* below would
            // line up the Any's `tag` field with the Tuple's `data`
            // pointer (4 bytes vs 8) and corrupt every subsequent
            // field access.
            if (name != "Any" && name != "struct.Any") {
                FunctionCallee f = TheModule->getOrInsertFunction(
                    "quirk_opaque_to_struct",
                    FunctionType::get(Type::getInt8PtrTy(Context),
                                      {Type::getInt8PtrTy(Context)}, false));
                Value* asI8 = anyPtr;
                if (anyPtr->getType() != Type::getInt8PtrTy(Context))
                    asI8 = Builder.CreateBitCast(anyPtr, Type::getInt8PtrTy(Context));
                Value* raw = Builder.CreateCall(f, {asI8});
                return Builder.CreateBitCast(raw, targetType);
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

    // --- List Comprehension: [expr for var in iterable (where cond)] ---
    Value* generateListComprehension(ListComprehensionNode* comp) {
        Function* parentFunc = Builder.GetInsertBlock()->getParent();

        // 1. Create an empty result list.
        std::vector<Value*> noArgs;
        Value* resultList = structGen->allocateAndInit("List", noArgs);
        if (!resultList) return nullptr;

        Function* appendFunc = TheModule->getFunction("Core_Collections_List_List_append");
        if (!appendFunc) return resultList;

        // 2. Evaluate the iterable and get its iterator.
        Value* iterable = handleExpression(comp->iterable.get());
        if (!iterable) return resultList;
        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isPointerTy())
            iterable = Builder.CreateLoad(iterable->getType()->getPointerElementType(), iterable);

        if (!iterable->getType()->isPointerTy() ||
            !iterable->getType()->getPointerElementType()->isStructTy())
            return resultList;

        StructType* st = cast<StructType>(iterable->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        auto resolveF = [&](const std::string& n) -> Function* {
            Function* f = TheModule->getFunction(n);
            if (!f && functionDeclarations.count(n)) {
                const auto& ln = functionDeclarations[n]->linkageName;
                if (!ln.empty()) f = TheModule->getFunction(ln);
            }
            return f;
        };

        bool isPair = !comp->varName2.empty();
        Function* iterFunc = isPair ? resolveF(structName + "___iter_pairs") : nullptr;
        if (!iterFunc) iterFunc = resolveF(structName + "___iter");
        if (!iterFunc) iterFunc = resolveF(structName + "__iter");
        if (!iterFunc) return resultList;

        Value* iteratorObj = Builder.CreateCall(iterFunc, {iterable}, "comp_iter");
        StructType* iterST = cast<StructType>(iteratorObj->getType()->getPointerElementType());
        std::string iterName = iterST->getName().str();

        Function* hasNextFunc = resolveF(iterName + "___has_next");
        if (!hasNextFunc) hasNextFunc = resolveF(iterName + "__has_next");
        Function* nextFunc = resolveF(iterName + "___next");
        if (!nextFunc) nextFunc = resolveF(iterName + "__next");
        Function* curValFunc = isPair ? resolveF(iterName + "___current_value") : nullptr;
        if (!hasNextFunc || !nextFunc) return resultList;

        // 3. Loop: cond → body → back
        BasicBlock* condBB  = BasicBlock::Create(Context, "lcomp_cond",  parentFunc);
        BasicBlock* bodyBB  = BasicBlock::Create(Context, "lcomp_body",  parentFunc);
        BasicBlock* afterBB = BasicBlock::Create(Context, "lcomp_after", parentFunc);

        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(condBB);
        Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj});
        Builder.CreateCondBr(flowGen->toBool(hasNext), bodyBB, afterBB);

        Builder.SetInsertPoint(bodyBB);
        Value* item = Builder.CreateCall(nextFunc, {iteratorObj}, "comp_item");
        varGen->defineLocalVariable(comp->varName, item);
        if (isPair && curValFunc) {
            Value* val = Builder.CreateCall(curValFunc, {iteratorObj}, "comp_val");
            varGen->defineLocalVariable(comp->varName2, val);
        }

        // 4. Optional where filter.
        if (comp->condition) {
            BasicBlock* appendBB = BasicBlock::Create(Context, "lcomp_append", parentFunc);
            Value* cond = handleExpression(comp->condition.get());
            Builder.CreateCondBr(flowGen->toBool(cond), appendBB, condBB);
            Builder.SetInsertPoint(appendBB);
        }

        // 5. Evaluate element and append.
        Value* elem = handleExpression(comp->expr.get());
        if (elem) {
            Type* voidPtr = Type::getInt8PtrTy(Context);
            Value* boxed = elem;
            if (elem->getType()->isIntegerTy()) boxed = Builder.CreateIntToPtr(elem, voidPtr);
            else if (elem->getType() != voidPtr) boxed = Builder.CreateBitCast(elem, voidPtr);
            Builder.CreateCall(appendFunc, {resultList, boxed});
        }
        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(afterBB);
        return resultList;
    }

    // --- Map Comprehension: {key: val for var in iterable (where cond)} ---
    Value* generateMapComprehension(MapComprehensionNode* comp) {
        Function* parentFunc = Builder.GetInsertBlock()->getParent();

        // 1. Create empty map.
        std::vector<Value*> noArgs;
        Value* resultMap = structGen->allocateAndInit("Map", noArgs);
        if (!resultMap) return nullptr;

        Function* putFunc = TheModule->getFunction("Core_Collections_Map_Map_put");
        if (!putFunc) return resultMap;

        // 2. Evaluate iterable and get iterator.
        Value* iterable = handleExpression(comp->iterable.get());
        if (!iterable) return resultMap;
        if (iterable->getType()->isPointerTy() &&
            iterable->getType()->getPointerElementType()->isPointerTy())
            iterable = Builder.CreateLoad(iterable->getType()->getPointerElementType(), iterable);

        if (!iterable->getType()->isPointerTy() ||
            !iterable->getType()->getPointerElementType()->isStructTy())
            return resultMap;

        StructType* st = cast<StructType>(iterable->getType()->getPointerElementType());
        std::string structName = st->getName().str();

        auto resolveF = [&](const std::string& n) -> Function* {
            Function* f = TheModule->getFunction(n);
            if (!f && functionDeclarations.count(n)) {
                const auto& ln = functionDeclarations[n]->linkageName;
                if (!ln.empty()) f = TheModule->getFunction(ln);
            }
            return f;
        };

        bool isPair = !comp->varName2.empty();
        Function* iterFunc = isPair ? resolveF(structName + "___iter_pairs") : nullptr;
        if (!iterFunc) iterFunc = resolveF(structName + "___iter");
        if (!iterFunc) iterFunc = resolveF(structName + "__iter");
        if (!iterFunc) return resultMap;

        Value* iteratorObj = Builder.CreateCall(iterFunc, {iterable}, "mcomp_iter");
        StructType* iterST = cast<StructType>(iteratorObj->getType()->getPointerElementType());
        std::string iterName = iterST->getName().str();

        Function* hasNextFunc = resolveF(iterName + "___has_next");
        if (!hasNextFunc) hasNextFunc = resolveF(iterName + "__has_next");
        Function* nextFunc = resolveF(iterName + "___next");
        if (!nextFunc) nextFunc = resolveF(iterName + "__next");
        Function* curValFunc = isPair ? resolveF(iterName + "___current_value") : nullptr;
        if (!hasNextFunc || !nextFunc) return resultMap;

        BasicBlock* condBB  = BasicBlock::Create(Context, "mcomp_cond",  parentFunc);
        BasicBlock* bodyBB  = BasicBlock::Create(Context, "mcomp_body",  parentFunc);
        BasicBlock* afterBB = BasicBlock::Create(Context, "mcomp_after", parentFunc);

        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(condBB);
        Value* hasNext = Builder.CreateCall(hasNextFunc, {iteratorObj});
        Builder.CreateCondBr(flowGen->toBool(hasNext), bodyBB, afterBB);

        Builder.SetInsertPoint(bodyBB);
        Value* item = Builder.CreateCall(nextFunc, {iteratorObj}, "mcomp_item");
        varGen->defineLocalVariable(comp->varName, item);
        if (isPair && curValFunc) {
            Value* val = Builder.CreateCall(curValFunc, {iteratorObj}, "mcomp_val");
            varGen->defineLocalVariable(comp->varName2, val);
        }

        if (comp->condition) {
            BasicBlock* putBB = BasicBlock::Create(Context, "mcomp_put", parentFunc);
            Value* cond = handleExpression(comp->condition.get());
            Builder.CreateCondBr(flowGen->toBool(cond), putBB, condBB);
            Builder.SetInsertPoint(putBB);
        }

        Value* keyVal = handleExpression(comp->keyExpr.get());
        Value* valVal = handleExpression(comp->valExpr.get());
        if (keyVal && valVal) {
            // Key must be a String* — unbox via quirk_opaque_to_string if it arrives as i8*
            if (keyVal->getType()->isPointerTy() &&
                keyVal->getType()->getPointerElementType()->isIntegerTy(8)) {
                Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                if (!opaqueToStr) {
                    Type* retTy = StructTypes.count("String")
                        ? (Type*)PointerType::getUnqual(StructTypes["String"])
                        : (Type*)Type::getInt8PtrTy(Context);
                    FunctionType* ft = FunctionType::get(retTy, {Type::getInt8PtrTy(Context)}, false);
                    opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule.get());
                }
                keyVal = Builder.CreateCall(opaqueToStr, {keyVal}, "mcomp_key_str");
            }
            Type* voidPtr = Type::getInt8PtrTy(Context);
            if (valVal->getType()->isIntegerTy()) valVal = Builder.CreateIntToPtr(valVal, voidPtr);
            else if (valVal->getType() != voidPtr) valVal = Builder.CreateBitCast(valVal, voidPtr);
            Builder.CreateCall(putFunc, {resultMap, keyVal, valVal});
        }
        Builder.CreateBr(condBB);
        Builder.SetInsertPoint(afterBB);
        return resultMap;
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
        if (lit->value == "true")  return ConstantInt::getTrue(Context);
        if (lit->value == "false") return ConstantInt::getFalse(Context);
        if (lit->value == "null")  return Constant::getNullValue(Type::getInt8PtrTy(Context));

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
            // Single-quoted literals are 1-char Strings (Char type is gone).
            std::string rawStr = unescapeString(lit->value.substr(1, lit->value.size() - 2));
            Value* rawPtr = Builder.CreateGlobalStringPtr(rawStr);
            std::vector<Value*> args = {rawPtr};
            return structGen->allocateAndInit("String", args);
        }

        if (varGen->exists(lit->value)) return varGen->resolveVariable(lit->value);

        // Function name in a value context (not a direct call) — synthesize
        // a Callable wrapper on the fly. Lets users pass `define`d functions
        // as Callable args, store them in variables, return them, etc.
        if (functionDeclarations.count(lit->value)) {
            if (Value* cb = emitFunctionAsCallable(lit->value)) return cb;
        }
    }

    if (auto arr = dynamic_cast<ListLiteralNode*>(node)) return structGen->generateListLiteral(arr, [this](Node* n) { return this->handleExpression(n); });
    if (auto setLit = dynamic_cast<SetLiteralNode*>(node)) return structGen->generateSetLiteral(setLit, [this](Node* n) { return this->handleExpression(n); });
    if (auto mapLit = dynamic_cast<MapLiteralNode*>(node)) return structGen->generateMapLiteral(mapLit, [this](Node* n) { return this->handleExpression(n); });
    if (auto comp = dynamic_cast<ListComprehensionNode*>(node)) return generateListComprehension(comp);
    if (auto comp = dynamic_cast<MapComprehensionNode*>(node)) return generateMapComprehension(comp);
    if (auto tup = dynamic_cast<TupleLiteralNode*>(node)) return structGen->generateTupleLiteral(tup,
        [this](Node* n) { return this->handleExpression(n); },
        [this](Value* v) { return this->boxToVoidPtr(v); });

    // RangeLiteralNode in non-for-loop expression context is not supported directly
    if (dynamic_cast<RangeLiteralNode*>(node)) return nullptr;

    if (auto binOp = dynamic_cast<BinaryOpNode*>(node)) {
        if (binOp->op == "not") return mathGen->generateNot(handleExpression(binOp->left.get()));

        // Postfix `?` — null/zero check: true when value is not null/zero
        if (binOp->op == "?") {
            Value* val = handleExpression(binOp->left.get());
            if (!val) return ConstantInt::getFalse(Context);
            if (val->getType()->isPointerTy())
                return Builder.CreateICmpNE(val, Constant::getNullValue(val->getType()), "hasval");
            if (val->getType()->isIntegerTy())
                return Builder.CreateICmpNE(val, ConstantInt::get(val->getType(), 0), "hasval");
            if (val->getType()->isDoubleTy())
                return Builder.CreateFCmpONE(val, ConstantFP::get(val->getType(), 0.0), "hasval");
            return ConstantInt::getTrue(Context);
        }
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

            // Cast lval to i8* for isinstance — use the canonical boxer so
            // doubles get the proper bitcast→i64→inttoptr sequence; raw
            // IntToPtr would emit invalid IR for a double `lval` (e.g.,
            // `3.14 is Double`).
            Value* lvalI8 = boxToVoidPtr(lval);

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

        // ── `elem in collection` ─────────────────────────────────────────────
        if (binOp->op == "in" || binOp->op == "not in") {
            Value* elem = handleExpression(binOp->left.get());
            Value* coll = handleExpression(binOp->right.get());
            if (!elem || !coll) return ConstantInt::getFalse(Context);

            Type* i8PtrTy = Type::getInt8PtrTy(Context);
            Value* elemI8 = elem->getType() == i8PtrTy
                ? elem
                : (elem->getType()->isPointerTy()
                    ? Builder.CreateBitCast(elem, i8PtrTy)
                    : Builder.CreateIntToPtr(elem, i8PtrTy));

            Value* result = nullptr;

            // Determine collection type and dispatch accordingly
            bool isMap = false, isSet = false, isString = false;
            if (coll->getType()->isPointerTy() && coll->getType()->getPointerElementType()->isStructTy()) {
                std::string sname = cast<StructType>(coll->getType()->getPointerElementType())->getName().str();
                if (sname.find("Set") != std::string::npos) isSet = true;
                else if (sname.find("Map") != std::string::npos) isMap = true;
            } else if (coll->getType()->isPointerTy() && coll->getType()->getPointerElementType()->isIntegerTy(8)) {
                isString = true;
            }

            if (isSet) {
                // `elem in set` → Set.has(set, elem_i8)
                Function* hasFn = TheModule->getFunction("Core_Collections_Set_Set_has");
                if (!hasFn) {
                    FunctionType* ft = FunctionType::get(Type::getInt32Ty(Context), {coll->getType(), i8PtrTy}, false);
                    hasFn = Function::Create(ft, Function::ExternalLinkage, "Core_Collections_Set_Set_has", TheModule.get());
                }
                result = Builder.CreateCall(hasFn, {coll, elemI8}, "in_result");
            } else if (isMap) {
                // `key in map` → Map.has(map, key_str)
                // elem should be a String*; if it's i8* wrap it
                Value* keyStr = elem;
                if (StructTypes.count("String")) {
                    Type* strPtrTy = PointerType::getUnqual(StructTypes["String"]);
                    if (elem->getType() != strPtrTy) {
                        Function* opaqueToStr = TheModule->getFunction("quirk_opaque_to_string");
                        if (!opaqueToStr) {
                            FunctionType* ft = FunctionType::get(strPtrTy, {i8PtrTy}, false);
                            opaqueToStr = Function::Create(ft, Function::ExternalLinkage, "quirk_opaque_to_string", TheModule.get());
                        }
                        Value* asI8 = elem->getType() == i8PtrTy ? elem : Builder.CreateBitCast(elem, i8PtrTy);
                        keyStr = Builder.CreateCall(opaqueToStr, {asI8}, "key_str");
                    }
                }
                Function* hasFn = TheModule->getFunction("Core_Collections_Map_Map_has");
                if (!hasFn) {
                    Type* mapPtrTy = coll->getType();
                    Type* strPtrTy = StructTypes.count("String") ? (Type*)PointerType::getUnqual(StructTypes["String"]) : i8PtrTy;
                    FunctionType* ft = FunctionType::get(Type::getInt32Ty(Context), {mapPtrTy, strPtrTy}, false);
                    hasFn = Function::Create(ft, Function::ExternalLinkage, "Core_Collections_Map_Map_has", TheModule.get());
                }
                result = Builder.CreateCall(hasFn, {coll, keyStr}, "in_result");
            } else if (isString) {
                // `sub in str` → String.contains via strstr check
                // Build both as String* then call Core_String_String_contains
                Value* collStr = coll; // already i8* (char*)
                std::vector<Value*> args = {collStr};
                Value* strObj = structGen->allocateAndInit("String", args);
                Function* containsFn = TheModule->getFunction("Core_String_String_contains");
                if (!containsFn) {
                    Type* strPtrTy = PointerType::getUnqual(StructTypes["String"]);
                    FunctionType* ft = FunctionType::get(Type::getInt32Ty(Context), {strPtrTy, strPtrTy}, false);
                    containsFn = Function::Create(ft, Function::ExternalLinkage, "Core_String_String_contains", TheModule.get());
                }
                // elem must be a String* too
                Value* elemStr = elemI8;
                if (StructTypes.count("String")) {
                    std::vector<Value*> eargs = {elemI8};
                    elemStr = structGen->allocateAndInit("String", eargs);
                }
                result = Builder.CreateCall(containsFn, {strObj, elemStr}, "in_result");
            } else {
                // Default: List.contains
                Value* collI8 = coll->getType() == i8PtrTy ? coll : Builder.CreateBitCast(coll, i8PtrTy);
                Function* containsFn = TheModule->getFunction("Core_Collections_List_List_contains");
                if (!containsFn) {
                    FunctionType* ft = FunctionType::get(Type::getInt32Ty(Context), {i8PtrTy, i8PtrTy}, false);
                    containsFn = Function::Create(ft, Function::ExternalLinkage, "Core_Collections_List_List_contains", TheModule.get());
                }
                result = Builder.CreateCall(containsFn, {collI8, elemI8}, "in_result");
            }

            Value* boolResult = Builder.CreateICmpNE(result, ConstantInt::get(Type::getInt32Ty(Context), 0), "in_bool");
            if (binOp->op == "not in") boolResult = Builder.CreateNot(boolResult, "not_in_bool");
            return boolResult;
        }
        // ── end `in` / `not in` ──────────────────────────────────────────────

        // ── `expr as TypeName` ───────────────────────────────────────────────
        if (binOp->op == "as") {
            Value* src = handleExpression(binOp->left.get());
            if (!src) return nullptr;
            std::string targetType;
            if (auto* lit = dynamic_cast<LiteralNode*>(binOp->right.get()))
                targetType = lit->value;

            Type* srcTy = src->getType();
            Type* i32Ty = Type::getInt32Ty(Context);
            Type* dblTy = Type::getDoubleTy(Context);

            if (targetType == "Double" || targetType == "Float") {
                if (srcTy->isIntegerTy())  return Builder.CreateSIToFP(src, dblTy, "cast_dbl");
                if (srcTy->isDoubleTy())   return src;
            }
            if (targetType == "Int") {
                if (srcTy->isDoubleTy())      return Builder.CreateFPToSI(src, i32Ty, "cast_int");
                if (srcTy->isIntegerTy(1))    return Builder.CreateZExt(src, i32Ty, "cast_int");
                if (srcTy->isIntegerTy(8))    return Builder.CreateZExt(src, i32Ty, "cast_int");
                if (srcTy->isIntegerTy(32))   return src;
                if (srcTy->isPointerTy())     return Builder.CreatePtrToInt(src, i32Ty, "cast_int");
            }
            if (targetType == "Bool") {
                if (srcTy->isIntegerTy(1))  return src;
                if (srcTy->isIntegerTy())   return Builder.CreateICmpNE(src, ConstantInt::get(srcTy, 0), "cast_bool");
                if (srcTy->isDoubleTy())    return Builder.CreateFCmpONE(src, ConstantFP::get(dblTy, 0.0), "cast_bool");
                if (srcTy->isPointerTy())   return Builder.CreateICmpNE(src, Constant::getNullValue(srcTy), "cast_bool");
            }
            if (targetType == "String") {
                // Int/Bool/Double → String via runtime str helpers
                if (srcTy->isIntegerTy(1)) {
                    Function* f = TheModule->getFunction("Core_Primitives_Bool_str");
                    if (f) {
                        Value* ext = Builder.CreateZExt(src, i32Ty);
                        return Builder.CreateCall(f, {ext}, "cast_str");
                    }
                }
                if (srcTy->isIntegerTy()) {
                    Value* i32val = srcTy->isIntegerTy(32) ? src : Builder.CreateSExt(src, i32Ty);
                    Function* f = TheModule->getFunction("Core_Primitives_Int_str");
                    if (f) return Builder.CreateCall(f, {i32val}, "cast_str");
                }
                if (srcTy->isDoubleTy()) {
                    Function* f = TheModule->getFunction("Core_Primitives_Double_str");
                    if (f) return Builder.CreateCall(f, {src}, "cast_str");
                }
            }
            // Pointer-to-pointer: bitcast (covers Any, struct upcasts, etc.)
            if (srcTy->isPointerTy()) {
                Type* dstTy = typeGen->getLLVMType(targetType);
                if (dstTy && dstTy->isPointerTy())
                    return Builder.CreateBitCast(src, dstTy, "cast_ptr");
                // Any = i8*
                if (targetType == "Any")
                    return Builder.CreateBitCast(src, Type::getInt8PtrTy(Context), "cast_any");
            }
            return src; // no-op fallback
        }
        // ── end `as` ─────────────────────────────────────────────────────────

        Value* L = handleExpression(binOp->left.get());
        Value* R = handleExpression(binOp->right.get());
        if (!L || !R) return nullptr;

        // Normalize `expr == null` / `expr != null` for any pointer type.
        // `null` evaluates as a typeless `i8*` ConstantPointerNull; without
        // this fixup, the magic-method dispatch fails (no `Match___eq`) and
        // the math fallback ICmps mismatched pointer types.
        if (binOp->op == "==" || binOp->op == "!=") {
            auto isNullPtr = [](Value* v) {
                return isa<ConstantPointerNull>(v);
            };
            if (isNullPtr(R) && L->getType()->isPointerTy() && L->getType() != R->getType()) {
                R = ConstantPointerNull::get(cast<PointerType>(L->getType()));
            } else if (isNullPtr(L) && R->getType()->isPointerTy() && L->getType() != R->getType()) {
                L = ConstantPointerNull::get(cast<PointerType>(R->getType()));
            }
            // Both null-pointer comparisons now resolve via the math fallback
            // ICmpEQ/ICmpNE on matching pointer types.
            if (isNullPtr(L) || isNullPtr(R)) {
                if (binOp->op == "==")
                    return Builder.CreateICmpEQ(L, R, "ptr_eq_null");
                else
                    return Builder.CreateICmpNE(L, R, "ptr_ne_null");
            }

            // Boxed-Any compared to a primitive Bool/Int literal — e.g.
            // `pred(v) == false` where pred is a `Callable` returning Bool,
            // or `pairs.get(0).0 == 10` where the tuple data is a tagged
            // int (`IntToPtr(N)`). Two i8* encodings can show up:
            //   - Any* heap-box (tagged ANY_INT/BOOL/CHAR — from box_*)
            //   - Tagged integer (`inttoptr` of an i32 — from arithmetic
            //     and tuple/list storage of small ints)
            // `quirk_opaque_to_int` handles both transparently;
            // `Core_Primitives_Any_to_int` SEGV'd on tagged ints because
            // it derefs `.tag` on pointer N.
            Type* i8p = Type::getInt8PtrTy(Context);
            auto unboxAnyToInt = [&](Value* boxedI8p, Type* targetIntTy) {
                FunctionCallee toInt = TheModule->getOrInsertFunction(
                    "quirk_opaque_to_int",
                    Type::getInt32Ty(Context),
                    i8p);
                Value* asI32 = Builder.CreateCall(toInt, {boxedI8p}, "any_unbox_i");
                if (targetIntTy->isIntegerTy(32)) return asI32;
                return Builder.CreateIntCast(asI32, targetIntTy, true, "any_unbox_cast");
            };
            if (L->getType() == i8p && R->getType()->isIntegerTy() &&
                !R->getType()->isIntegerTy(64)) {
                L = unboxAnyToInt(L, Type::getInt32Ty(Context));
                if (R->getType()->isIntegerTy(1))
                    R = Builder.CreateZExt(R, Type::getInt32Ty(Context), "bool_widen");
                else if (R->getType() != L->getType())
                    R = Builder.CreateIntCast(R, L->getType(), true, "rhs_widen");
            } else if (R->getType() == i8p && L->getType()->isIntegerTy() &&
                       !L->getType()->isIntegerTy(64)) {
                R = unboxAnyToInt(R, Type::getInt32Ty(Context));
                if (L->getType()->isIntegerTy(1))
                    L = Builder.CreateZExt(L, Type::getInt32Ty(Context), "bool_widen");
                else if (L->getType() != R->getType())
                    L = Builder.CreateIntCast(L, R->getType(), true, "lhs_widen");
            }
            // i8* (type-erased generic-field read) vs typed struct
            // pointer — the v3 phase 3-b substitution narrows
            // `w.value` to e.g. String at Sema time but the field
            // still lowers as i8*. Bitcast the i8* to the struct ptr
            // type so the magic `___eq` dispatch (below) finds the
            // method and the fallback ICmp uses matching ptr types.
            // For String specifically, dispatch routes through
            // Core_String_String___eq for value-based equality.
            if (L->getType() == i8p && R->getType()->isPointerTy() &&
                R->getType() != i8p &&
                R->getType()->getPointerElementType()->isStructTy()) {
                L = Builder.CreateBitCast(L, R->getType(), "lhs_struct_unbox");
            } else if (R->getType() == i8p && L->getType()->isPointerTy() &&
                       L->getType() != i8p &&
                       L->getType()->getPointerElementType()->isStructTy()) {
                R = Builder.CreateBitCast(R, L->getType(), "rhs_struct_unbox");
            }
            // Both sides erased — no struct type info either way.
            // Route through quirk_opaque_eq, which does shape-aware
            // equality (tagged-int / Any* / String). Without this,
            // `b1.equals(b2)` on `Box[String]` falls through to raw
            // pointer eq and two distinct heap allocations of the
            // same string compare false.
            else if (L->getType() == i8p && R->getType() == i8p) {
                FunctionCallee opEq = TheModule->getOrInsertFunction(
                    "quirk_opaque_eq", Type::getInt32Ty(Context), i8p, i8p);
                Value* res = Builder.CreateCall(opEq, {L, R}, "opaque_eq");
                Value* asBool = Builder.CreateICmpNE(res,
                    ConstantInt::get(Type::getInt32Ty(Context), 0), "opaque_eq_b");
                if (binOp->op == "!=")
                    asBool = Builder.CreateNot(asBool, "opaque_ne");
                return asBool;
            }
        }

        // i8* (type-erased generic-field read) vs typed struct
        // pointer — same bridge as the eq-branch above, extended
        // to ordered comparisons (`<`, `>`, `<=`, `>=`). The bridge
        // is intentionally NOT applied to `+` / `-` / `*` / `/`:
        // for those, an i8* operand against a String* is the
        // string-concat path (`"" + actual` where actual is a
        // tagged Int), and bitcasting the i8* to String* would
        // turn the tagged int into a garbage pointer derefed by
        // the concat helper. Without this `<`-and-friends gate,
        // `b.value < "expected"` on `Box[String]` arrived at the
        // operator-overloading dispatch with L as i8* (struct test
        // fails) and fell through to MathGen's ICmp on mismatched
        // operand types — LLVM verifier rejects.
        {
            const std::string& op = binOp->op;
            bool isOrderedCmp = (op == "<" || op == ">" || op == "<=" || op == ">=");
            if (isOrderedCmp) {
                Type* i8p = Type::getInt8PtrTy(Context);
                if (L->getType() == i8p && R->getType()->isPointerTy() &&
                    R->getType() != i8p &&
                    R->getType()->getPointerElementType()->isStructTy()) {
                    L = Builder.CreateBitCast(L, R->getType(), "lhs_struct_unbox");
                } else if (R->getType() == i8p && L->getType()->isPointerTy() &&
                           L->getType() != i8p &&
                           L->getType()->getPointerElementType()->isStructTy()) {
                    R = Builder.CreateBitCast(R, L->getType(), "rhs_struct_unbox");
                }
            }
        }

        // Commutative-operator swap: when the RHS is a struct pointer
        // and the LHS isn't, but the operator is commutative
        // (`+`, `*`, `==`, `!=`), swap L↔R so the magic-method
        // dispatch below can resolve through the struct's overload.
        // The motivating case is `3 * "ab"` (Int * String) — Sema
        // accepts it as String repetition (v3.17.0), and we want
        // Codegen to dispatch to `String.__mul(self: String, n: Int)`
        // with the String as receiver. Without the swap, L is i32
        // (not a struct pointer), the dispatch skips entirely, and
        // mathGen emits raw `mul %String*, i32` — IR verifier abort.
        if ((binOp->op == "*" || binOp->op == "+" ||
             binOp->op == "==" || binOp->op == "!=") &&
            R && R->getType()->isPointerTy() &&
            R->getType()->getPointerElementType()->isStructTy() &&
            !(L->getType()->isPointerTy() &&
              L->getType()->getPointerElementType()->isStructTy())) {
            std::swap(L, R);
        }

        // Operator overloading: check for magic methods on the left operand struct
        if (L->getType()->isPointerTy() &&
            L->getType()->getPointerElementType()->isStructTy()) {
            StructType* st = cast<StructType>(L->getType()->getPointerElementType());
            std::string sName = st->getName().str();
            if (sName.find("struct.") == 0) sName = sName.substr(7);
            static const std::map<std::string, std::string> opMethods = {
                {"+",  "___add"}, {"-",  "___sub"}, {"*",  "___mul"}, {"/",  "___div"},
                {"==", "___eq"},  {"!=", "___ne"},  {"<",  "___lt"},  {">",  "___gt"},
                {"<=", "___le"},  {">=", "___ge"},
            };
            auto it = opMethods.find(binOp->op);
            if (it != opMethods.end()) {
                // Resolve through the per-struct method registry first
                // (gets the full linkage name including any module
                // prefix); fall back to the bare-name form for user
                // structs that live at module root with no prefix.
                Function* magicFn = nullptr;
                // Consult the per-struct method registry, but ONLY
                // dispatch when the method's declared RHS param type
                // can take the actual R value as-is. This keeps the
                // dispatch targeted:
                //
                //   - `xs + ys` (List + List): __add(self, other: List)
                //     — both struct ptrs match. Dispatch fires.
                //   - `s * n`  (String * Int):  __mul(self, n: Int)
                //     — param[1] is i32 and R is i32. Dispatch fires.
                //   - `s + d`  (String + Double): __add(self,
                //     other: String) — param[1] is String*, R is
                //     double. Skip; legacy coercion paths handle it.
                //
                // Before v3.17.0 the gate required both operands be
                // pointers to the SAME struct type — that worked for
                // List + List but ruled out asymmetric ops like
                // String * Int even though their magic methods accept
                // exactly that shape.
                //
                // opMethods value is the post-mangled dunder
                // (`___add` — designed to be appended to the struct
                // name to form `List___add`). The per-struct map is
                // keyed by the BARE dunder (`__add`, one fewer
                // underscore), so strip the leading `_` here.
                std::string bareDunder = it->second;
                if (!bareDunder.empty() && bareDunder[0] == '_') bareDunder = bareDunder.substr(1);
                auto sIt = structMethodNodes.find(sName);
                if (sIt != structMethodNodes.end()) {
                    auto mIt = sIt->second.find(bareDunder);
                    if (mIt != sIt->second.end()) {
                        const std::string& ln = mIt->second->linkageName;
                        Function* candidate = TheModule->getFunction(ln.empty() ? it->second : ln);
                        if (candidate && candidate->arg_size() >= 2 && R) {
                            Type* rhsParam = candidate->getFunctionType()->getParamType(1);
                            bool rhsFits = false;
                            if (R->getType() == rhsParam) {
                                rhsFits = true;
                            } else if (R->getType()->isPointerTy() && rhsParam->isPointerTy()) {
                                // Same-struct pointers (e.g. List* vs
                                // List*) reach here after the type
                                // equality check above; this branch
                                // catches bitcast-compatible pointer
                                // shapes (struct* ↔ struct* where
                                // both wrap the same StructType).
                                Type* re = R->getType()->getPointerElementType();
                                Type* pe = rhsParam->getPointerElementType();
                                if (re == pe) rhsFits = true;
                            }
                            if (rhsFits) magicFn = candidate;
                        }
                    }
                }
                if (!magicFn) magicFn = TheModule->getFunction(sName + it->second);
                if (magicFn && magicFn->arg_size() >= 2) {
                    Value* rArg = R;
                    Type* expectedTy = magicFn->getFunctionType()->getParamType(1);
                    if (rArg->getType() != expectedTy) {
                        if (rArg->getType()->isPointerTy() && expectedTy->isPointerTy())
                            rArg = Builder.CreateBitCast(rArg, expectedTy);
                        else if (rArg->getType()->isIntegerTy() && expectedTy->isIntegerTy())
                            rArg = Builder.CreateIntCast(rArg, expectedTy, true);
                        else if (rArg->getType()->isIntegerTy() && expectedTy->isPointerTy())
                            rArg = Builder.CreateIntToPtr(rArg, expectedTy);
                    }
                    Value* res = Builder.CreateCall(magicFn, {L, rArg}, "op_result");
                    // Comparison ops are typed Bool at the Quirk level. Extern
                    // implementations widen the C ABI return to i32, so truncate
                    // back to i1 — otherwise the result boxes as ANY_INT and
                    // prints as 0/1 instead of true/false.
                    static const std::set<std::string> cmpOps = {"==","!=","<","<=",">",">="};
                    if (cmpOps.count(binOp->op) && res->getType()->isIntegerTy() &&
                        !res->getType()->isIntegerTy(1)) {
                        res = Builder.CreateICmpNE(
                            res, ConstantInt::get(res->getType(), 0), "cmp_to_bool");
                    }
                    return res;
                }
            }
        }

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
            // String character indexing: call Core_String_String___get for bounds checking
            if (L->getType()->isPointerTy()) {
                Type* elem = L->getType()->getPointerElementType();
                bool isStrStruct = elem->isStructTy() &&
                    cast<StructType>(elem)->getName().contains("String");
                if (isStrStruct) {
                    Type* i32Ty = Type::getInt32Ty(Context);
                    Type* strPtrTy = L->getType();
                    FunctionCallee getFn = TheModule->getOrInsertFunction(
                        "Core_String_String___get",
                        i32Ty, strPtrTy, i32Ty);
                    Value* idx = R;
                    if (!idx->getType()->isIntegerTy(32))
                        idx = Builder.CreateIntCast(idx, i32Ty, true);
                    return Builder.CreateCall(getFn, {L, idx}, "char_at");
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

            // Don't trigger string concat when an opaque i8* is mixed
            // with a plain numeric type — that case goes through
            // arithmetic unboxing (e.g. nonlocal cell values). Also
            // suppress when BOTH sides are opaque i8* (no struct type
            // info): default to numeric. The arithmetic unbox in
            // MathGen handles both encodings (Any* heap-box, tagged
            // int), so we won't crash on a real String pair — at
            // worst we'd get a garbage int. Real String literals are
            // always typed as `%String*` not `i8*`, so they take the
            // string-concat branch correctly.
            bool lOpaque = L->getType()->isPointerTy() && L->getType()->getPointerElementType()->isIntegerTy(8);
            bool rOpaque = R->getType()->isPointerTy() && R->getType()->getPointerElementType()->isIntegerTy(8);
            bool lNum = L->getType()->isIntegerTy() || L->getType()->isDoubleTy();
            bool rNum = R->getType()->isIntegerTy() || R->getType()->isDoubleTy();
            bool suppressString = (lOpaque && rNum) || (rOpaque && lNum) || (lOpaque && rOpaque);

            if (!suppressString && (isStringType(L) || isStringType(R))) {
                auto makeString = [&](Value* val) -> Value* {
                    Type* ty = val->getType();
                    // 1. Already a String Struct
                    if (ty->isPointerTy() && ty->getPointerElementType()->isStructTy()) {
                        StructType* st = cast<StructType>(ty->getPointerElementType());
                        if (st->getName().contains("String")) return val;
                        // ... (keep __str call logic) ...
                        std::string sName = st->getName().str();
                        if (sName.find("struct.") == 0) sName = sName.substr(7);
                        { size_t d = sName.find('.'); if (d != std::string::npos && std::isdigit((unsigned char)sName[d+1])) sName = sName.substr(0, d); }
                        // Try direct name, then walk inheritance hierarchy for __str
                        auto findStr = [&](const std::string& t) -> Function* {
                            Function* f = TheModule->getFunction(t + "___str");
                            if (!f) { std::string sfx = t + "___str"; for (auto& F : *TheModule) if (F.getName().endswith(sfx)) { f = &F; break; } }
                            if (!f) f = TheModule->getFunction(t + "__str");
                            return f;
                        };
                        Function* strFunc = findStr(sName);
                        Value* strSelf = val;
                        if (!strFunc) {
                            std::string cur = sName;
                            while (!strFunc && structHierarchy.count(cur) && !structHierarchy[cur].empty()) {
                                cur = structHierarchy[cur][0];
                                strFunc = findStr(cur);
                                if (strFunc && StructTypes.count(cur))
                                    strSelf = Builder.CreateBitCast(val, PointerType::getUnqual(StructTypes[cur]));
                            }
                        }
                        if (strFunc) {
                            Value* ret = Builder.CreateCall(strFunc, {strSelf});
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
                    if (!f) {
                        // Search for struct-specific match to avoid picking up wrong type's method.
                        std::string target = sName + "___" + suffix.substr(2);
                        for (auto& F : *TheModule)
                            if (F.getName().contains(target)) { f = &F; break; }
                    }
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
                    if (negated) {
                        // __eq returns i32 (Bool widened for C ABI). CreateNot
                        // on i32 1 yields -2; we want 0/1, so compare to 0.
                        result = Builder.CreateICmpEQ(
                            result, ConstantInt::get(result->getType(), 0), "cmp_ne");
                    } else if (result->getType()->isIntegerTy() &&
                               !result->getType()->isIntegerTy(1)) {
                        // Truncate widened i32 Bool to i1 so the result is
                        // typed Bool — matters for boxing/printing.
                        result = Builder.CreateICmpNE(
                            result, ConstantInt::get(result->getType(), 0), "cmp_to_bool");
                    }
                    return result;
                }
            }
        }
        if (binOp->op == "??") {
            if (!L) return R;
            if (!L->getType()->isPointerTy()) return L; // non-pointer can't be null
            Value* isNull = Builder.CreateICmpEQ(
                L, Constant::getNullValue(L->getType()), "coalesce_isnull");
            Value* rhs = R ? R : Constant::getNullValue(L->getType());
            if (rhs->getType() != L->getType()) {
                if (rhs->getType()->isPointerTy())
                    rhs = Builder.CreateBitCast(rhs, L->getType(), "coalesce_cast");
                else
                    rhs = Constant::getNullValue(L->getType());
            }
            return Builder.CreateSelect(isNull, rhs, L, "coalesce");
        }

        return mathGen->generateBinaryOp(binOp->op, L, R);
    }

    if (auto member = dynamic_cast<MemberAccessNode*>(node)) {
        // Enum variant access: Direction.North
        if (auto* lit = dynamic_cast<LiteralNode*>(member->object.get())) {
            if (enumVariants.count(lit->value)) {
                // `EnumName.values` — return a fresh List of the
                // backing values (String/Int) for backed enums, or the
                // variant names as Strings for unbacked. Built lazily
                // each call from the packed global; cheap because the
                // packed bytes are immutable and the List itself is
                // GC-allocated.
                if ((member->memberName == "values" ||
                     member->memberName == "names" ||
                     member->memberName == "variants") && backedEnums.count(lit->value)) {
                    const BackedEnumInfo& info = backedEnums[lit->value];
                    Type* i32 = Type::getInt32Ty(Context);
                    Type* i8p = Type::getInt8PtrTy(Context);
                    Type* listPtrTy = PointerType::getUnqual(StructTypes["List"]);
                    Value* count = ConstantInt::get(i32, info.values.size());

                    // `.names` always walks the names-only packed
                    // global (variant identifiers as Strings), never
                    // the backing values. For unbacked enums the two
                    // blobs are content-identical; emitting both
                    // keeps the codegen branch flat.
                    if (member->memberName == "names") {
                        GlobalVariable* namesGv =
                            TheModule->getNamedGlobal("__" + lit->value + "_names");
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_values_str",
                            FunctionType::get(listPtrTy, {i8p, i32}, false));
                        Value* asI8p = Builder.CreateBitCast(namesGv, i8p);
                        return Builder.CreateCall(fn, {asI8p, count});
                    }

                    // `.variants` — List of ordinals (0..n-1). Each
                    // variant lowers to its i32 ordinal at runtime,
                    // so the "list of variant instances" reduces to
                    // `[0, 1, ..., n-1]` boxed as Any-ints. Reuse
                    // quirk_enum_variants which builds that range
                    // directly, no packed input needed.
                    if (member->memberName == "variants") {
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_variants",
                            FunctionType::get(listPtrTy, {i32}, false));
                        return Builder.CreateCall(fn, {count});
                    }

                    // `.values`
                    GlobalVariable* packedGv = TheModule->getNamedGlobal("__" + lit->value + "_packed");
                    if (info.backingType == "Int") {
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_values_int",
                            FunctionType::get(listPtrTy,
                                {PointerType::getUnqual(i32), i32}, false));
                        Value* asI32p = Builder.CreateBitCast(packedGv, PointerType::getUnqual(i32));
                        return Builder.CreateCall(fn, {asI32p, count});
                    }
                    if (info.backingType == "Double") {
                        Type* dbl = Type::getDoubleTy(Context);
                        FunctionCallee fn = TheModule->getOrInsertFunction(
                            "quirk_enum_values_double",
                            FunctionType::get(listPtrTy,
                                {PointerType::getUnqual(dbl), i32}, false));
                        Value* asDp = Builder.CreateBitCast(packedGv, PointerType::getUnqual(dbl));
                        return Builder.CreateCall(fn, {asDp, count});
                    }
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_values_str",
                        FunctionType::get(listPtrTy, {i8p, i32}, false));
                    Value* asI8p = Builder.CreateBitCast(packedGv, i8p);
                    return Builder.CreateCall(fn, {asI8p, count});
                }
                const auto& variants = enumVariants[lit->value];
                auto it = std::find(variants.begin(), variants.end(), member->memberName);
                if (it != variants.end()) {
                    int idx = (int)std::distance(variants.begin(), it);
                    return ConstantInt::get(Type::getInt32Ty(Context), idx);
                }
            }
        }

        // `instance.ordinal` — at runtime an enum instance IS already
        // an i32 (its declaration-order index), so `.ordinal` is just
        // a pass-through of the underlying value. Handled identically
        // for the three carrier shapes (binding, chained variant,
        // struct field) by evaluating member->object directly. No
        // runtime call needed.
        if (member->memberName == "ordinal") {
            std::string enumName;
            if (auto* lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                auto it = varEnumTypes.find(lit->value);
                if (it != varEnumTypes.end()) enumName = it->second;
            } else if (auto* innerMem = dynamic_cast<MemberAccessNode*>(member->object.get())) {
                if (auto* innerLit = dynamic_cast<LiteralNode*>(innerMem->object.get())) {
                    if (backedEnums.count(innerLit->value)) {
                        enumName = innerLit->value;
                    } else {
                        std::string ownerStruct;
                        if (innerLit->value == "self") ownerStruct = currentCodegenClass;
                        else {
                            Value* v = varGen->resolveVariable(innerLit->value);
                            if (v && v->getType()->isPointerTy() &&
                                v->getType()->getPointerElementType()->isStructTy()) {
                                StructType* st = cast<StructType>(v->getType()->getPointerElementType());
                                std::string sName = st->getName().str();
                                if (sName.find("struct.") == 0) sName = sName.substr(7);
                                ownerStruct = sName;
                            }
                        }
                        if (!ownerStruct.empty()) {
                            auto& fields = structFieldTypes[ownerStruct];
                            auto fIt = fields.find(innerMem->memberName);
                            if (fIt != fields.end() && backedEnums.count(fIt->second))
                                enumName = fIt->second;
                        }
                    }
                }
            }
            if (!enumName.empty()) {
                Value* ord = handleExpression(member->object.get());
                // Defensive cast — varGen may hand back an i8* in some
                // closure-capture paths; coerce to i32 with PtrToInt.
                if (ord->getType()->isPointerTy())
                    ord = Builder.CreatePtrToInt(ord, Type::getInt32Ty(Context));
                return ord;
            }
        }

        // Backed-enum `.value` reverse lookup. Three shapes get here:
        //   g.value              — g is an enum-typed binding tracked
        //                          via varEnumTypes
        //   Gender.Other.value   — chained member access; the inner
        //                          MemberAccess names the enum directly
        //   self.gender.value    — struct field of enum type; looked
        //                          up via structFieldTypes
        if (member->memberName == "value") {
            std::string enumName;
            if (auto* lit = dynamic_cast<LiteralNode*>(member->object.get())) {
                auto it = varEnumTypes.find(lit->value);
                if (it != varEnumTypes.end()) enumName = it->second;
            } else if (auto* innerMem = dynamic_cast<MemberAccessNode*>(member->object.get())) {
                if (auto* innerLit = dynamic_cast<LiteralNode*>(innerMem->object.get())) {
                    if (backedEnums.count(innerLit->value)) {
                        enumName = innerLit->value;  // Gender.Other.value
                    } else {
                        // Probably struct-field access: object.field.value.
                        // Resolve via the owning struct's field-type map.
                        // For `self.field`, the struct is currentCodegenClass.
                        // For `x.field`, look up x's type in varEnumTypes
                        // (handles e.g. `user := User(...); user.gender.value`).
                        std::string ownerStruct;
                        if (innerLit->value == "self") {
                            ownerStruct = currentCodegenClass;
                        } else {
                            auto t = varEnumTypes.find(innerLit->value);
                            if (t == varEnumTypes.end()) {
                                // Fallback: query varGen for the declared type
                                Value* v = varGen->resolveVariable(innerLit->value);
                                if (v && v->getType()->isPointerTy() &&
                                    v->getType()->getPointerElementType()->isStructTy()) {
                                    StructType* st = cast<StructType>(v->getType()->getPointerElementType());
                                    std::string sName = st->getName().str();
                                    if (sName.find("struct.") == 0) sName = sName.substr(7);
                                    ownerStruct = sName;
                                }
                            }
                        }
                        if (!ownerStruct.empty()) {
                            auto& fields = structFieldTypes[ownerStruct];
                            auto fIt = fields.find(innerMem->memberName);
                            if (fIt != fields.end() && backedEnums.count(fIt->second))
                                enumName = fIt->second;
                        }
                    }
                }
            }
            if (!enumName.empty() && backedEnums.count(enumName)) {
                const BackedEnumInfo& info = backedEnums[enumName];
                Value* ordinal = handleExpression(member->object.get());
                Type* i32 = Type::getInt32Ty(Context);
                Type* i8p = Type::getInt8PtrTy(Context);
                GlobalVariable* packedGv = TheModule->getNamedGlobal("__" + enumName + "_packed");
                Value* packedPtr = Builder.CreateBitCast(packedGv, i8p);
                Value* count     = ConstantInt::get(i32, info.values.size());
                if (info.backingType == "Int") {
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_value_int",
                        FunctionType::get(i32, {i32, i8p, i32}, false));
                    return Builder.CreateCall(fn, {ordinal, packedPtr, count});
                }
                if (info.backingType == "Double") {
                    Type* dbl = Type::getDoubleTy(Context);
                    FunctionCallee fn = TheModule->getOrInsertFunction(
                        "quirk_enum_value_double",
                        FunctionType::get(dbl, {i32, PointerType::getUnqual(dbl), i32}, false));
                    Value* packedD = Builder.CreateBitCast(packedGv, PointerType::getUnqual(dbl));
                    return Builder.CreateCall(fn, {ordinal, packedD, count});
                }
                FunctionCallee fn = TheModule->getOrInsertFunction(
                    "quirk_enum_value_str",
                    FunctionType::get(
                        PointerType::getUnqual(StructTypes["String"]),
                        {i32, i8p, i32}, false));
                return Builder.CreateCall(fn, {ordinal, packedPtr, count});
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

        // Numeric tuple field access: `t.0`, `t.1`, ... Equivalent to
        // `t[0]` but using dot-notation. We detect it before the generic
        // member-access path so a Tuple's `.N` calls the runtime helper
        // rather than searching for a named field that doesn't exist.
        // Also handles i8* (Any) receivers — the common case of
        // `pairs.get(i).0` where `pairs: List<Tuple>`. At runtime the
        // i8* points at a Tuple struct, so a bitcast is safe; if it
        // doesn't, `Core_Collections_Tuple_Tuple___get` returns NULL
        // for out-of-bounds and the caller sees null instead of UB.
        if (objPtr && !member->memberName.empty() &&
            std::all_of(member->memberName.begin(), member->memberName.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
            StructTypes.count("Tuple")) {
            Type* tuplePtrTy = PointerType::getUnqual(StructTypes["Tuple"]);
            Type* elt = objPtr->getType()->isPointerTy()
                ? objPtr->getType()->getPointerElementType()
                : nullptr;
            bool isTuple = elt && elt == StructTypes["Tuple"];
            bool isOpaqueAny = elt && elt->isIntegerTy(8);
            if (isTuple || isOpaqueAny) {
                Function* getFn = TheModule->getFunction("Core_Collections_Tuple_Tuple___get");
                if (!getFn) {
                    FunctionType* ft = FunctionType::get(Type::getInt8PtrTy(Context),
                        {tuplePtrTy, Type::getInt32Ty(Context)}, false);
                    getFn = Function::Create(ft, Function::ExternalLinkage,
                        "Core_Collections_Tuple_Tuple___get", TheModule.get());
                }
                Value* tupPtr = isOpaqueAny
                    ? Builder.CreateBitCast(objPtr, tuplePtrTy, "any_as_tuple")
                    : objPtr;
                int idx = std::stoi(member->memberName);
                return Builder.CreateCall(getFn,
                    {tupPtr, ConstantInt::get(Type::getInt32Ty(Context), idx)},
                    "tup_" + member->memberName);
            }
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

        // Safe field access: obj?.field — emit a null-branch so a null obj
        // produces null-of-field-type instead of a segfault. Mirrors the
        // safe-call path above for obj?.method().
        if (member->isSafeAccess && objPtr && objPtr->getType()->isPointerTy()) {
            Function* parentFunc = Builder.GetInsertBlock()->getParent();
            BasicBlock* preBB    = Builder.GetInsertBlock();
            BasicBlock* doReadBB = BasicBlock::Create(Context, "safe_field", parentFunc);
            BasicBlock* afterBB  = BasicBlock::Create(Context, "safe_field_after");
            Builder.CreateCondBr(
                Builder.CreateICmpEQ(objPtr, Constant::getNullValue(objPtr->getType()), "safe_field_isnull"),
                afterBB, doReadBB);

            Builder.SetInsertPoint(doReadBB);
            Value* val = structGen->generateMemberAccess(objPtr, member->memberName);
            BasicBlock* readEndBB = Builder.GetInsertBlock();
            if (!readEndBB->getTerminator()) Builder.CreateBr(afterBB);

            parentFunc->getBasicBlockList().push_back(afterBB);
            Builder.SetInsertPoint(afterBB);
            if (val && !val->getType()->isVoidTy()) {
                PHINode* phi = Builder.CreatePHI(val->getType(), 2, "safe_field_result");
                phi->addIncoming(Constant::getNullValue(val->getType()), preBB);
                phi->addIncoming(val, readEndBB);
                return phi;
            }
            return nullptr;
        }

        Value* fieldVal = structGen->generateMemberAccess(objPtr, member->memberName);
        if (fieldVal) return fieldVal;

        // Property accessor: `obj.name` (no parens) — when no field of
        // that name exists, look for a zero-arg method and auto-call it.
        // Lets users define computed properties like `area`, `is_done`,
        // `length`, etc. without parens at the call site. Methods with
        // arguments still require `obj.method(args)`.
        if (objPtr && objPtr->getType()->isPointerTy()
            && objPtr->getType()->getPointerElementType()->isStructTy()) {
            std::string structName = cast<StructType>(objPtr->getType()->getPointerElementType())->getName().str();
            if (structName.rfind("struct.", 0) == 0) structName = structName.substr(7);
            // Resolve <Struct>_<name> first, then the magic-method form.
            Function* propFn = TheModule->getFunction(structName + "_" + member->memberName);
            if (!propFn) propFn = resolveFunction(structName + "_" + member->memberName);
            if (!propFn) propFn = TheModule->getFunction(structName + "___" + member->memberName);
            if (!propFn) propFn = resolveFunction(structName + "___" + member->memberName);
            // Only auto-call zero-arg (the `self` param is the one arg).
            if (propFn && propFn->arg_size() == 1) {
                Value* arg = objPtr;
                Type* expected = propFn->getFunctionType()->getParamType(0);
                if (arg->getType() != expected && arg->getType()->isPointerTy() && expected->isPointerTy()) {
                    arg = Builder.CreateBitCast(arg, expected);
                }
                return Builder.CreateCall(propFn, {arg}, member->memberName);
            }
        }
        return nullptr;
    }

    if (auto c = dynamic_cast<ConstructorNode*>(node)) return handleConstructor(c);

    if (auto tern = dynamic_cast<TernaryNode*>(node)) {
        Value* cond = handleExpression(tern->condition.get());
        if (!cond) return nullptr;

        Value* condBool;
        if (cond->getType()->isIntegerTy(1))
            condBool = cond;
        else if (cond->getType()->isPointerTy())
            condBool = Builder.CreateICmpNE(cond, Constant::getNullValue(cond->getType()), "tern_cond");
        else if (cond->getType()->isIntegerTy())
            condBool = Builder.CreateICmpNE(cond, ConstantInt::get(cond->getType(), 0), "tern_cond");
        else if (cond->getType()->isDoubleTy())
            condBool = Builder.CreateFCmpONE(cond, ConstantFP::get(cond->getType(), 0.0), "tern_cond");
        else
            condBool = cond;

        Function* parentFunc = Builder.GetInsertBlock()->getParent();
        BasicBlock* thenBB  = BasicBlock::Create(Context, "tern_then", parentFunc);
        BasicBlock* elseBB  = BasicBlock::Create(Context, "tern_else");
        BasicBlock* mergeBB = BasicBlock::Create(Context, "tern_merge");

        Builder.CreateCondBr(condBool, thenBB, elseBB);

        Builder.SetInsertPoint(thenBB);
        Value* thenVal = handleExpression(tern->thenExpr.get());
        BasicBlock* thenEndBB = Builder.GetInsertBlock();
        if (!thenEndBB->getTerminator()) Builder.CreateBr(mergeBB);

        parentFunc->getBasicBlockList().push_back(elseBB);
        Builder.SetInsertPoint(elseBB);
        Value* elseVal = handleExpression(tern->elseExpr.get());
        BasicBlock* elseEndBB = Builder.GetInsertBlock();
        if (!elseEndBB->getTerminator()) Builder.CreateBr(mergeBB);

        parentFunc->getBasicBlockList().push_back(mergeBB);
        Builder.SetInsertPoint(mergeBB);

        if (!thenVal || !elseVal || thenVal->getType()->isVoidTy() || elseVal->getType()->isVoidTy())
            return nullptr;

        Type* thenTy = thenVal->getType();
        Type* elseTy = elseVal->getType();
        if (thenTy != elseTy && thenTy->isPointerTy() && elseTy->isPointerTy())
            elseVal = Builder.CreateBitCast(elseVal, thenTy, "tern_cast");

        PHINode* phi = Builder.CreatePHI(thenVal->getType(), 2, "tern_result");
        phi->addIncoming(thenVal, thenEndBB);
        phi->addIncoming(elseVal, elseEndBB);
        return phi;
    }

    if (auto sl = dynamic_cast<SliceNode*>(node)) {
        Value* objVal = handleExpression(sl->object.get());
        if (!objVal) return nullptr;

        Type* i32Ty    = Type::getInt32Ty(Context);
        Type* i8PtrTy  = Type::getInt8PtrTy(Context);

        // Determine object type: String or List
        bool isListSlice = false;
        if (objVal->getType()->isPointerTy()) {
            Type* pointee = objVal->getType()->getPointerElementType();
            if (pointee->isStructTy()) {
                StringRef name = cast<StructType>(pointee)->getName();
                if (name.contains("List") || name == "List") isListSlice = true;
            }
        }
        // Also check if the sema type was List via the object expression type
        // (fallback: if object holds a List*, treat as list)

        Value* startVal = sl->start
            ? handleExpression(sl->start.get())
            : ConstantInt::get(i32Ty, 0);
        if (!startVal) return nullptr;
        if (!startVal->getType()->isIntegerTy(32))
            startVal = Builder.CreateIntCast(startVal, i32Ty, true);

        if (isListSlice) {
            // List slice: Core_Collections_List_List_slice(List*, start, end)
            // end==-1 is handled by passing size; use INT_MIN (-2147483648) as "no end"
            // Actually: just pass -1 and let runtime clamp (end<0 → end+=size → size-1).
            // Better: use the List size field. List struct: {void** data, int size, int cap}
            StructType* listTy = StructTypes.count("List") ? StructTypes["List"] : nullptr;
            Type* listPtrTy = listTy ? (Type*)PointerType::getUnqual(listTy) : i8PtrTy;
            if (objVal->getType() != listPtrTy)
                objVal = Builder.CreateBitCast(objVal, listPtrTy);

            Value* endVal;
            if (sl->end) {
                endVal = handleExpression(sl->end.get());
                if (!endVal) return nullptr;
                if (!endVal->getType()->isIntegerTy(32))
                    endVal = Builder.CreateIntCast(endVal, i32Ty, true);
            } else if (listTy) {
                // Load size field (index 1 in {void**, int, int})
                Value* sizePtr = Builder.CreateGEP(listTy, objVal,
                    {ConstantInt::get(i32Ty, 0), ConstantInt::get(i32Ty, 1)}, "list_sizeptr");
                endVal = Builder.CreateLoad(i32Ty, sizePtr, "list_size");
            } else {
                endVal = ConstantInt::getSigned(i32Ty, -1);
            }

            FunctionCallee sliceFn = TheModule->getOrInsertFunction(
                "Core_Collections_List_List_slice",
                listPtrTy, listPtrTy, i32Ty, i32Ty);
            return Builder.CreateCall(sliceFn, {objVal, startVal, endVal}, "list_slice");

        } else {
            // String slice: Core_String_String_substring(String*, start, end)
            StructType* strTy = StructTypes.count("String") ? StructTypes["String"] : nullptr;
            if (!strTy) strTy = StructType::create(Context, {i8PtrTy, i32Ty}, "String");
            Type* strPtrTy = PointerType::getUnqual(strTy);
            if (objVal->getType() != strPtrTy)
                objVal = Builder.CreateBitCast(objVal, strPtrTy);

            Value* endVal;
            if (sl->end) {
                endVal = handleExpression(sl->end.get());
                if (!endVal) return nullptr;
                if (!endVal->getType()->isIntegerTy(32))
                    endVal = Builder.CreateIntCast(endVal, i32Ty, true);
            } else {
                Value* lenPtr = Builder.CreateGEP(strTy, objVal,
                    {ConstantInt::get(i32Ty, 0), ConstantInt::get(i32Ty, 1)}, "str_lenptr");
                endVal = Builder.CreateLoad(i32Ty, lenPtr, "str_len");
            }

            FunctionCallee substringFn = TheModule->getOrInsertFunction(
                "Core_String_String_substring",
                strPtrTy, strPtrTy, i32Ty, i32Ty);
            return Builder.CreateCall(substringFn, {objVal, startVal, endVal}, "slice");
        }
    }

    return nullptr;
}
std::string LLVMCodegen::currentCodegenClass = "";