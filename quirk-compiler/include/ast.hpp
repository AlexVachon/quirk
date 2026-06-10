#ifndef AST_HPP
#define AST_HPP

// Set by `--diagnostics-json` (defined in Compiler.cpp). When true, every
// error printer in the compiler (Parser, Sema) writes one NDJSON record
// per diagnostic to stdout instead of the human-readable ANSI output to
// stderr. Designed for tools that consume errors structurally — chiefly
// the v1.6+ LSP server, which spawns `quirk --check --diagnostics-json`
// and turns each line into a `Diagnostic`.
extern bool g_diagnostics_json;

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Node {
   public:
    std::string moduleName;
    std::string filePath;
    int line = 0;
    int col  = 0;

    virtual ~Node() = default;
    virtual void print(int indent) const = 0;
};

class UseNode : public Node {
   public:
    std::string moduleName;
    std::vector<std::string> filterList;
    std::string alias; // from .path as alias

    UseNode(std::string mod, std::vector<std::string> filters = {}, std::string alias = "")
        : moduleName(mod), filterList(filters), alias(alias) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Use: " << moduleName;
        if (!alias.empty()) std::cout << " as " << alias;
        if (!filterList.empty()) {
            std::cout << " (Only: ";
            for (auto& s : filterList) std::cout << s << " ";
            std::cout << ")";
        }
        std::cout << std::endl;
    }
};

class WithNode : public Node {
   public:
    std::unique_ptr<Node> resource;
    std::string varName;
    std::vector<std::unique_ptr<Node>> body;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "With Resource as " << varName << " {"
                  << std::endl;
        resource->print(indent + 2);
        std::cout << space << " Body:" << std::endl;
        for (const auto& stmt : body) {
            stmt->print(indent + 4);
        }
        std::cout << space << "}" << std::endl;
    }
};

struct Parameter {
    std::string name;
    std::string type;
    bool isRef = false;
    bool isVariadic = false;
    std::unique_ptr<Node> defaultValue = nullptr;
};

class FunctionNode : public Node {
   public:
    std::string name;
    std::string cls;
    std::string returnType = "void";
    std::vector<Parameter> parameters;
    std::vector<std::unique_ptr<Node>> body;

    bool isExtern = false;
    bool isStatic = false;
    bool isAbstract = false;             // true for interface method signatures (no body)

    std::string linkageName;
    std::unique_ptr<Node> whereClause;
    std::vector<std::string> typeParams; // generic type params, e.g. ["T", "U"]
    // where T: Interface1 & Interface2 — maps type param name → required interfaces
    std::map<std::string, std::vector<std::string>> genericConstraints;

    // Python-style decorators: `@a` or `@a(args)` lines stacked above the
    // `define` line. Stored bottom-up in source order; applied top-down at
    // codegen time so `@a \n @b \n define f` produces `f := a(b(f__inner))`.
    std::vector<std::unique_ptr<Node>> decorators;

    // True when this FunctionNode is the *wrapper* synthesized by the parser
    // for a decorated function. The wrapper's body is built specially by
    // Codegen: it lazily evaluates `decoratorChainExpr` once, caches the
    // resulting Callable in a module-internal global, then dispatches through
    // that cached Callable on every call. Lets stateful decorators (e.g.
    // `@cached`, `@retry`) keep state across invocations.
    bool isDecoratorWrapper = false;
    std::unique_ptr<Node> decoratorChainExpr;  // e.g. `a(b(foo__inner__))`


    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << (isExtern ? "Extern " : "")
                  << "Function: " << name << "(";
        for (size_t i = 0; i < parameters.size(); ++i) {
            std::cout << (parameters[i].isRef ? "ref " : "")
                      << parameters[i].name << ": ";

            if (parameters[i].name == "self" && parameters[i].type.empty() &&
                !cls.empty()) {
                std::cout << cls;
            } else {
                std::cout << parameters[i].type;
            }

            if (i < parameters.size() - 1)
                std::cout << ", ";
        }
        std::cout << ") {" << std::endl;
        for (const auto& node : body)
            node->print(indent + 2);
        std::cout << space << "}" << std::endl;
    }
};

class ReturnNode : public Node {
   public:
    std::unique_ptr<Node> expression;

    ReturnNode(std::unique_ptr<Node> expr) : expression(std::move(expr)) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Return:" << std::endl;
        if (expression)
            expression->print(indent + 2);
    }
};

class StructField {
   public:
    std::string name;
    std::string type;
    std::unique_ptr<Node> defaultValue;
};

class StructNode : public Node {
   public:
    std::string name;
    std::vector<std::string> parents;      // struct inheritance chain
    std::vector<std::string> interfaces;   // interface conformances (populated by Sema)
    std::vector<std::string> typeParams;   // generic type params, e.g. ["T"]
    std::map<std::string, std::vector<std::string>> genericConstraints; // where T: Interface
    std::vector<StructField> fields;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Struct: " << name;
        if (!parents.empty()) {
            std::cout << " (uses: ";
            for (size_t i = 0; i < parents.size(); ++i) {
                std::cout << parents[i] << (i < parents.size() - 1 ? ", " : "");
            }
            std::cout << ")";
        }
        std::cout << " {" << std::endl;
        for (const auto& f : fields)
            std::cout << space << "  " << f.name << ": " << f.type << std::endl;
        std::cout << space << "}" << std::endl;
    }
};

struct ConstructorArg {
    std::string fieldName;
    std::unique_ptr<Node> value;
};

class ConstructorNode : public Node {
   public:
    std::string structName;
    std::vector<ConstructorArg> args;

    ConstructorNode(std::string name) : structName(name) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Constructor: " << structName << "(" << std::endl;
        for (const auto& a : args) {
            std::cout << space << "  " << a.fieldName << ":" << std::endl;
            a.value->print(indent + 4);
        }
        std::cout << space << ")" << std::endl;
    }
};

class MemberAccessNode : public Node {
   public:
    std::unique_ptr<Node> object;
    std::string memberName;
    bool isSafeAccess = false;  // true when accessed via ?.

    MemberAccessNode(std::unique_ptr<Node> obj, std::string member)
        : object(std::move(obj)), memberName(member) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "MemberAccess: " << (isSafeAccess ? "?." : ".") << memberName << std::endl;
        object->print(indent + 2);
    }
};

class VarDeclNode : public Node {
   public:
    std::unique_ptr<Node> lhs;
    std::unique_ptr<Node> expression;
    std::string op;
    std::string typeAnnotation;
    // Type Sema settled on after checking the RHS, when the user
    // didn't write an explicit `typeAnnotation`. Used by the
    // `--symbols-json` walker so the LSP can emit inlay hints next
    // to `:=` bindings. Leaves `typeAnnotation` untouched so the AST
    // still reflects what the user actually typed.
    std::string inferredType;
    bool isConst = false;

    VarDeclNode(std::unique_ptr<Node> left,
                std::unique_ptr<Node> expr,
                std::string operation = ":=",
                std::string type = "")
        : lhs(std::move(left)),
          expression(std::move(expr)),
          op(operation),
          typeAnnotation(type) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Assignment/Decl (" << op << ")";
        if (!typeAnnotation.empty()) {
            std::cout << " [Type: " << typeAnnotation << "]";
        }
        std::cout << ":" << std::endl;
        lhs->print(indent + 2);
        expression->print(indent + 2);
    }
};

struct Arg {
    std::string name;
    std::unique_ptr<Node> value;
    bool isSpread = false; // true for ...expr spread argument
};

class CallNode : public Node {
   public:
    std::unique_ptr<Node> callee;
    std::vector<Arg> args;

    CallNode(std::unique_ptr<Node> callTarget)
        : callee(std::move(callTarget)) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Call:" << std::endl;
        callee->print(indent + 2);
        std::cout << space << "  Args:" << std::endl;
        for (const auto& arg : args) {
            if (!arg.name.empty())
                std::cout << space << "    " << arg.name << ": " << std::endl;
            arg.value->print(indent + 4);
        }
    }
};

class BinaryOpNode : public Node {
   public:
    std::string op;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;

    BinaryOpNode(std::string op,
                 std::unique_ptr<Node> l,
                 std::unique_ptr<Node> r)
        : op(op), left(std::move(l)), right(std::move(r)) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "BinaryOp (" << op << "):" << std::endl;
        left->print(indent + 2);
        right->print(indent + 2);
    }
};

class LiteralNode : public Node {
   public:
    std::string value;
    LiteralNode(std::string val) : value(val) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Literal: " << value
                  << std::endl;
    }
};

class TupleLiteralNode : public Node {
   public:
    std::vector<std::unique_ptr<Node>> elements;

    TupleLiteralNode(std::vector<std::unique_ptr<Node>> elems)
        : elements(std::move(elems)) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "TupleLiteral: (" << std::endl;
        for (const auto& elem : elements)
            elem->print(indent + 2);
        std::cout << std::string(indent, ' ') << ")" << std::endl;
    }
};

class ListLiteralNode : public Node {
   public:
    std::vector<std::unique_ptr<Node>> elements;

    ListLiteralNode(std::vector<std::unique_ptr<Node>> elems)
        : elements(std::move(elems)) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "ListLiteral: [" << std::endl;
        for (const auto& elem : elements) {
            elem->print(indent + 2);
        }
        std::cout << std::string(indent, ' ') << "]" << std::endl;
    }
};

struct MapLiteralNode : public Node {
    // Stores pairs of (Key, Value)
    std::vector<std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>>> elements;

    MapLiteralNode() = default;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "MapLiteral: {" << std::endl;
        for (const auto& pair : elements) {
            std::cout << space << "  Key:" << std::endl;
            pair.first->print(indent + 4);
            std::cout << space << "  Value:" << std::endl;
            pair.second->print(indent + 4);
        }
        std::cout << space << "}" << std::endl;
    }
};

struct SetLiteralNode : public Node {
    std::vector<std::unique_ptr<Node>> elements;

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "SetLiteral: {" << elements.size() << " elems}" << std::endl;
    }
};

struct TypeAliasNode : public Node {
    std::string name;
    std::string target; // target type name

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "TypeAlias: " << name << " = " << target << std::endl;
    }
};

// Tagged-union declaration (v2.4 / v3 phase 2). Each variant is a
// named constructor with zero or more typed payload fields. Each
// variant lowers in Codegen to a struct whose first field is a tag
// (`__type_id`), inheriting from a virtual base struct named after
// the union itself — so `match` dispatches via the same vtable
// machinery already used for `case Subclass =>` arms on user
// structs. See CHANGELOG v2.4.0 / project_tagged_unions.md.
struct TaggedUnionVariant {
    std::string name;                                          // e.g. "Ok"
    std::vector<std::pair<std::string, std::string>> fields;   // (name, type)
};

struct TaggedUnionDeclNode : public Node {
    std::string name;                          // e.g. "Result"
    std::vector<TaggedUnionVariant> variants;

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "TaggedUnion: " << name << " =";
        for (size_t i = 0; i < variants.size(); i++) {
            std::cout << (i == 0 ? " " : " | ") << variants[i].name << "(";
            for (size_t j = 0; j < variants[i].fields.size(); j++) {
                if (j > 0) std::cout << ", ";
                std::cout << variants[i].fields[j].first << ": " << variants[i].fields[j].second;
            }
            std::cout << ")";
        }
        std::cout << std::endl;
    }
};

struct ListComprehensionNode : public Node {
    std::unique_ptr<Node> expr;
    std::string varName;
    std::string varName2; // optional second var for pair iteration: for k, v in iterable
    std::unique_ptr<Node> iterable;
    std::unique_ptr<Node> condition; // nullable — the 'where' clause

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::string vars = varName2.empty() ? varName : varName + ", " + varName2;
        std::cout << sp << "ListComprehension: [expr for " << vars << " in ...]" << std::endl;
    }
};

struct MapComprehensionNode : public Node {
    std::unique_ptr<Node> keyExpr;
    std::unique_ptr<Node> valExpr;
    std::string varName;
    std::string varName2; // optional second var for pair iteration: for k, v in iterable
    std::unique_ptr<Node> iterable;
    std::unique_ptr<Node> condition; // nullable

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::string vars = varName2.empty() ? varName : varName + ", " + varName2;
        std::cout << sp << "MapComprehension: {k: v for " << vars << " in ...}" << std::endl;
    }
};

struct ElIfBlock {
    std::unique_ptr<Node> condition;
    std::vector<std::unique_ptr<Node>> body;
};

class IfNode : public Node {
   public:
    std::unique_ptr<Node> condition;
    std::vector<std::unique_ptr<Node>> thenBranch;
    std::vector<ElIfBlock> elIfBranches;
    std::vector<std::unique_ptr<Node>> elseBranch;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "If (" << std::endl;
        condition->print(indent + 2);
        std::cout << space << ") Then {" << std::endl;
        for (const auto& stmt : thenBranch)
            stmt->print(indent + 2);
        std::cout << space << "}";
        if (!elseBranch.empty()) {
            std::cout << " Else {" << std::endl;
            for (const auto& stmt : elseBranch)
                stmt->print(indent + 2);
            std::cout << space << "}";
        }
        std::cout << std::endl;
    }
};

class WhileNode : public Node {
   public:
    std::unique_ptr<Node> condition;
    std::vector<std::unique_ptr<Node>> body;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "While (" << std::endl;
        condition->print(indent + 2);
        std::cout << space << ") {" << std::endl;
        for (const auto& stmt : body)
            stmt->print(indent + 2);
        std::cout << space << "}" << std::endl;
    }
};

class ForNode : public Node {
   public:
    std::string varName;
    std::string varName2; // optional second variable for pair iteration (k, v in map)
    std::vector<std::string> destructureVars; // for (a, b) in tupleList
    bool isRef;
    std::unique_ptr<Node> iterable;
    std::vector<std::unique_ptr<Node>> body;

    ForNode(std::string name, bool ref, std::unique_ptr<Node> iter)
        : varName(name), isRef(ref), iterable(std::move(iter)) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "For " << (isRef ? "ref " : "") << varName
                  << " in " << std::endl;
        iterable->print(indent + 2);
        std::cout << space << " {" << std::endl;
        for (const auto& stmt : body)
            stmt->print(indent + 2);
        std::cout << space << "}" << std::endl;
    }
};

class DeleteNode : public Node {
   public:
    std::unique_ptr<Node> target;
    DeleteNode(std::unique_ptr<Node> t) : target(std::move(t)) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Delete:" << std::endl;
        target->print(indent + 2);
    }
};

struct CatchBlock {
    std::string varName;
    std::vector<std::string> types;
    std::vector<std::unique_ptr<Node>> body;
};

class TryCatchNode : public Node {
   public:
    std::vector<std::unique_ptr<Node>> tryBlock;
    std::vector<CatchBlock> catchBlocks;
    std::vector<std::unique_ptr<Node>> finallyBlock;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Try {" << std::endl;
        for (const auto& stmt : tryBlock) stmt->print(indent + 2);
        for (const auto& cb : catchBlocks) {
            std::cout << space << "} Catch (" << cb.varName << ": ";
            for (size_t i = 0; i < cb.types.size(); ++i) {
                std::cout << cb.types[i] << (i < cb.types.size() - 1 ? ", " : "");
            }
            std::cout << ") {" << std::endl;
            for (const auto& stmt : cb.body) stmt->print(indent + 2);
        }
        if (!finallyBlock.empty()) {
            std::cout << space << "} Finally {" << std::endl;
            for (const auto& stmt : finallyBlock) stmt->print(indent + 2);
        }
        std::cout << space << "}" << std::endl;
    }
};

class ThrowNode : public Node {
   public:
    std::unique_ptr<Node> expression;
    std::unique_ptr<Node> cause;
    int line;
    std::string moduleName;

    ThrowNode(std::unique_ptr<Node> expr, std::unique_ptr<Node> causeNode, int l)
        : expression(std::move(expr)), cause(std::move(causeNode)), line(l) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        if (expression) {
            std::cout << space << "Throw (Line " << line << "):" << std::endl;
            expression->print(indent + 2);
        } else {
            std::cout << space << "Rethrow (Line " << line << ")" << std::endl;
        }
    }
};

class BreakNode : public Node {
   public:
    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Break" << std::endl;
    }
};

class ContinueNode : public Node {
   public:
    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Continue" << std::endl;
    }
};


struct LambdaParam {
    std::string name;
    std::string type; // empty = untyped
    bool isVariadic = false;
};

class LambdaNode : public Node {
   public:
    std::vector<LambdaParam> params;
    std::unique_ptr<Node> exprBody;                    // set for fn(x) => expr
    std::vector<std::unique_ptr<Node>> stmtBody;       // set for fn(x) { stmts }
    bool isExpression = true;
    std::string declaredReturnType;                    // user-annotated `fn(x) -> T`; empty if omitted
    std::string inferredReturnType;                    // set by Sema; empty = opaque i8*

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Lambda(";
        for (size_t i = 0; i < params.size(); i++) {
            std::cout << params[i].name;
            if (!params[i].type.empty()) std::cout << ": " << params[i].type;
            if (i + 1 < params.size()) std::cout << ", ";
        }
        std::cout << ") =>" << std::endl;
        if (isExpression && exprBody) exprBody->print(indent + 2);
        else for (const auto& s : stmtBody) s->print(indent + 2);
    }
};

class EnumNode : public Node {
   public:
    std::string name;
    std::vector<std::string> variants;   // in declaration order
    // Backed enums: `enum Name(String) { V1, V2 = "x" }`. Empty means
    // the legacy unbacked enum (ordinal-only). When set, the enum is
    // callable as a value-→ordinal lookup: `Name("x")` returns the
    // matching variant or throws ValueError. `.value` on an instance
    // returns the backing value. Supported: "String", "Int".
    std::string backingType;
    // Parallel to `variants`. Each entry is the variant's backing
    // literal as written in source (without quotes, e.g. `"male"` is
    // stored as `male`). An empty entry means "use the default" —
    // for String, that's the variant name as-written; for Int, the
    // variant's ordinal index.
    std::vector<std::string> variantValues;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Enum: " << name;
        if (!backingType.empty()) std::cout << "(" << backingType << ")";
        std::cout << " {";
        for (size_t i = 0; i < variants.size(); i++) {
            std::cout << " " << variants[i];
            if (i < variantValues.size() && !variantValues[i].empty())
                std::cout << "=" << variantValues[i];
        }
        std::cout << " }" << std::endl;
    }
};

struct MatchArm {
    std::vector<std::unique_ptr<Node>> patterns;  // expressions; empty when isWildcard
    bool isWildcard = false;
    bool isTypeMatch = false;        // true for `case Int =>` type-dispatch
    std::vector<std::string> typeNames; // types to match when isTypeMatch
    std::string bindName;            // optional `case Int as x =>` binding
    // Tuple destructure: `case (a, b) =>` rewrites to a wildcard arm with
    // bindNames = ["a", "b"]. Each name is bound to the corresponding
    // element of the scrutinee tuple at codegen time. Empty when not a
    // tuple-destructure arm.
    std::vector<std::string> bindNames;
    // `true` for `case [a, b] =>` list destructure (vs `case (a, b)`).
    // Codegen reads through List.get(i) instead of Tuple.get(i).
    bool bindsList = false;
    // Optional `if cond` guard. The arm fires only if the pattern matches
    // AND `guard` evaluates truthy. Lets `case x if x > 0 => …` work.
    std::unique_ptr<Node> guard;
    std::vector<std::unique_ptr<Node>> body;
};

class MatchNode : public Node {
   public:
    std::unique_ptr<Node> scrutinee;
    std::vector<MatchArm> arms;

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Match (" << std::endl;
        scrutinee->print(indent + 2);
        std::cout << space << ") {" << std::endl;
        for (const auto& arm : arms) {
            if (arm.isWildcard) {
                std::cout << space << "  case _:" << std::endl;
            } else {
                std::cout << space << "  case ";
                for (size_t i = 0; i < arm.patterns.size(); ++i) {
                    arm.patterns[i]->print(0);
                    if (i + 1 < arm.patterns.size()) std::cout << ", ";
                }
                std::cout << ":" << std::endl;
            }
            for (const auto& s : arm.body) s->print(indent + 4);
        }
        std::cout << space << "}" << std::endl;
    }
};

class SliceNode : public Node {
   public:
    std::unique_ptr<Node> object;
    std::unique_ptr<Node> start;  // nullptr means 0
    std::unique_ptr<Node> end;    // nullptr means length

    SliceNode(std::unique_ptr<Node> obj, std::unique_ptr<Node> s, std::unique_ptr<Node> e)
        : object(std::move(obj)), start(std::move(s)), end(std::move(e)) {}

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "Slice[\n";
        object->print(indent + 2);
        std::cout << sp << "  start: "; if (start) start->print(0); else std::cout << "0\n";
        std::cout << sp << "  end:   "; if (end)   end->print(0);   else std::cout << "len\n";
        std::cout << sp << "]\n";
    }
};

class TernaryNode : public Node {
   public:
    std::unique_ptr<Node> condition;
    std::unique_ptr<Node> thenExpr;
    std::unique_ptr<Node> elseExpr;

    TernaryNode(std::unique_ptr<Node> cond, std::unique_ptr<Node> thn, std::unique_ptr<Node> els)
        : condition(std::move(cond)), thenExpr(std::move(thn)), elseExpr(std::move(els)) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "Ternary (\n";
        condition->print(indent + 2);
        std::cout << space << "  ?\n";
        thenExpr->print(indent + 2);
        std::cout << space << "  :\n";
        elseExpr->print(indent + 2);
        std::cout << space << ")\n";
    }
};

// Range literal: start..end  (used in `for x in 0..10`)
struct RangeLiteralNode : public Node {
    std::unique_ptr<Node> start;
    std::unique_ptr<Node> end;

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "Range(\n";
        start->print(indent + 2);
        std::cout << sp << "  ..\n";
        end->print(indent + 2);
        std::cout << sp << ")\n";
    }
};

// interface Printable { define __str(self) -> String }
class InterfaceNode : public Node {
   public:
    std::string name;
    std::vector<std::string> extends;                        // other interfaces this extends
    std::vector<std::unique_ptr<FunctionNode>> methods;      // abstract method signatures

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << "Interface: " << name;
        if (!extends.empty()) {
            std::cout << " : ";
            for (size_t i = 0; i < extends.size(); i++)
                std::cout << extends[i] << (i + 1 < extends.size() ? ", " : "");
        }
        std::cout << " {" << std::endl;
        for (const auto& m : methods) m->print(indent + 2);
        std::cout << sp << "}" << std::endl;
    }
};

// nonlocal x, y  — marks variables as shared mutable cells with enclosing scope
struct NonlocalNode : public Node {
    std::vector<std::string> vars;
    bool isGlobal = false; // true for `global x` (currently a no-op)

    void print(int indent) const override {
        std::string sp(indent, ' ');
        std::cout << sp << (isGlobal ? "Global" : "Nonlocal") << ": ";
        for (size_t i = 0; i < vars.size(); i++)
            std::cout << vars[i] << (i + 1 < vars.size() ? ", " : "");
        std::cout << "\n";
    }
};

#endif