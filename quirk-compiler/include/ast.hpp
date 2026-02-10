#ifndef AST_HPP
#define AST_HPP

#include <iostream>
#include <memory>
#include <string>
#include <vector>

class Node {
   public:
    virtual ~Node() = default;
    virtual void print(int indent) const = 0;
};

class UseNode : public Node {
   public:
    std::string moduleName;
    std::string subTarget;

    UseNode(std::string mod, std::string sub = "")
        : moduleName(mod), subTarget(sub) {}

    void print(int indent) const override {
        std::cout << std::string(indent, ' ') << "Use: " << moduleName;
        if (!subTarget.empty())
            std::cout << "." << subTarget;
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
    std::vector<std::string> parents;
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

    MemberAccessNode(std::unique_ptr<Node> obj, std::string member)
        : object(std::move(obj)), memberName(member) {}

    void print(int indent) const override {
        std::string space(indent, ' ');
        std::cout << space << "MemberAccess: ." << memberName << std::endl;
        object->print(indent + 2);
    }
};

class VarDeclNode : public Node {
   public:
    std::unique_ptr<Node> lhs;
    std::unique_ptr<Node> expression;
    std::string op;
    std::string typeAnnotation;

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

#endif