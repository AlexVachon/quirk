#include <iostream>
#include <algorithm>
#include <functional>
#include <sstream>
#include "sema.hpp"

std::string Sema::currentClass = "";

// Strip generic type args: "List[T]" -> "List", "Map[K, V]" -> "Map", "Int" -> "Int"
static std::string baseType(const std::string& t) {
    auto pos = t.find('[');
    return pos != std::string::npos ? t.substr(0, pos) : t;
}

// Extract the comma-separated type args from a parameterized type:
//   "Box[Int]"          -> ["Int"]
//   "Map[String, Int]"  -> ["String", "Int"]
//   "Result[T, E]"      -> ["T", "E"]   (still parameterized at the use site)
//   "Int"               -> []           (no brackets)
// Respects nesting: `List[Pair[K, V]]` returns ["Pair[K, V]"].
// Used by the v2.4.2 type-substitution pass — when a binding is
// declared `b: Box[Int]`, member-access resolution swaps any `T`
// (from Box's typeParams) for `Int` before returning the field type.
static std::vector<std::string> extractTypeArgs(const std::string& t) {
    std::vector<std::string> out;
    auto open = t.find('[');
    if (open == std::string::npos) return out;
    int depth = 0;
    std::string cur;
    for (size_t i = open + 1; i < t.size(); i++) {
        char c = t[i];
        if (c == '[') { depth++; cur += c; }
        else if (c == ']') {
            if (depth == 0) {
                // Trim leading/trailing whitespace on the final arg.
                size_t a = cur.find_first_not_of(" \t");
                size_t b = cur.find_last_not_of(" \t");
                if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
                return out;
            }
            depth--;
            cur += c;
        }
        else if (c == ',' && depth == 0) {
            size_t a = cur.find_first_not_of(" \t");
            size_t b = cur.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        }
        else { cur += c; }
    }
    // Unterminated — best-effort emit whatever's there.
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Substitute generic type params with concrete args inside a field
// or return-type string. Walks the string and replaces whole-word
// occurrences of `typeParams[i]` with `typeArgs[i]`. Recursive shapes
// (`List[T]`) are handled because the substitution is textual — the
// `T` inside `List[T]` gets swapped just like a bare `T`. If either
// vector is shorter, only the prefix in common substitutes.
static std::string substituteTypeParams(const std::string& type,
                                        const std::vector<std::string>& typeParams,
                                        const std::vector<std::string>& typeArgs) {
    if (typeParams.empty() || typeArgs.empty()) return type;
    const size_t n = std::min(typeParams.size(), typeArgs.size());
    std::string out = type;
    for (size_t i = 0; i < n; i++) {
        const std::string& from = typeParams[i];
        const std::string& to   = typeArgs[i];
        if (from.empty() || from == to) continue;
        // Whole-word replace (alphanumeric/underscore boundaries) so
        // `T` in `T` matches but `T` in `Tuple` doesn't.
        std::string next;
        size_t i2 = 0;
        while (i2 < out.size()) {
            size_t hit = out.find(from, i2);
            if (hit == std::string::npos) { next.append(out, i2, std::string::npos); break; }
            char before = hit == 0 ? '\0' : out[hit - 1];
            char after  = hit + from.size() < out.size() ? out[hit + from.size()] : '\0';
            auto isWordChar = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_';
            };
            next.append(out, i2, hit - i2);
            if (isWordChar(before) || isWordChar(after)) {
                next.append(from);
            } else {
                next.append(to);
            }
            i2 = hit + from.size();
        }
        out = std::move(next);
    }
    return out;
}

// v3.1.0 monomorphization helpers ──────────────────────────────────
//
// Pre-pass that runs before structRegistry is built. Walks the AST
// for `StructName[ConcreteArgs]` annotations, synthesizes one
// specialized StructNode per unique pair (with field types
// T-substituted), and rewrites every annotation in place to the
// mangled name. Downstream Sema + Codegen then see only concrete
// structs.
//
// Method-body specialization is NOT done by this pass — that
// requires deep-cloning FunctionNode bodies and substituting T
// inside expressions, which needs an AST clone visitor we don't
// have yet. As a result, methods declared on the generic struct
// are NOT carried over to the specialized variants in this slice.
// Member-field access works (Box[Int].value GEPs against the
// specialized layout); method calls (Box[Int].method()) fall back
// to the original via the inheritance walk.

// Mangle `Box[Int]` → `Box__Int`. Recursive in args, so
// `List[Box[Int]]` → `List__Box__Int`. Trailing `?` (nullable
// marker) is preserved post-mangle.
static std::string monoMangleType(const std::string& type) {
    if (type.empty()) return type;
    bool nullable = !type.empty() && type.back() == '?';
    std::string t = nullable ? type.substr(0, type.size() - 1) : type;
    auto pos = t.find('[');
    if (pos == std::string::npos) return type;
    std::string base = t.substr(0, pos);
    auto args = extractTypeArgs(t);
    std::string out = base;
    for (const auto& a : args) {
        out += "__";
        out += monoMangleType(a);
    }
    if (nullable) out += "?";
    return out;
}

// Strip nullable suffix for register lookups. `Box[Int]?` and
// `Box[Int]` share the same specialized struct — the `?` just
// means the binding can hold null in addition.
static std::string stripNullable(const std::string& t) {
    if (!t.empty() && t.back() == '?') return t.substr(0, t.size() - 1);
    return t;
}

void Sema::collectInstantiationsFromType(const std::string& type) {
    if (type.empty()) return;
    std::string t = stripNullable(type);
    auto pos = t.find('[');
    if (pos == std::string::npos) return;
    std::string base = t.substr(0, pos);
    auto args = extractTypeArgs(t);
    // Nested first so `List[Box[Int]]` registers `Box[Int]` before
    // the outer `List[Box[Int]]`.
    for (const auto& a : args) collectInstantiationsFromType(a);
    if (!structRegistry.count(base)) return;
    // Reject if any arg is itself a generic param still in scope.
    // Those instantiations resolve at use sites, not at synthesis.
    for (const auto& a : args) {
        std::string aBase = stripNullable(baseType(a));
        if (isGenericParam(aBase)) return;
    }
    monoInstantiations.insert({base, args});
}

void Sema::collectInstantiationsInNode(Node* n) {
    if (!n) return;
    if (auto* s = dynamic_cast<StructNode*>(n)) {
        for (const auto& f : s->fields) collectInstantiationsFromType(f.type);
        for (const auto& p : s->parents) collectInstantiationsFromType(p);
        return;
    }
    if (auto* f = dynamic_cast<FunctionNode*>(n)) {
        for (const auto& p : f->parameters) collectInstantiationsFromType(p.type);
        collectInstantiationsFromType(f->returnType);
        for (auto& s : f->body) collectInstantiationsInNode(s.get());
        return;
    }
    if (auto* v = dynamic_cast<VarDeclNode*>(n)) {
        collectInstantiationsFromType(v->typeAnnotation);
        if (v->expression) collectInstantiationsInNode(v->expression.get());
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(n)) {
        for (auto& s : i->thenBranch) collectInstantiationsInNode(s.get());
        for (auto& b : i->elIfBranches) for (auto& s : b.body) collectInstantiationsInNode(s.get());
        for (auto& s : i->elseBranch) collectInstantiationsInNode(s.get());
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        for (auto& s : w->body) collectInstantiationsInNode(s.get());
        return;
    }
    if (auto* fr = dynamic_cast<ForNode*>(n)) {
        for (auto& s : fr->body) collectInstantiationsInNode(s.get());
        return;
    }
    if (auto* tc = dynamic_cast<TryCatchNode*>(n)) {
        for (auto& s : tc->tryBlock) collectInstantiationsInNode(s.get());
        for (auto& cb : tc->catchBlocks) for (auto& s : cb.body) collectInstantiationsInNode(s.get());
        for (auto& s : tc->finallyBlock) collectInstantiationsInNode(s.get());
        return;
    }
}

// Walk the AST and rewrite every `StructName[ConcreteArgs]`
// annotation to the mangled name (`StructName__Arg1__Arg2`).
// Only the type-string fields are rewritten; expression bodies
// keep their original types (the synthesised structs see them
// via inheritance and field-type substitution).
void Sema::rewriteTypeAnnotations(Node* n) {
    if (!n) return;
    if (auto* s = dynamic_cast<StructNode*>(n)) {
        for (auto& f : s->fields) f.type = monoMangleType(f.type);
        for (auto& p : s->parents) p = monoMangleType(p);
        return;
    }
    if (auto* f = dynamic_cast<FunctionNode*>(n)) {
        for (auto& p : f->parameters) p.type = monoMangleType(p.type);
        f->returnType = monoMangleType(f->returnType);
        for (auto& s : f->body) rewriteTypeAnnotations(s.get());
        return;
    }
    if (auto* v = dynamic_cast<VarDeclNode*>(n)) {
        v->typeAnnotation = monoMangleType(v->typeAnnotation);
        if (v->expression) rewriteTypeAnnotations(v->expression.get());
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(n)) {
        for (auto& s : i->thenBranch) rewriteTypeAnnotations(s.get());
        for (auto& b : i->elIfBranches) for (auto& s : b.body) rewriteTypeAnnotations(s.get());
        for (auto& s : i->elseBranch) rewriteTypeAnnotations(s.get());
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(n)) {
        for (auto& s : w->body) rewriteTypeAnnotations(s.get());
        return;
    }
    if (auto* fr = dynamic_cast<ForNode*>(n)) {
        for (auto& s : fr->body) rewriteTypeAnnotations(s.get());
        return;
    }
    if (auto* tc = dynamic_cast<TryCatchNode*>(n)) {
        for (auto& s : tc->tryBlock) rewriteTypeAnnotations(s.get());
        for (auto& cb : tc->catchBlocks) for (auto& s : cb.body) rewriteTypeAnnotations(s.get());
        for (auto& s : tc->finallyBlock) rewriteTypeAnnotations(s.get());
        return;
    }
}

void Sema::runMonomorphizePrePass(std::vector<std::unique_ptr<Node>>& nodes) {
    // First pass: build a structRegistry SHADOW so the collector's
    // `structRegistry.count(base)` check sees user structs that
    // haven't been Pass-1-registered yet.
    for (const auto& n : nodes) {
        if (auto* s = dynamic_cast<StructNode*>(n.get())) {
            structRegistry[s->name] = s;
        }
    }
    // Second pass: collect every `Foo[Args]` annotation.
    for (const auto& n : nodes) collectInstantiationsInNode(n.get());

    // Third pass: synthesise one specialised StructNode per
    // (StructName, [Args]) pair. Field types get T → Arg
    // substituted; field names are preserved verbatim.
    std::vector<std::unique_ptr<Node>> synthesised;
    for (const auto& inst : monoInstantiations) {
        const std::string& base = inst.first;
        const std::vector<std::string>& args = inst.second;
        std::string mangled = base;
        for (const auto& a : args) { mangled += "__"; mangled += monoMangleType(a); }
        if (structRegistry.count(mangled)) continue;   // already done
        auto* src = structRegistry[base];
        if (!src) continue;
        if (src->typeParams.size() != args.size()) continue;
        auto spec = std::make_unique<StructNode>();
        spec->name = mangled;
        spec->parents = { base };   // inherit from generic so methods reachable
        spec->line = src->line; spec->col = src->col; spec->filePath = src->filePath;
        for (const auto& f : src->fields) {
            StructField sf;
            sf.name = f.name;
            sf.type = substituteTypeParams(f.type, src->typeParams, args);
            // Recursively mangle nested instantiations in the
            // substituted field type, so `Box[Box[Int]]` ends up
            // with a `value: Box__Int` field that downstream
            // resolution can find.
            sf.type = monoMangleType(sf.type);
            spec->fields.push_back(std::move(sf));
        }
        structRegistry[mangled] = spec.get();
        synthesised.push_back(std::move(spec));
    }
    for (auto& n : synthesised) nodes.push_back(std::move(n));

    // Fourth pass: rewrite every annotation in the original AST
    // to the mangled form. After this, downstream passes never
    // see `Foo[Args]` again — only concrete `Foo__Args`.
    for (const auto& n : nodes) rewriteTypeAnnotations(n.get());

    // Reset the shadow registry — Pass 1 will repopulate it via
    // its normal walk over the (now-extended) nodes vector.
    structRegistry.clear();
}

// Minimal JSON-string escaper for the NDJSON diagnostics mode. Quotes,
// backslashes, and control bytes get escaped; everything else passes
// through verbatim (we don't try to be Unicode-aware — Quirk file paths
// and error messages are ASCII in practice, and the LSP just feeds the
// string to its JSON parser).
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// One NDJSON record per diagnostic. Goes to stdout so the LSP can read
// line-buffered (`stderr` would buffer differently and complicate
// integration on every editor).
static void emitDiagnosticJson(const char* level, const std::string& msg,
                               const std::string& path, int line, int col,
                               const std::vector<std::string>& suggestions = {}) {
    std::cout << "{\"level\":\"" << level
              << "\",\"msg\":\""  << json_escape(msg)
              << "\",\"path\":\"" << json_escape(path)
              << "\",\"line\":"   << line
              << ",\"col\":"      << col;
    if (!suggestions.empty()) {
        std::cout << ",\"suggestions\":[";
        for (size_t i = 0; i < suggestions.size(); i++) {
            if (i) std::cout << ",";
            std::cout << "\"" << json_escape(suggestions[i]) << "\"";
        }
        std::cout << "]";
    }
    std::cout << "}\n";
    std::cout.flush();
}

// Shared formatting helper — prints one error to stderr (or one JSON
// line to stdout when `--diagnostics-json` is on). `suggestions` is
// only consulted by the JSON branch; the human-readable rendering
// adds the suggestions as a follow-up hint line.
static void printSemaError(const std::string& msg, int line, int col,
                           const std::string& path,
                           const std::map<std::string, std::string>& sourceMap,
                           const std::vector<std::string>& suggestions = {}) {
    if (g_diagnostics_json) {
        emitDiagnosticJson("error", msg, path, line, col, suggestions);
        return;
    }
    std::cerr << "\033[1;31m[ERROR]\033[0m " << msg << "\n";
    if (line > 0) {
        std::cerr << " --> ";
        if (!path.empty()) std::cerr << path << ":";
        std::cerr << line;
        if (col > 0) std::cerr << ":" << col;
        std::cerr << "\n";
        if (!path.empty() && sourceMap.count(path)) {
            const std::string& src = sourceMap.at(path);
            int cur = 1; std::string lineText;
            std::istringstream ss(src);
            while (std::getline(ss, lineText)) { if (cur++ == line) break; }
            std::string ln = std::to_string(line);
            std::cerr << std::string(ln.size(), ' ') << " |\n";
            std::cerr << ln << " | " << lineText << "\n";
            int off = (col > 1) ? col - 1 : 0;
            std::cerr << std::string(ln.size(), ' ') << " | "
                      << std::string(off, ' ')
                      << "\033[1;33m^--- here\033[0m\n";
        }
    }
    if (!suggestions.empty()) {
        std::cerr << "  hint: did you mean ";
        for (size_t i = 0; i < suggestions.size(); i++) {
            if (i > 0) std::cerr << (i + 1 == suggestions.size() ? " or " : ", ");
            std::cerr << "`" << suggestions[i] << "`";
        }
        std::cerr << "?\n";
    }
    if (line > 0) std::cerr << "\n";
}

[[noreturn]] void Sema::fatalError(const std::string& msg, int line, int col,
                                    const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                          ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    // Flush any previously accumulated errors first, then print this one and exit
    flushErrors();
    printSemaError(msg, line, col, path, sourceMap);
    exit(1);
}

void Sema::reportError(const std::string& msg, int line, int col,
                       const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                          ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    errors.push_back({msg, path, line, col, {}});
}

void Sema::reportError(const std::string& msg,
                       const std::vector<std::string>& suggestions,
                       int line, int col,
                       const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                          ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    errors.push_back({msg, path, line, col, suggestions});
}

// Damerau-Levenshtein distance — counts single-char transpositions as
// one edit instead of two (plain Levenshtein would score `gte` vs
// `get` as distance 2, missing the common keyboard-finger-swap typo).
// Uses a three-row DP so transpositions across positions i-2/j-2 are
// in scope; the matrix is small enough (identifier names rarely
// exceed ~30 chars) that the allocation cost is irrelevant.
static size_t editDistance(const std::string& a, const std::string& b) {
    if (a.empty()) return b.size();
    if (b.empty()) return a.size();
    size_t na = a.size(), nb = b.size();
    std::vector<std::vector<size_t>> d(na + 1, std::vector<size_t>(nb + 1));
    for (size_t i = 0; i <= na; i++) d[i][0] = i;
    for (size_t j = 0; j <= nb; j++) d[0][j] = j;
    for (size_t i = 1; i <= na; i++) {
        for (size_t j = 1; j <= nb; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1,
                                d[i][j - 1] + 1,
                                d[i - 1][j - 1] + cost});
            if (i >= 2 && j >= 2 &&
                a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[na][nb];
}

std::vector<std::string> Sema::suggestNames(const std::string& query, size_t maxN) {
    // Distance cutoff scales with name length: short typos (1 char)
    // shouldn't admit 2-edit "matches"; long names get a little more
    // slack. Pinning at 2 keeps the "did you mean Direction" for
    // `print` confusion in check.
    const size_t cutoff = std::min<size_t>(query.size() <= 4 ? 1 : 2, 2);

    std::vector<std::pair<size_t, std::string>> scored;
    auto consider = [&](const std::string& candidate) {
        if (candidate.empty() || candidate == query) return;
        size_t d = editDistance(query, candidate);
        if (d <= cutoff) scored.push_back({d, candidate});
    };

    // Local scopes — params + locals in the current function.
    for (const auto& scope : scopeStack)
        for (const auto& kv : scope) consider(kv.first);
    // Globals + module aliases.
    for (const auto& kv : globalSymbols) consider(kv.first);
    for (const auto& kv : globalModuleAliases) consider(kv.first);
    // Registries.
    for (const auto& kv : structRegistry) consider(kv.first);
    for (const auto& kv : enumRegistry) consider(kv.first);
    for (const auto& kv : interfaceRegistry) consider(kv.first);
    // Top-level module functions live under structRegistry[""] in
    // practice, but the method registry also holds them under the
    // empty class slot. Iterate both for safety.
    for (const auto& cls : methodRegistry)
        for (const auto& kv : cls.second)
            consider(kv.first);
    // Module-level constants like `PI := 3.14`.
    for (const auto& kv : moduleConstRegistry) consider(kv.first);

    // Dedup + sort by distance.
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& kv : scored) {
        if (seen.insert(kv.second).second) out.push_back(kv.second);
        if (out.size() >= maxN) break;
    }
    return out;
}

// Top-N closest member names (fields + methods) on `structName` to
// `query`. Walks the parent chain so inherited members are eligible.
// Used by the member-not-found error path to turn "no member 'fls'
// on List" into "did you mean `filter`?".
std::vector<std::string> Sema::suggestMembers(const std::string& structName,
                                              const std::string& query,
                                              size_t maxN) {
    const size_t cutoff = std::min<size_t>(query.size() <= 4 ? 1 : 2, 2);
    std::vector<std::pair<size_t, std::string>> scored;
    std::set<std::string> visited;

    std::function<void(const std::string&)> walk = [&](const std::string& sn) {
        if (visited.count(sn)) return;
        visited.insert(sn);
        auto sIt = structRegistry.find(sn);
        if (sIt == structRegistry.end()) return;
        StructNode* st = sIt->second;
        // Fields: take the bare name (the field type isn't part of
        // what the user typed).
        for (const auto& f : st->fields) {
            if (f.name.empty() || f.name == query) continue;
            size_t d = editDistance(query, f.name);
            if (d <= cutoff) scored.push_back({d, f.name});
        }
        // Methods: registered under `methodRegistry[sn]` keyed by
        // mangled name `<sn>_<method>` or `<sn>__<dunder>`. Strip
        // the prefix to recover the bare method the user types.
        auto mIt = methodRegistry.find(sn);
        if (mIt != methodRegistry.end()) {
            for (const auto& kv : mIt->second) {
                const std::string& full = kv.first;
                std::string bare = full;
                if (bare.rfind(sn + "__", 0) == 0) bare = bare.substr(sn.size() + 1); // dunder
                else if (bare.rfind(sn + "_", 0) == 0) bare = bare.substr(sn.size() + 1);
                if (bare.empty() || bare == query) continue;
                size_t d = editDistance(query, bare);
                if (d <= cutoff) scored.push_back({d, bare});
            }
        }
        // Walk parents (single-line; multi-parent is fine, we dedup).
        for (const auto& p : st->parents) walk(p);
    };
    walk(structName);

    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& kv : scored) {
        if (seen.insert(kv.second).second) out.push_back(kv.second);
        if (out.size() >= maxN) break;
    }
    return out;
}

// Top-N closest top-level functions in a specific module by edit
// distance. Walks `methodRegistry[""]` (the top-level slot) and
// keeps candidates whose owning FunctionNode->moduleName matches
// `modName`. Used by the `module 'X' has no function 'Y'` path to
// surface `net.gte(...)` → `did you mean 'get'?`.
std::vector<std::string> Sema::suggestModuleFunctions(
        const std::string& modName, const std::string& query, size_t maxN) {
    const size_t cutoff = std::min<size_t>(query.size() <= 4 ? 1 : 2, 2);
    // Match by package prefix the same way lookupTopLevel does:
    // `modName` is what the user typed (e.g. `net`), candidate
    // FunctionNodes have canonical module names like `net.http`.
    auto pkgOf = [](const std::string& m) -> std::string {
        auto dot = m.find('.');
        return dot == std::string::npos ? m : m.substr(0, dot);
    };
    std::vector<std::pair<size_t, std::string>> scored;
    auto top = methodRegistry.find("");
    if (top != methodRegistry.end()) {
        for (const auto& kv : top->second) {
            if (kv.first.empty() || kv.first == query) continue;
            FunctionNode* fn = kv.second;
            if (!fn) continue;
            if (fn->moduleName != modName && pkgOf(fn->moduleName) != modName)
                continue;
            size_t d = editDistance(query, kv.first);
            if (d <= cutoff) scored.push_back({d, kv.first});
        }
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& kv : scored) {
        if (seen.insert(kv.second).second) out.push_back(kv.second);
        if (out.size() >= maxN) break;
    }
    return out;
}

// Top-N closest enum-variant names by edit distance. Used by the
// `'X' is not a variant of enum 'Y'` path so `Color.Reed` surfaces
// `did you mean 'Red'?`.
std::vector<std::string> Sema::suggestEnumVariants(
        const std::string& enumName, const std::string& query, size_t maxN) {
    const size_t cutoff = std::min<size_t>(query.size() <= 4 ? 1 : 2, 2);
    auto it = enumRegistry.find(enumName);
    if (it == enumRegistry.end()) return {};
    std::vector<std::pair<size_t, std::string>> scored;
    for (const auto& v : it->second->variants) {
        if (v.empty() || v == query) continue;
        size_t d = editDistance(query, v);
        if (d <= cutoff) scored.push_back({d, v});
    }
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& kv : scored) {
        if (seen.insert(kv.second).second) out.push_back(kv.second);
        if (out.size() >= maxN) break;
    }
    return out;
}

// Format a `did you mean` hint to append to a fatalError message.
// Empty string when no suggestions match, so the caller can blindly
// concatenate without branching.
static std::string formatHint(const std::vector<std::string>& suggestions) {
    if (suggestions.empty()) return "";
    std::string out = "\n  hint: did you mean ";
    for (size_t i = 0; i < suggestions.size(); i++) {
        if (i > 0) out += (i + 1 == suggestions.size() ? " or " : ", ");
        out += "`";
        out += suggestions[i];
        out += "`";
    }
    out += "?";
    return out;
}

void Sema::flushErrors() {
    for (auto& e : errors)
        printSemaError(e.msg, e.line, e.col, e.filePath, sourceMap, e.suggestions);
    errors.clear();
}

void Sema::reportWarning(const std::string& msg, int line, int col,
                         const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                             ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    warnings.push_back({msg, path, line, col});
}

void Sema::flushWarnings() {
    for (auto& w : warnings)
        std::cerr << "\033[1;33m[WARNING]\033[0m " << w.msg
                  << "\n --> " << w.filePath << ":" << w.line << ":" << w.col << "\n";
    warnings.clear();
}

bool Sema::analyze(std::vector<std::unique_ptr<Node>> &nodes)
{
    if (scopeStack.empty())
        enterScope();

    // Pass 0: Register Global Module Aliases (Synchronized with Codegen)
    for (const auto &node : nodes) {
        if (auto use = dynamic_cast<UseNode*>(node.get())) {
            if (use->filterList.empty()) {
                std::string alias = use->moduleName;
                size_t lastDot = alias.rfind('.');
                if (lastDot == std::string::npos) lastDot = alias.rfind('/');
                if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
                globalModuleAliases[alias] = "MODULE$" + use->moduleName;
            }
        }
    }

    // Register built-in Type struct (used by self.__class)
    static StructNode builtinTypeNode;
    if (builtinTypeNode.name.empty()) {
        builtinTypeNode.name = "Type";
        StructField nf; nf.name = "name";   nf.type = "String";
        StructField pf; pf.name = "parent"; pf.type = "String";
        builtinTypeNode.fields.push_back(std::move(nf));
        builtinTypeNode.fields.push_back(std::move(pf));
    }
    if (!structRegistry.count("Type")) structRegistry["Type"] = &builtinTypeNode;

    // Pass 0 (v3.1.0): Monomorphize `StructName[ConcreteArgs]`
    // annotations into synthesised specialised structs. After this
    // runs, Pass 1 + downstream see only concrete struct names —
    // no `Foo[Bar]` references survive in type annotations.
    runMonomorphizePrePass(nodes);

    // Pass 1: Register Structs, Interfaces, and Signatures
    for (const auto &node : nodes)
    {
        if (auto s = dynamic_cast<StructNode *>(node.get()))
        {
            structRegistry[s->name] = s;
        }
        else if (auto iface = dynamic_cast<InterfaceNode*>(node.get()))
        {
            interfaceRegistry[iface->name] = iface;
        }
        else if (auto f = dynamic_cast<FunctionNode *>(node.get()))
        {
            if (f->returnType.empty())
                f->returnType = "auto";
            defineVariable(f->name, f->returnType);
            if (!f->cls.empty())
                methodRegistry[f->cls][f->name] = f;
            else {
                // For top-level functions: keep the existing
                // single-FunctionNode pointer for the 99% case where
                // a name is unique. When a second package defines
                // the same name (e.g. `html.input` after
                // `console.input` is loaded), promote both into the
                // overloads side-table so the call-site can pick by
                // module visibility.
                auto& slot = methodRegistry[""][f->name];
                if (slot && slot->moduleName != f->moduleName) {
                    auto& cands = topLevelOverloads[f->name];
                    if (cands.empty()) cands.push_back(slot);
                    // Avoid duplicate registration on Sema re-runs.
                    bool already = false;
                    for (auto* c : cands) if (c == f) { already = true; break; }
                    if (!already) cands.push_back(f);
                }
                slot = f;
            }
        }
        else if (auto e = dynamic_cast<EnumNode*>(node.get())) {
            enumRegistry[e->name] = e;
        }
        else if (auto tu = dynamic_cast<TaggedUnionDeclNode*>(node.get())) {
            // Variants were already desugared to StructNodes at parse
            // time; here we just record the union → variant-set mapping
            // for the exhaustiveness check in checkStatement(MatchNode).
            std::vector<std::string> names;
            names.reserve(tu->variants.size());
            for (const auto& v : tu->variants) names.push_back(v.name);
            taggedUnionVariants[tu->name] = std::move(names);
        }
        else if (auto v = dynamic_cast<VarDeclNode*>(node.get())) {
            // Top-level `NAME := value` bindings — track them so
            // `from M use { NAME }` can resolve them across modules.
            // The LHS is a LiteralNode whose value is the name.
            if (auto* lhs = dynamic_cast<LiteralNode*>(v->lhs.get())) {
                moduleConstRegistry[lhs->value] = v;
            }
        }
    }

    // Pass 2: Analyze Bodies
    enterScope();

    for (const auto &node : nodes)
    {
        std::string mod = node->moduleName;
        if (moduleVisibility.find(mod) == moduleVisibility.end())
        {
            moduleVisibility[mod].visibleModules.insert("typing");
        }

        // Validate inheritance tree and interface conformance for structs
        if (auto s = dynamic_cast<StructNode*>(node.get())) {
            std::vector<std::string> structParents;
            for (const std::string& parentName : s->parents) {
                if (interfaceRegistry.count(parentName)) {
                    // It's an interface — record conformance and check it
                    s->interfaces.push_back(parentName);
                    checkInterfaceConformance(s, interfaceRegistry[parentName]);
                } else if (!structRegistry.count(parentName)) {
                    std::string msg = "struct '" + s->name + "' inherits from undefined type '" + parentName + "'";
                    msg += formatHint(suggestNames(parentName));
                    fatalError(msg, s->line, s->col, s->filePath);
                } else {
                    structParents.push_back(parentName);
                }
            }
            s->parents = structParents; // keep only struct parents (not interfaces)

            // Validate generic where constraints: each bound must name an interface, not a struct
            for (const auto& [typeVar, bounds] : s->genericConstraints) {
                for (const auto& bound : bounds) {
                    if (structRegistry.count(bound) && !interfaceRegistry.count(bound)) {
                        reportWarning(
                            "generic constraint '" + bound + "' is a concrete type, not an interface. "
                            "Use an interface or remove the constraint and use '" + bound + "' directly.",
                            s->line, s->col, s->filePath);
                    } else if (!interfaceRegistry.count(bound) && bound != "Any" && bound != "Primitive") {
                        reportWarning(
                            "generic constraint '" + bound + "' is not a known interface.",
                            s->line, s->col, s->filePath);
                    }
                }
            }
        }

        if (!node->filePath.empty()) currentFilePath = node->filePath;

        if (auto f = dynamic_cast<FunctionNode *>(node.get()))
        {
            checkFunction(f);
        }
        else if (auto use = dynamic_cast<UseNode *>(node.get()))
        {
            checkUse(use);
        }
        else if (dynamic_cast<InterfaceNode*>(node.get()))
        {
            // Interface declarations are compile-time only — no body to check
        }
        else if (!dynamic_cast<StructNode *>(node.get()))
        {
            checkStatement(node.get());
        }
    }
    exitScope();
    if (!warnings.empty()) flushWarnings();
    if (!errors.empty()) {
        flushErrors();
        return false;
    }
    return true;
}

void Sema::checkUse(UseNode *node)
{
    std::string sourceModule = static_cast<Node *>(node)->moduleName;
    VisibilityContext &ctx = moduleVisibility[sourceModule];

    if (!node->filterList.empty())
    {
        for (size_t i = 0; i < node->filterList.size(); i++)
        {
            const std::string& item = node->filterList[i];
            const std::string localName =
                (i < node->filterAliases.size() && !node->filterAliases[i].empty())
                    ? node->filterAliases[i]
                    : item;

            bool found = structRegistry.count(item) || methodRegistry[""].count(item)
                      || interfaceRegistry.count(item) || enumRegistry.count(item)
                      || moduleConstRegistry.count(item);
            if (!found) {
                // Look across every registered symbol for a close
                // match. We don't have a per-module index, so this is
                // suggestNames-flavored — `from sys use { argz }`
                // will pick up `argv` because it's the closest known
                // global; `from typing use { Lest }` finds `List`.
                std::string msg = "module '" + node->moduleName +
                                  "' does not export symbol '" + item + "'";
                msg += formatHint(suggestNames(item));
                fatalError(msg, node->line, node->col, node->filePath);
            }
            // The user-visible name in this module is `localName`
            // (either the bare import or the explicit `as` alias).
            // visibleSymbols tracks the local name for visibility
            // checks; importAliases records the alias → source
            // mapping so lookupTopLevel can dereference at
            // call-site resolution time.
            ctx.visibleSymbols.insert(localName);
            ctx.visibleSymbolSources[localName] = node->moduleName;
            if (localName != item) {
                ctx.importAliases[localName] = item;
            }
        }
    }
    else
    {
        std::string alias = node->alias;
        if (alias.empty()) {
            alias = node->moduleName;
            size_t lastDot = alias.rfind('.');
            if (lastDot == std::string::npos) lastDot = alias.rfind('/');
            if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
        }

        defineVariable(alias, "MODULE$" + node->moduleName);

        std::string modName = node->moduleName;
        std::replace(modName.begin(), modName.end(), '/', '.');
        ctx.visibleModules.insert(modName);
    }
}

bool Sema::isVisible(const std::string &name, const std::string &symbolModule, const std::string &currentModule)
{
    if (currentModule != "main")
        return true;
    if (symbolModule.find("typing") == 0 || symbolModule.find("core") == 0)
        return true;
    if (symbolModule == currentModule)
        return true;

    if (moduleVisibility.count(currentModule))
    {
        const auto &ctx = moduleVisibility[currentModule];
        if (ctx.visibleModules.count(symbolModule))
            return true;
        if (ctx.visibleSymbols.count(name))
            return true;
    }
    return false;
}

void Sema::checkInterfaceConformance(StructNode* s, InterfaceNode* iface) {
    for (const auto& method : iface->methods) {
        // method->name is already mangled as "InterfaceName_methodName"
        // For the struct, the method would be "StructName_methodName"
        std::string rawName = method->name;
        // Strip the interface prefix to get the raw method name
        if (rawName.size() > iface->name.size() + 1)
            rawName = rawName.substr(iface->name.size() + 1);
        std::string structMethodName = s->name + "_" + rawName;

        bool found = methodRegistry.count(s->name) &&
                     methodRegistry[s->name].count(structMethodName);
        if (!found) {
            reportError("struct '" + s->name + "' does not implement interface method '" +
                        rawName + "' (required by '" + iface->name + "')",
                        s->line, s->col, s->filePath);
        }
    }
    // Also check inherited interface methods
    for (const auto& ext : iface->extends) {
        if (interfaceRegistry.count(ext))
            checkInterfaceConformance(s, interfaceRegistry[ext]);
    }
}

void Sema::checkFunction(FunctionNode *f)
{
    // Abstract interface method signatures have no body to check
    if (f->isAbstract) return;

    std::string prevClass = currentClass;
    FunctionNode *prevFunc = currentFunctionNode;
    std::string prevScope = currentScope;

    currentClass = f->cls;
    currentFunctionNode = f;
    if (!f->filePath.empty()) currentFilePath = f->filePath;

    // Track scope for the usage table. We use the function's
    // demangled raw name when it's a method so usages match the same
    // `scope` key that `--symbols-json` emits for the decl.
    if (!f->cls.empty()) {
        if (f->name == f->cls + "__init")                currentScope = "__init";
        else if (f->name.rfind(f->cls + "_", 0) == 0)    currentScope = f->name.substr(f->cls.size() + 1);
        else                                              currentScope = f->name;
    } else {
        currentScope = f->name;
    }

    // Push generic type params as type aliases for "Any" so that annotations
    // like `item: T` and `-> T` are accepted without "unknown type" errors.
    std::vector<std::string> pushedParams;
    auto pushTypeParam = [&](const std::string& tp) {
        if (!typeAliases.count(tp)) {
            typeAliases[tp] = "Any";
            pushedParams.push_back(tp);
        }
    };
    for (const auto& tp : f->typeParams) pushTypeParam(tp);
    // Also push the enclosing struct's type params (e.g. T in struct Stack[T])
    if (!f->cls.empty() && structRegistry.count(f->cls))
        for (const auto& tp : structRegistry[f->cls]->typeParams) pushTypeParam(tp);

    // Validate generic where constraints: each bound must name an interface, not a struct
    for (const auto& [typeVar, bounds] : f->genericConstraints) {
        for (const auto& bound : bounds) {
            if (structRegistry.count(bound) && !interfaceRegistry.count(bound)) {
                reportWarning(
                    "generic constraint '" + bound + "' is a concrete type, not an interface. "
                    "Use an interface or remove the constraint and use '" + bound + "' directly.",
                    f->line, f->col, f->filePath);
            } else if (!interfaceRegistry.count(bound) && bound != "Any" && bound != "Primitive") {
                reportWarning(
                    "generic constraint '" + bound + "' is not a known interface.",
                    f->line, f->col, f->filePath);
            }
        }
    }

    enterScope();

    if (!f->cls.empty() && !f->isStatic)
        defineVariable("self", f->cls, false, true);
    // Type-alias substitution on parameter + return types (v3.14.0).
    // Mirrors the checkVarDecl path from v3.12.0: rewrite the
    // declared type in-place on the FunctionNode so Codegen reads the
    // canonical type when emitting function signatures and uses.
    // Without this, `define dup(xs: IntList) -> IntList` parameters
    // bind under the literal "IntList" alias name, downstream method
    // dispatch falls back to Any, and the LLVM body materialises as
    // garbage (List append failing silently, return values dropping
    // to 0, etc.).
    for (auto &param : f->parameters) {
        if (param.type.empty()) continue;
        std::string base = baseType(param.type);
        auto aIt = typeAliases.find(base);
        if (aIt != typeAliases.end() && aIt->second != "Any") {
            param.type = aIt->second;
        }
    }
    {
        std::string base = baseType(f->returnType);
        auto aIt = typeAliases.find(base);
        if (aIt != typeAliases.end() && aIt->second != "Any") {
            f->returnType = aIt->second;
        }
    }
    for (const auto &param : f->parameters)
        defineVariable(param.name, param.type.empty() ? "Any" : param.type, false, true);

    // Type-check default values against the declared parameter type.
    // A mismatched default (`raw: Int = "Hello"`) used to slip past
    // Sema and surface as an LLVM verifier abort in Codegen when the
    // default expression's value type didn't fit the function param
    // slot — fuzz #1 of v3.1.1 (`bitcast i8* to %String*` passed into
    // an i32 slot). Catching it here turns the ICE into a clean
    // Sema rejection.
    for (const auto& param : f->parameters) {
        if (!param.defaultValue || param.type.empty()) continue;
        std::string dt = checkExpression(param.defaultValue.get());
        if (dt.empty() || dt == "unknown") continue;
        if (!isCompatibleTypes(param.type, dt)) {
            fatalError("default value for parameter '" + param.name +
                       "' has type '" + dt + "' but parameter is declared '" +
                       param.type + "'",
                       f->line, f->col, f->filePath);
        }
    }

    if (f->whereClause)
        checkExpression(f->whereClause.get());

    if (!f->isExtern)
    {
        for (const auto &statement : f->body)
        {
            checkStatement(statement.get());
        }
    }

    if (f->returnType == "auto")
    {
        // No return statement found — default to void
        f->returnType = "void";
        if (auto* m = findMethod(f->cls, f->name)) m->returnType = "void";
    }
    else if (!f->cls.empty() && f->returnType != "void")
    {
        // Sync inferred return type back to registry (handles duplicate parsing)
        if (auto* m = findMethod(f->cls, f->name)) m->returnType = f->returnType;
    }

    exitScope();

    // Remove generic type param aliases we pushed
    for (const auto& tp : pushedParams)
        typeAliases.erase(tp);

    currentClass = prevClass;
    currentFunctionNode = prevFunc;
    currentScope = prevScope;
}

void Sema::checkVarDecl(VarDeclNode *node)
{
    lastNode = node;

    // Const mutation check: direct rebind AND index/member mutation on a const binding.
    if (node->op == "=" || node->op == "+=" || node->op == "-=" ||
        node->op == "*=" || node->op == "/=") {
        // Extract the root variable name from whatever LHS shape we have:
        //   m = ...          → LiteralNode("m")
        //   m["k"] = ...     → BinaryOpNode("[]", LiteralNode("m"), ...)
        //   m.field = ...    → MemberAccessNode(LiteralNode("m"), ...)
        std::string rootVar;
        if (auto lit = dynamic_cast<LiteralNode *>(node->lhs.get())) {
            rootVar = lit->value;
        } else if (auto bin = dynamic_cast<BinaryOpNode *>(node->lhs.get())) {
            if (bin->op == "[]")
                if (auto lit = dynamic_cast<LiteralNode *>(bin->left.get()))
                    rootVar = lit->value;
        } else if (auto mem = dynamic_cast<MemberAccessNode *>(node->lhs.get())) {
            if (auto lit = dynamic_cast<LiteralNode *>(mem->object.get()))
                rootVar = lit->value;
        }
        if (!rootVar.empty()) {
            for (int i = (int)scopeStack.size() - 1; i >= 0; i--) {
                if (scopeStack[i].count(rootVar)) {
                    if (scopeStack[i][rootVar].isConst)
                        fatalError("cannot mutate const variable '" + rootVar + "'",
                                   node->line, node->col, node->filePath);
                    break;
                }
            }
        }
    }

    std::string exprType = checkExpression(node->expression.get());

    // A node is a declaration if it uses ':=' OR if it has a type annotation (e.g. `x: Type = val`)
    bool isDecl = (node->op == ":=") || !node->typeAnnotation.empty();

    if (auto tup = dynamic_cast<TupleLiteralNode *>(node->lhs.get()))
    {
        // Tuple destructuring: (a, b) := tuple_expr
        for (auto& elem : tup->elements) {
            if (auto* nameNode = dynamic_cast<LiteralNode*>(elem.get()))
                defineVariable(nameNode->value, "Any");
        }
    }
    else if (auto lit = dynamic_cast<LiteralNode *>(node->lhs.get()))
    {
        // Check if variable already exists in any scope
        bool alreadyDefined = false;
        for (int i = (int)scopeStack.size() - 1; i >= 0; i--) {
            if (scopeStack[i].count(lit->value)) { alreadyDefined = true; break; }
        }

        if (isDecl || !alreadyDefined) {
            std::string finalType =
                node->typeAnnotation.empty() ? exprType : node->typeAnnotation;
            // Type-alias substitution (v3.12.0). `type Headers = Map`
            // means `h: Headers := Map()` should land `h` in scope
            // as the underlying type AND have the AST annotation
            // rewritten so Codegen — which reads
            // `vdecl->typeAnnotation` verbatim for its LLVM type
            // lookup and Any-unboxing decisions — sees the
            // canonical type too. Without the AST rewrite,
            // Codegen would query `getLLVMType("Headers")` and
            // fall back to an opaque shape, producing garbage
            // method dispatch downstream (`h.length()` returning
            // 8 instead of 0 on an empty Map).
            //
            // Only rewrite when the alias target is a real type,
            // not a `T → Any` generic-param erasure marker (those
            // are intentionally opaque).
            {
                std::string base = baseType(finalType);
                auto aIt = typeAliases.find(base);
                if (aIt != typeAliases.end() && aIt->second != "Any") {
                    finalType = aIt->second;
                    if (!node->typeAnnotation.empty())
                        node->typeAnnotation = finalType;
                }
            }
            // Stash the inferred type on the node so post-Sema walkers
            // (notably `--symbols-json`) can publish it as an inlay
            // hint at the binding site. Only meaningful when the user
            // didn't explicitly annotate.
            if (node->typeAnnotation.empty()) node->inferredType = exprType;
            defineVariable(lit->value, finalType, node->isConst);
        } else {
            // Reassignment — mark the existing binding as used
            resolveVariable(lit->value);
        }
    }
    else if (auto member = dynamic_cast<MemberAccessNode *>(node->lhs.get()))
    {
        std::string objType = checkExpression(member->object.get());
        if (resolveMember(objType, member->memberName) == "unknown") {
            std::string msg = "member '" + member->memberName + "' not found in '" + objType + "'";
            msg += formatHint(suggestMembers(baseType(objType), member->memberName));
            fatalError(msg, member->line, member->col, member->filePath);
        }
    }
}

void Sema::checkStatement(Node *node)
{
    if (auto v = dynamic_cast<VarDeclNode *>(node))
        checkVarDecl(v);
    else if (auto u = dynamic_cast<UseNode *>(node))
        checkUse(u);
    else if (auto i = dynamic_cast<IfNode *>(node))
        checkIf(i);
    else if (auto wi = dynamic_cast<WithNode *>(node))
        checkWith(wi);
    else if (auto w = dynamic_cast<WhileNode *>(node))
        checkWhile(w);
    else if (auto f = dynamic_cast<ForNode *>(node))
        checkFor(f);
    else if (auto c = dynamic_cast<CallNode *>(node))
        checkExpression(c);
    else if (auto t = dynamic_cast<TryCatchNode*>(node)) {
        enterScope();
        for (auto& s : t->tryBlock) checkStatement(s.get());
        exitScope();

        for (size_t i = 0; i < t->catchBlocks.size(); ++i) {
            auto& cb = t->catchBlocks[i];
            enterScope();
            for (const std::string& typeName : cb.types) {
                if (!structRegistry.count(typeName)) {
                    std::string msg = "catch type '" + typeName + "' is not defined";
                    msg += formatHint(suggestNames(typeName));
                    fatalError(msg, t->line, t->col, t->filePath);
                }
                if (structRegistry.count("Exception") && !inheritsFromException(typeName))
                    fatalError("catch type '" + typeName + "' does not inherit from 'Exception'",
                               t->line, t->col, t->filePath);
                for (size_t j = 0; j < i; ++j) {
                    for (const std::string& prevType : t->catchBlocks[j].types) {
                        if (inheritsFromException(typeName, prevType)) {
                            std::cerr << "\033[1;33m[WARNING]\033[0m catch (" << cb.varName
                                      << ": " << typeName << ") is unreachable — '"
                                      << prevType << "' in block " << (j + 1)
                                      << " already handles it\n";
                        }
                    }
                }
            }
            if (!cb.varName.empty()) defineVariable(cb.varName, cb.types[0]);
            for (auto& s : cb.body) checkStatement(s.get());
            exitScope();
        }

        for (auto& s : t->finallyBlock) checkStatement(s.get());
    }
    else if (auto ta = dynamic_cast<TypeAliasNode*>(node)) {
        typeAliases[ta->name] = ta->target;
        defineVariable(ta->name, "Type");
    }
    else if (auto m = dynamic_cast<MatchNode*>(node)) {
        std::string scrutType = checkExpression(m->scrutinee.get());
        // Exhaustiveness: if the scrutinee is a tagged-union root and
        // no wildcard arm is present, warn for any missing variant.
        // Doesn't error — runtime fall-through is harmless (the match
        // just exits with no body); the warning surfaces the missed
        // case at compile time without blocking partial matches.
        if (taggedUnionVariants.count(scrutType)) {
            bool hasWildcard = false;
            std::set<std::string> covered;
            for (const auto& arm : m->arms) {
                if (arm.isWildcard) { hasWildcard = true; break; }
                if (arm.isTypeMatch)
                    for (const auto& tn : arm.typeNames) covered.insert(tn);
            }
            if (!hasWildcard) {
                std::vector<std::string> missing;
                for (const auto& v : taggedUnionVariants[scrutType])
                    if (!covered.count(v)) missing.push_back(v);
                if (!missing.empty()) {
                    std::string msg = "non-exhaustive match on '" + scrutType + "' — missing variant";
                    if (missing.size() > 1) msg += "s";
                    msg += ": ";
                    for (size_t i = 0; i < missing.size(); i++) {
                        if (i > 0) msg += ", ";
                        msg += missing[i];
                    }
                    msg += ". Add an arm or a `_` wildcard.";
                    reportWarning(msg, m->line, m->col, m->filePath);
                }
            }
        }
        // v3.8.0: parallel exhaustiveness check for plain enums. A
        // `match c` where `c: Color` and `Color` has variants {Red,
        // Green, Blue} should warn when an arm is missing. Tagged
        // unions already get this; plain enums had no coverage check
        // before, so writing `case Color.Red => …` and forgetting
        // Green silently fell through with no diagnostic.
        //
        // Variant arms are MemberAccess patterns of the form
        // `Color.Red`. Skip arms with guards (they're conditional —
        // the pattern matches but the body may not run) by treating
        // guarded arms as non-covering. Match the tagged-union policy
        // and keep this a warning, not an error: partial matches are
        // legal Quirk, and the fall-through behaviour (match exits
        // silently with no body) doesn't crash anything.
        else if (enumRegistry.count(scrutType)) {
            bool hasWildcard = false;
            std::set<std::string> covered;
            for (const auto& arm : m->arms) {
                if (arm.isWildcard) { hasWildcard = true; break; }
                if (arm.guard) continue;  // conditional — doesn't fully cover
                for (const auto& pat : arm.patterns) {
                    auto* ma = dynamic_cast<MemberAccessNode*>(pat.get());
                    if (!ma) continue;
                    auto* obj = dynamic_cast<LiteralNode*>(ma->object.get());
                    if (!obj || obj->value != scrutType) continue;
                    covered.insert(ma->memberName);
                }
            }
            if (!hasWildcard) {
                std::vector<std::string> missing;
                for (const auto& v : enumRegistry[scrutType]->variants)
                    if (!covered.count(v)) missing.push_back(v);
                if (!missing.empty()) {
                    std::string msg = "non-exhaustive match on enum '" + scrutType + "' — missing variant";
                    if (missing.size() > 1) msg += "s";
                    msg += ": ";
                    for (size_t i = 0; i < missing.size(); i++) {
                        if (i > 0) msg += ", ";
                        msg += scrutType + "." + missing[i];
                    }
                    msg += ". Add an arm or a `_` wildcard.";
                    reportWarning(msg, m->line, m->col, m->filePath);
                }
            }
        }
        for (auto& arm : m->arms) {
            if (arm.isTypeMatch) {
                enterScope();
                if (!arm.bindName.empty()) defineVariable(arm.bindName, arm.typeNames[0], false, true);
                if (arm.guard) checkExpression(arm.guard.get());
                for (auto& s : arm.body) checkStatement(s.get());
                exitScope();
            } else {
                for (auto& pat : arm.patterns) checkExpression(pat.get());
                enterScope();
                // Wildcard-with-bind (`case x`) makes `x` visible in both
                // the guard and the body. Same pattern as `as x` above.
                if (arm.isWildcard && !arm.bindName.empty()) {
                    defineVariable(arm.bindName, "Any", false, true);
                }
                // Tuple destructure (`case (a, b)`): each name is bound to
                // the corresponding tuple slot at codegen time.
                for (auto& n : arm.bindNames) {
                    defineVariable(n, "Any", false, true);
                }
                if (arm.guard) checkExpression(arm.guard.get());
                for (auto& s : arm.body) checkStatement(s.get());
                exitScope();
            }
        }
    }
    else if (auto th = dynamic_cast<ThrowNode*>(node)) {
        if (th->expression) {
            std::string type = checkExpression(th->expression.get());
            if (!structRegistry.count(type))
                fatalError("can only throw struct objects, got '" + type + "'",
                           th->line, 0, th->filePath);
        }
        if (th->cause) checkExpression(th->cause.get());
        // bare throw (nullptr expression): re-raises current exception — no type check needed
    }
    else if (auto r = dynamic_cast<ReturnNode *>(node))
        checkReturn(r);
    else if (auto nl = dynamic_cast<NonlocalNode*>(node)) {
        // Mark nonlocal vars as known so references inside closures type-check cleanly.
        for (const auto& v : nl->vars)
            defineVariable(v, "Any", false, true);
    }
    else if (dynamic_cast<BreakNode*>(node)) { /* no-op */ }
    else if (dynamic_cast<ContinueNode*>(node)) { /* no-op */ }
}

// Walk an if-condition collecting (varName, typeName) pairs from `x is T`
// checks joined by `and`. Pairs are applied as shadowed bindings in the
// then-branch scope so member access / method lookup on `x` inside the
// block resolves against `T`. We skip `or` joins (either side could hold),
// and skip negations.
static void collectNarrowings(Node* cond, std::vector<std::pair<std::string,std::string>>& out) {
    if (!cond) return;
    if (auto* bin = dynamic_cast<BinaryOpNode*>(cond)) {
        if (bin->op == "and") {
            collectNarrowings(bin->left.get(), out);
            collectNarrowings(bin->right.get(), out);
            return;
        }
        if (bin->op == "is") {
            auto* lhs = dynamic_cast<LiteralNode*>(bin->left.get());
            auto* rhs = dynamic_cast<LiteralNode*>(bin->right.get());
            if (lhs && rhs && !lhs->value.empty() && !rhs->value.empty()) {
                char c0 = lhs->value[0];
                // LHS must look like an identifier, not a literal value.
                if (!std::isdigit(static_cast<unsigned char>(c0)) && c0 != '"' && c0 != '\''
                    && lhs->value != "true" && lhs->value != "false" && lhs->value != "null") {
                    out.emplace_back(lhs->value, rhs->value);
                }
            }
        }
    }
}

void Sema::checkIf(IfNode *node)
{
    lastNode = node;
    std::string condType = checkExpression(node->condition.get());
    if (condType != "Bool" && condType != "unknown" && condType != "Any")
        reportError("'if' condition must be 'Bool', got '" + condType + "'",
                    node->line, node->col, node->filePath);
    enterScope();
    {
        std::vector<std::pair<std::string,std::string>> narrowings;
        collectNarrowings(node->condition.get(), narrowings);
        for (auto& [var, type] : narrowings) defineVariable(var, type, false, false);
    }
    for (auto &s : node->thenBranch)
        checkStatement(s.get());
    exitScope();
    for (auto &b : node->elIfBranches)
    {
        std::string elifType = checkExpression(b.condition.get());
        if (elifType != "Bool" && elifType != "unknown")
            reportError("'elif' condition must be 'Bool'", node->line, node->col, node->filePath);
        enterScope();
        {
            std::vector<std::pair<std::string,std::string>> narrowings;
            collectNarrowings(b.condition.get(), narrowings);
            for (auto& [var, type] : narrowings) defineVariable(var, type, false, false);
        }
        for (auto &s : b.body)
            checkStatement(s.get());
        exitScope();
    }
    if (!node->elseBranch.empty())
    {
        enterScope();
        for (auto &s : node->elseBranch)
            checkStatement(s.get());
        exitScope();
    }
}

void Sema::checkWhile(WhileNode *node)
{
    lastNode = node;
    std::string wCondType = checkExpression(node->condition.get());
    if (wCondType != "Bool" && wCondType != "unknown" && wCondType != "Any")
        reportError("'while' condition must be 'Bool'", node->line, node->col, node->filePath);
    enterScope();
    for (auto &s : node->body)
        checkStatement(s.get());
    exitScope();
}

void Sema::checkFor(ForNode *node)
{
    // Sugar: `for v in EnumName` iterates the enum's variants. Treat
    // a bare enum-name literal as if the user had written
    // `EnumName.variants` so checkExpression doesn't trip on the
    // "undefined variable" path. Codegen does the matching rewrite.
    if (auto* lit = dynamic_cast<LiteralNode*>(node->iterable.get())) {
        if (enumRegistry.count(lit->value)) {
            // Variants list yields Any-typed items (boxed ordinals).
            enterScope();
            defineVariable(node->varName, "Any");
            if (!node->varName2.empty()) defineVariable(node->varName2, "Any", false, true);
            for (const auto& dv : node->destructureVars)
                defineVariable(dv, "Any", false, true);
            for (auto& s : node->body) checkStatement(s.get());
            exitScope();
            return;
        }
    }

    std::string iterType = checkExpression(node->iterable.get());
    std::string itemType = "Any";

    if (iterType == "String")
        itemType = "String";    // iterating a String yields length-1 Strings
    else if (iterType == "File")
        itemType = "String";
    else if (structRegistry.count(iterType))
    {
        if (auto* iterFn = findMethod(iterType, iterType + "___iter")) {
            const std::string& iterStruct = iterFn->returnType;
            if (auto* nextFn = findMethod(iterStruct, iterStruct + "___next"))
                itemType = nextFn->returnType;
        }
    }

    enterScope();
    defineVariable(node->varName, itemType);
    if (!node->varName2.empty()) defineVariable(node->varName2, "Any", false, true);
    for (const auto& dv : node->destructureVars)
        defineVariable(dv, "Any", false, true);
    for (auto &s : node->body)
        checkStatement(s.get());
    exitScope();
}

std::string Sema::checkExpression(Node *node)
{
    if (auto lit = dynamic_cast<LiteralNode *>(node))
        return checkLiteral(lit);
    if (auto binOp = dynamic_cast<BinaryOpNode *>(node))
        return checkBinaryOp(binOp);
    if (auto c = dynamic_cast<ConstructorNode *>(node))
        return checkConstructor(c);
    if (auto m = dynamic_cast<MemberAccessNode *>(node))
        return checkMemberAccess(m);
    if (auto c = dynamic_cast<CallNode *>(node))
        return checkCall(c);
    if (auto arr = dynamic_cast<ListLiteralNode *>(node))
        return checkListLiteral(arr);
    if (auto map = dynamic_cast<MapLiteralNode *>(node))
        return checkMapLiteral(map);
    if (auto s = dynamic_cast<SetLiteralNode *>(node)) {
        for (auto& e : s->elements) checkExpression(e.get());
        return "Set";
    }
    if (auto comp = dynamic_cast<ListComprehensionNode *>(node)) {
        checkExpression(comp->iterable.get());
        enterScope();
        defineVariable(comp->varName, "Any", false, true);
        if (!comp->varName2.empty()) defineVariable(comp->varName2, "Any", false, true);
        if (comp->condition) checkExpression(comp->condition.get());
        checkExpression(comp->expr.get());
        exitScope();
        return "List";
    }
    if (auto comp = dynamic_cast<MapComprehensionNode *>(node)) {
        checkExpression(comp->iterable.get());
        enterScope();
        defineVariable(comp->varName, "Any", false, true);
        if (!comp->varName2.empty()) defineVariable(comp->varName2, "Any", false, true);
        if (comp->condition) checkExpression(comp->condition.get());
        checkExpression(comp->keyExpr.get());
        checkExpression(comp->valExpr.get());
        exitScope();
        return "Map";
    }
    if (auto tup = dynamic_cast<TupleLiteralNode *>(node)) {
        for (auto& elem : tup->elements) checkExpression(elem.get());
        return "Tuple";
    }
    if (auto lambda = dynamic_cast<LambdaNode *>(node)) {
        FunctionNode* savedFn = currentFunctionNode;
        FunctionNode lambdaStub;
        lambdaStub.name = "<lambda>";
        lambdaStub.returnType = "auto";
        currentFunctionNode = &lambdaStub;
        enterScope();
        for (const auto& p : lambda->params) {
            std::string t = p.isVariadic ? "List" : (p.type.empty() ? "Any" : p.type);
            defineVariable(p.name, t, false, true);
        }
        if (lambda->isExpression && lambda->exprBody) {
            lambda->inferredReturnType = checkExpression(lambda->exprBody.get());
        } else {
            for (auto& s : lambda->stmtBody) checkStatement(s.get());
            if (!lambdaStub.returnType.empty() && lambdaStub.returnType != "auto" && lambdaStub.returnType != "void")
                lambda->inferredReturnType = lambdaStub.returnType;
        }
        exitScope();
        currentFunctionNode = savedFn;
        return "Callable";
    }
    if (auto tern = dynamic_cast<TernaryNode*>(node)) {
        checkExpression(tern->condition.get());
        std::string thenType = checkExpression(tern->thenExpr.get());
        std::string elseType = checkExpression(tern->elseExpr.get());
        if (thenType == elseType) return thenType;
        // Strip optional markers for comparison
        auto strip = [](const std::string& s) {
            return (!s.empty() && s.back() == '?') ? s.substr(0, s.size() - 1) : s;
        };
        if (strip(thenType) == strip(elseType)) return strip(thenType);
        return thenType;
    }
    if (auto sl = dynamic_cast<SliceNode*>(node)) {
        lastNode = sl;
        std::string objType = checkExpression(sl->object.get());
        if (sl->start) checkExpression(sl->start.get());
        if (sl->end)   checkExpression(sl->end.get());
        if (objType == "String") return "String";
        if (objType == "List")   return "List";
        fatalError("slice '[:]' not supported on type '" + objType + "'",
                   sl->line, sl->col, sl->filePath);
    }
    return "unknown";
}

std::string Sema::checkLiteral(LiteralNode *node)
{
    lastNode = node;
    if (std::isdigit(node->value[0]))
        return (node->value.find('.') != std::string::npos) ? "Double" : "Int";
    if (node->value[0] == '"' || node->value[0] == '\'')
        return "String";        // 'x' and "x" both produce length-1 strings
    if (node->value == "true" || node->value == "false")
        return "Bool";
    if (node->value == "null")
        return "Null";
    std::string t = resolveVariable(node->value);
    // Bare function name used as a value (passed as an arg, stored
    // in a variable, returned from a function) resolves to its
    // **return type** by default — `apply(dbl, 5)` saw `dbl` typed
    // as `Int` and rejected it against an `Callable` parameter.
    // When the name belongs to a top-level function AND there's no
    // local binding shadowing it, surface it as `Callable` so the
    // call-site type-check passes. The direct-call path at
    // `checkCall(node->callee == LiteralNode)` bypasses checkLiteral
    // entirely and still gets the function's return type.
    if (methodRegistry[""].count(node->value)) {
        // Top-level functions ARE in scopeStack[0] (Pass 1 calls
        // defineVariable with their return type) so a naive "any
        // scope binding == shadowed" check would treat every
        // function name as already-shadowed. Only deeper scopes
        // represent a true local re-binding (`dbl := 42` inside a
        // function body); the global registration is the function
        // itself, not a shadow.
        bool shadowed = false;
        for (int i = (int)scopeStack.size() - 1; i >= 1; --i) {
            if (scopeStack[i].count(node->value)) { shadowed = true; break; }
        }
        if (!shadowed) return "Callable";
    }
    return t;
}

std::string Sema::checkBinaryOp(BinaryOpNode *node)
{
    lastNode = node;
    if (node->op == "not")
    {
        std::string t = checkExpression(node->left.get());
        if (t != "Bool" && t != "Any" && t != "Int")
            fatalError("'not' operand must be 'Bool', got '" + t + "'",
                       node->line, node->col, node->filePath);
        return "Bool";
    }

    if (node->op == "?")
    {
        checkExpression(node->left.get());
        return "Bool";
    }

    if (node->op == "is")
    {
        checkExpression(node->left.get());
        return "Bool";
    }

    if (node->op == "as")
    {
        checkExpression(node->left.get());
        // RHS is always a LiteralNode holding the target type name
        if (auto lit = dynamic_cast<LiteralNode*>(node->right.get()))
            return lit->value;
        return "unknown";
    }

    if (node->op == "in" || node->op == "not in")
    {
        checkExpression(node->left.get());
        checkExpression(node->right.get());
        return "Bool";
    }

    std::string lType = checkExpression(node->left.get());
    std::string rType = checkExpression(node->right.get());

    if (node->op == "and" || node->op == "or")
        return "Bool";

    // Null-coalesce: result is the non-optional type of the LHS
    if (node->op == "??") {
        // Strip trailing ? from lType to get the base type
        std::string base = lType;
        if (!base.empty() && base.back() == '?') base.pop_back();
        return base.empty() ? rType : base;
    }

    // Array Access
    if (node->op == "[]")
    {
        lType = baseType(lType); // strip generic args: "List[T]" -> "List"
        if (structRegistry.count(lType))
        {
            std::string funcName = lType + "___get";
            if (methodRegistry[lType].count(funcName))
            {
                FunctionNode *func = methodRegistry[lType][funcName];
                if (!func->parameters.empty())
                {
                    std::string expectedKeyType = func->parameters[0].type;
                    bool validKey = (rType == expectedKeyType);
                    if (!validKey)
                        fatalError("type mismatch for '" + lType + "[]': expected '" +
                                   expectedKeyType + "' index, got '" + rType + "'",
                                   node->line, node->col, node->filePath);
                }
                return func->returnType;
            }
        }
        if (rType != "Int")
            fatalError("array index must be 'Int', got '" + rType + "'",
                       node->line, node->col, node->filePath);
        if (lType == "Any" || lType == "String")
            return (lType == "String") ? "String" : "Any";
        fatalError("type '" + lType + "' does not support indexing with '[]'",
                   node->line, node->col, node->filePath);
    }

    // Type-compatibility gate for arithmetic / comparison. Reject
    // mismatches *before* the operator-overloading branch — that
    // branch declares a result type (e.g. "Bool" for ==) but
    // doesn't actually verify Codegen will dispatch a struct
    // method, so without this gate `5 == "5"` would land at
    // Codegen's ICmp(i32, String*) and abort the LLVM verifier.
    {
        auto isNumericEarly = [](const std::string& t) {
            return t == "Int" || t == "int" || t == "Double" || t == "double";
        };
        auto isUnknownEarly = [this](const std::string& t) {
            // `void` is the FunctionNode default before Pass-2 infers a
            // return type — treat it the same as Any here so we don't
            // reject correct programs whose callee's return type Sema
            // didn't resolve in time (e.g. resp.status_code coming back
            // as void because the field path lost its annotation).
            // Generic type params (T, K, V, …) in scope are also unknown
            // — `self.value * 2` inside a `Box[T]` method body must
            // compile; codegen falls back to quirk_opaque_to_int for
            // the i8* unbox at runtime.
            if (t.empty() || t == "Any" || t == "Null" || t == "auto" || t == "void") return true;
            return isGenericParam(t);
        };
        bool arith = (node->op == "+" || node->op == "-" || node->op == "*" ||
                      node->op == "/" || node->op == "%");
        bool cmp   = (node->op == "==" || node->op == "!=" || node->op == "<" ||
                      node->op == "<=" || node->op == ">"  || node->op == ">=");
        if (arith || cmp) {
            bool ok = false;
            // Same type is always fine (covers enum==enum, String==String,
            // user-struct==user-struct via overload, etc.).
            if (lType == rType) ok = true;
            // Anything against Any/Null/empty/auto is deferred.
            else if (isUnknownEarly(lType) || isUnknownEarly(rType)) ok = true;
            // Numeric coercion across Int/Double.
            else if (isNumericEarly(lType) && isNumericEarly(rType)) ok = true;
            // `+` with String operand is concat — handled below.
            else if (node->op == "+" && (lType == "String" || rType == "String")) ok = true;
            // Struct on the LHS with a user-defined __op overload —
            // trust the overload. Built-in primitives (Int / Double /
            // Bool / Char / String) are registered as structs and have
            // findable __eq/__add methods, but Codegen handles them as
            // primitives at the IR level (raw ICmp / CreateAdd) and
            // doesn't actually dispatch to those methods. Exclude
            // them so a primitive-mismatch (Int == String) doesn't
            // silently slip through.
            else if (structRegistry.count(lType)) {
                static const std::set<std::string> primitivesAsStructs = {
                    "Int","Double","Bool","Char","String",
                    "int","double","bool","char","string"
                };
                if (!primitivesAsStructs.count(lType)) {
                    static const std::map<std::string, std::string> magicMap = {
                        {"+","__add"},{"-","__sub"},{"*","__mul"},{"/","__div"},{"%","__mod"},
                        {"==","__eq"},{"!=","__ne"},{"<","__lt"},{"<=","__le"},
                        {">","__gt"},{">=","__ge"},
                    };
                    auto m = magicMap.find(node->op);
                    if (m != magicMap.end()) {
                        FunctionNode* fn = findMethod(lType, lType + "_" + m->second);
                        if (!fn && node->op == "!=")
                            fn = findMethod(lType, lType + "___eq");
                        if (fn) ok = true;
                    }
                }
            }
            if (!ok) {
                fatalError("operator '" + node->op + "' incompatible types: '" +
                           lType + "' and '" + rType + "'",
                           node->line, node->col, node->filePath);
            }
        }
    }

    // Operator Overloading
    if (structRegistry.count(lType))
    {
        std::string magic;
        if (node->op == "+")       magic = "__add";
        else if (node->op == "-")  magic = "__sub";
        else if (node->op == "*")  magic = "__mul";
        else if (node->op == "/")  magic = "__div";
        else if (node->op == "%")  magic = "__mod";
        else if (node->op == "==") magic = "__eq";
        else if (node->op == "!=") magic = "__ne";
        else if (node->op == "<")  magic = "__lt";
        else if (node->op == "<=") magic = "__le";
        else if (node->op == ">")  magic = "__gt";
        else if (node->op == ">=") magic = "__ge";

        if (!magic.empty())
        {
            FunctionNode* fn = findMethod(lType, lType + "_" + magic);
            // For !=, fall back to __eq if __ne is not defined
            if (!fn && node->op == "!=") fn = findMethod(lType, lType + "___eq");
            if (fn) {
                static const std::set<std::string> boolOps = {"==","!=","<","<=",">",">="};
                if (boolOps.count(node->op)) return "Bool";
                return fn->returnType;
            }
        }
    }

    // Primitives
    // Type-compatibility helpers for arithmetic / comparison. Both
    // operands need to be in the same "kind" — Codegen otherwise emits
    // an ICmp against incompatible LLVM types and LLVM's verifier
    // aborts the whole process (no traceback, no chance to catch).
    auto isNumeric = [](const std::string& t) {
        return t == "Int" || t == "int" || t == "Double" || t == "double";
    };
    auto isUnknown = [this](const std::string& t) {
        // Empty / "Any" / "Null" / "auto" / "void" all defer to runtime.
        // `void` shows up when Sema can't resolve a member-access result
        // type (e.g. resp.status_code through a Map.get chain) — treating
        // it as an error here misclassifies correct code. Generic type
        // params (T, K, V, …) currently in scope are also unknown for
        // operator-typing purposes — `self.value * 2` inside a `Box[T]`
        // method body must compile, and at the use site Sema's type-
        // substitution narrows the result correctly. Codegen falls back
        // to `quirk_opaque_to_int` for the i8* unbox at runtime.
        if (t.empty() || t == "Any" || t == "Null" || t == "auto" || t == "void") return true;
        return isGenericParam(t);
    };
    auto compatibleOperands = [&](const std::string& a, const std::string& b) {
        if (a == b) return true;
        if (isUnknown(a) || isUnknown(b)) return true;
        if (isNumeric(a) && isNumeric(b)) return true;
        // Enum vs same-enum already handled by a==b above; enum vs Int
        // is intentionally rejected (use `.value` if you want the int).
        return false;
    };

    if (node->op == "+")
    {
        // `+` is also string concat. Anything + String / String + anything
        // is allowed; numeric + numeric is allowed; otherwise error.
        if (lType == "String" || rType == "String")
            return "String";
        // List concatenation (v3.15.0): `xs + ys` dispatches to
        // `List.__add` and produces a fresh List with `self` ahead of
        // `other`. Pre-v3.15.0 Sema fell through to the numeric
        // compatible-operands branch and typed the expression as Int,
        // which both surfaced wrong types downstream and caused
        // Codegen to emit raw integer add — at runtime it SIGSEGV'd
        // dereferencing the List pointers as integers.
        if (lType == "List" && rType == "List")
            return "List";
        if (compatibleOperands(lType, rType)) {
            if (lType == "Double" || rType == "Double")
                return "Double";
            return "Int";
        }
        fatalError("'+' operands must be numeric or include a String; got '" +
                   lType + "' and '" + rType + "'",
                   node->line, node->col, node->filePath);
    }
    if (node->op == "-" || node->op == "*" || node->op == "/" || node->op == "%")
    {
        // Arithmetic — both sides must be numeric (or unknown / deferred).
        // Enums are intentionally rejected: `Color.Red + 1` was silently
        // accepted before and produced a wrong-typed Bool/Int at codegen.
        //
        // The `a == b` shortcut in compatibleOperands accepts equal
        // types (`String == String`, `Bool == Bool`) — fine for `==`
        // but wrong for `*` / `-` / `/` / `%`, where only numerics
        // (and unknown/generic placeholders) make sense. Without this
        // explicit numeric check, `"x" * "y"` falls through to Codegen
        // and emits `mul %String* %a, %b` → IR verifier rejection.
        bool eachOk = (isNumeric(lType) || isUnknown(lType)) &&
                      (isNumeric(rType) || isUnknown(rType));
        if (eachOk && !enumRegistry.count(lType) && !enumRegistry.count(rType)) {
            if (lType == "Double" || rType == "Double")
                return "Double";
            return "Int";
        }
        fatalError("'" + node->op + "' operands must be numeric; got '" +
                   lType + "' and '" + rType + "'",
                   node->line, node->col, node->filePath);
    }
    // Allow comparison between same enum types
    if (node->op == "==" || node->op == "!=") {
        if (enumRegistry.count(lType) && lType == rType) return "Bool";
        // `enum vs same-typed Any / Null` is fine — covers `g != null`,
        // `g == returned_any`. But enum vs unrelated type is rejected.
        if (enumRegistry.count(lType) || enumRegistry.count(rType)) {
            const std::string& other = enumRegistry.count(lType) ? rType : lType;
            if (isUnknown(other)) return "Bool";
            fatalError("cannot compare enum '" +
                       (enumRegistry.count(lType) ? lType : rType) +
                       "' with '" + other + "'",
                       node->line, node->col, node->filePath);
        }
    }

    if (node->op == ">" || node->op == "<" || node->op == ">=" ||
        node->op == "<=" || node->op == "==" || node->op == "!=")
    {
        if (compatibleOperands(lType, rType)) return "Bool";
        fatalError("cannot compare '" + lType + "' with '" + rType +
                   "' (operator '" + node->op + "')",
                   node->line, node->col, node->filePath);
    }
    fatalError("unsupported operator '" + node->op + "' on types '" + lType + "' and '" + rType + "'",
               node->line, node->col, node->filePath);
}

std::string Sema::checkMemberAccess(MemberAccessNode *node)
{
    lastNode = node;
    // Enum variant access: Direction.North
    if (auto lit = dynamic_cast<LiteralNode*>(node->object.get())) {
        if (enumRegistry.count(lit->value)) {
            EnumNode* en = enumRegistry[lit->value];
            // v2.3.1+: the class-level accessors `.values`, `.names`,
            // `.variants` and the instance accessors `.value`, `.name`,
            // `.ordinal` are now methods (require parens). They're
            // checked via checkCall's member-call branch when invoked
            // as `EnumName.values()` / `g.value()`. Bare property
            // access here is a usage error — point at the new shape.
            static const std::set<std::string> nowMethods = {
                "values", "names", "variants", "value", "name", "ordinal"
            };
            if (nowMethods.count(node->memberName)) {
                fatalError("'" + node->memberName + "' on enum '" + lit->value +
                           "' is a method — write `" + lit->value + "." +
                           node->memberName + "()` (with parens)",
                           node->line, node->col, node->filePath);
            }
            if (node->memberName == "str") return "String";
            auto it = std::find(en->variants.begin(), en->variants.end(), node->memberName);
            if (it == en->variants.end()) {
                std::string msg = "'" + node->memberName + "' is not a variant of enum '" + lit->value + "'";
                msg += formatHint(suggestEnumVariants(lit->value, node->memberName));
                fatalError(msg, node->line, node->col, node->filePath);
            }
            return en->name;
        }
    }

    // v2.3.1+: `.value` / `.ordinal` / `.name` as bare property access
    // on an enum-typed binding is also a usage error (same family).
    // checkCall handles the `g.value()` form below.
    {
        static const std::set<std::string> instanceMethods = {
            "value", "ordinal", "name"
        };
        if (instanceMethods.count(node->memberName)) {
            std::string objType;
            if (auto lit = dynamic_cast<LiteralNode*>(node->object.get())) {
                objType = resolveVariable(lit->value);
            } else {
                objType = checkExpression(node->object.get());
            }
            if (!objType.empty() && objType.back() == '?') objType.pop_back();
            if (enumRegistry.count(objType)) {
                fatalError("'" + node->memberName + "' is a method on enum '" + objType +
                           "' — write `obj." + node->memberName + "()` (with parens)",
                           node->line, node->col, node->filePath);
            }
        }
    }

    std::string objType = checkExpression(node->object.get());
    // Strip Optional marker before method lookup. Preserve the full
    // parameterized form (e.g. `Box[Int]`) in `objTypeFull` so the
    // type-substitution pass below can read out the concrete args;
    // the stripped base name (`Box`) goes through to the actual
    // method/field resolution path.
    if (!objType.empty() && objType.back() == '?') objType.pop_back();
    std::string objTypeFull = objType;
    objType = baseType(objType);

    // Magic attributes
    if (node->memberName == "__name")   return "String";
    if (node->memberName == "__parent") return "String";
    if (node->memberName == "__class")  return "Type";

    if (objType.rfind("MODULE$", 0) == 0)
    {
        std::string modName = objType.substr(7);
        const std::string& funcName = node->memberName;
        if (auto* fn = findMethod("", funcName))
            return fn->returnType;
        {
            std::string msg = "module '" + modName + "' has no function '" + funcName + "'";
            msg += formatHint(suggestModuleFunctions(modName, funcName));
            fatalError(msg, node->line, node->col, node->filePath);
        }
    }

    std::string type = resolveMember(objType, node->memberName);
    // Substitute generic type params with the concrete args from
    // the receiver type. `b: Box[Int]; b.value` returns the raw
    // field type `T` from resolveMember; we now rewrite it to `Int`
    // so downstream callers (operator typing, function-arg checks,
    // chained .member access) see the narrowed type. This is the
    // v3 phase 3-b Sema substitution — the field still lowers as
    // Any* at codegen time, but Sema-level type information is now
    // properly threaded.
    {
        auto sIt = structRegistry.find(objType);
        if (sIt != structRegistry.end() && !sIt->second->typeParams.empty()) {
            std::vector<std::string> typeArgs = extractTypeArgs(objTypeFull);
            if (!typeArgs.empty()) {
                type = substituteTypeParams(type, sIt->second->typeParams, typeArgs);
            }
        }
    }
    if (type == "unknown") {
        // Numeric tuple-index `.0` / `.1` / … on an Any: at runtime
        // the value is often a Tuple (e.g. `pairs.get(0).0` where
        // `pairs: List<Tuple>`). Codegen has a path that calls the
        // Tuple_get helper on the underlying i8*, so let it through
        // and type the result as Any. Without this, `t: Tuple :=
        // pairs.get(i)` was forced on every tuple-in-collection read.
        if (objType == "Any" && !node->memberName.empty() &&
            std::all_of(node->memberName.begin(), node->memberName.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            return "Any";
        }
        std::string msg = "'" + objType + "' has no member '" + node->memberName + "'";
        // The usual cause of an unexpected `Any` is a value flowing through
        // a `Callable` (whose return is type-erased) or a Map/List read.
        // The typed-walrus annotation re-narrows the binding and lets the
        // codegen unbox at the assignment — pointing at it turns a
        // head-scratcher into a one-line fix at the callsite.
        if (objType == "Any") {
            msg += "\n  hint: 'Any' values (e.g. Callable returns, Map.get) "
                   "don't carry struct types; annotate the binding to unbox, "
                   "e.g. `resp: T := <expr>`";
        } else {
            // Concrete receiver: surface the closest field/method name
            // so typos like `list.fls` (instead of `filter`) become a
            // one-line fix.
            msg += formatHint(suggestMembers(baseType(objType), node->memberName));
        }
        fatalError(msg, node->line, node->col, node->filePath);
    }
    if (type == "method") {
        if (auto* fn = findMethod(objType, objType + "_" + node->memberName)) {
            const std::string& ret = fn->returnType;
            if (ret.empty() || ret == "auto") return "Any";
            return ret;
        }
        if (structRegistry.count(objType)) {
            for (const auto& par : structRegistry[objType]->parents) {
                std::string pf = par + "_" + node->memberName;
                if (methodRegistry[par].count(pf)) {
                    std::string ret = methodRegistry[par][pf]->returnType;
                    if (ret.empty() || ret == "auto") return "Any";
                    return ret;
                }
            }
        }
        return "Any";
    }
    return type;
}

void Sema::checkInitArgTypes(const std::string& name, FunctionNode* init,
                              const std::vector<std::string>& argTypes,
                              int line, int col, const std::string& filePath) {
    for (size_t i = 0; i < argTypes.size() && i < init->parameters.size(); ++i) {
        const auto& param = init->parameters[i];
        if (param.isVariadic) break;
        const std::string& paramType = param.type;
        const std::string& argType   = argTypes[i];
        if (paramType.empty()) continue;       // untyped param — accept anything
        if (argType.empty()) continue;         // unknown arg type — defer
        if (isCompatibleTypes(paramType, argType)) continue;
        fatalError("argument " + std::to_string(i + 1) + " of " + name +
                   "() expected '" + paramType + "' but got '" + argType + "'",
                   line, col, filePath);
    }
}

void Sema::checkInitArgCount(const std::string& name, FunctionNode* init,
                              int provided, int line, int col, const std::string& filePath) {
    int required = 0, total = 0;
    bool hasVariadic = false;
    for (const auto& p : init->parameters) {
        if (p.isVariadic) { hasVariadic = true; break; }
        total++;
        if (!p.defaultValue) required++;
    }
    if (hasVariadic || (provided >= required && provided <= total)) return;

    std::string expect = (required == total)
        ? std::to_string(required) + " argument" + (required == 1 ? "" : "s")
        : "between " + std::to_string(required) + " and " + std::to_string(total) + " arguments";
    fatalError(name + "() takes " + expect +
               " but " + std::to_string(provided) + " " + (provided == 1 ? "was" : "were") + " given",
               line, col, filePath);
}

std::string Sema::checkConstructor(ConstructorNode *node)
{
    lastNode = node;
    if (!structRegistry.count(node->structName)) return "unknown";

    std::string initName = node->structName + "__init";
    if (methodRegistry.count(node->structName) &&
        methodRegistry[node->structName].count(initName)) {
        FunctionNode* init = methodRegistry[node->structName][initName];
        checkInitArgCount(node->structName, init,
                          (int)node->args.size(), node->line, node->col, node->filePath);
        std::vector<std::string> argTypes;
        argTypes.reserve(node->args.size());
        for (auto& a : node->args) argTypes.push_back(checkExpression(a.value.get()));
        checkInitArgTypes(node->structName, init, argTypes,
                          node->line, node->col, node->filePath);
    }

    return node->structName;
}

std::string Sema::checkCall(CallNode *node)
{
    lastNode = node;
    if (auto l = dynamic_cast<LiteralNode *>(node->callee.get())) {
        if (l->value == "super") {
            if (currentClass.empty())
                fatalError("'super' used outside of a struct method", node->line, node->col, node->filePath);
            if (structRegistry[currentClass]->parents.empty())
                fatalError("'" + currentClass + "' has no parent — cannot use 'super'",
                           node->line, node->col, node->filePath);
            return structRegistry[currentClass]->parents[0];
        }

        // Always check all argument expressions so variables inside them
        // are marked used. Collect the resulting types so positional ctor
        // calls below can validate each arg against the matching param.
        std::vector<std::string> argTypes;
        argTypes.reserve(node->args.size());
        for (auto& a : node->args) argTypes.push_back(checkExpression(a.value.get()));

        // Positional constructor call: Foo(...) where Foo is a known struct.
        // Validate argument count against __init (self already stripped by parser).
        if (structRegistry.count(l->value)) {
            std::string initName = l->value + "__init";
            if (methodRegistry.count(l->value) &&
                methodRegistry[l->value].count(initName)) {
                FunctionNode* init = methodRegistry[l->value][initName];
                checkInitArgCount(l->value, init,
                                  (int)node->args.size(), node->line, node->col, node->filePath);
                checkInitArgTypes(l->value, init, argTypes,
                                  node->line, node->col, node->filePath);
            }
            return l->value;
        }

        // Backed-enum value lookup: `Gender("Male")` where Gender is
        // declared `enum Gender(String) { ... }`. Takes exactly one
        // argument of the backing type and returns the enum. Throws
        // ValueError at runtime on no-match — for null-on-miss callers,
        // use `Gender.parse(...)` (routed through the member-access path).
        if (enumRegistry.count(l->value) && !enumRegistry[l->value]->backingType.empty()) {
            EnumNode* en = enumRegistry[l->value];
            if (node->args.size() != 1) {
                fatalError(l->value + "(...) expects exactly one argument of type '" +
                           en->backingType + "' (got " + std::to_string(node->args.size()) + ")",
                           node->line, node->col, node->filePath);
            }
            const std::string& argType = argTypes[0];
            if (!isCompatibleTypes(en->backingType, argType)) {
                fatalError(l->value + "(...) expects '" + en->backingType +
                           "' but got '" + argType + "'",
                           node->line, node->col, node->filePath);
            }
            return l->value;
        }

        {
            std::string vtype = resolveVariable(l->value);

            // Hard error: bare call into a module that was imported with `use X`
            // but NOT explicitly named via `from X use { name }`. Forbids:
            //     use slug; slug(base)          // ← error
            // while still allowing the two valid forms:
            //     use slug; slug.slug(base)     // dotted access
            //     from slug use { slug }; slug(base)
            if (vtype.rfind("MODULE$", 0) == 0) {
                const std::string& ctxMod = currentFunctionNode ? currentFunctionNode->moduleName : "main";
                // Single map lookup instead of count+[] (each was O(log n)).
                bool explicitlyImported = false;
                auto mvIt = moduleVisibility.find(ctxMod);
                if (mvIt != moduleVisibility.end()) {
                    explicitlyImported = mvIt->second.visibleSymbols.count(l->value) > 0;
                }
                if (!explicitlyImported) {
                    std::string mod = vtype.substr(7);
                    std::replace(mod.begin(), mod.end(), '/', '.');
                    fatalError(
                        "cannot call module '" + mod + "' directly. Use '" + mod + "." + l->value
                        + "(...)' or import the function with 'from " + mod + " use { " + l->value + " }'",
                        node->line, node->col, node->filePath);
                }
                // Explicitly imported AND module-aliased — prefer the function.
                FunctionNode* fn = lookupTopLevel(l->value);
                if (fn) {
                    if (!fn->linkageName.empty() && fn->linkageName != l->value)
                        node->resolvedLinkageName = fn->linkageName;
                    return fn->returnType.empty() ? "void" : fn->returnType;
                }
            }

            // Calling a Callable variable or a generic-param value — return Any
            if (vtype == "Callable" || isGenericParam(vtype)) return "Any";

            // Pass 1 freezes the scope binding to whatever returnType was
            // parsed (or the FunctionNode default "void" when no `-> T`).
            // Pass 2 later infers the real return type from `return`
            // statements and writes it back to the FunctionNode, but the
            // scope binding is never updated. Prefer the live registry
            // entry so callers see e.g. "String" for an inferred-return
            // function whose body returns a String.
            FunctionNode* fn = lookupTopLevel(l->value);
            if (fn) {
                if (!fn->linkageName.empty() && fn->linkageName != l->value)
                    node->resolvedLinkageName = fn->linkageName;
                // Type-check positional arguments against the function's
                // declared parameters. Same shape as the struct-ctor
                // check that landed in 2.2.2 — catches things like
                // `User(name, age, null)` for `age: Int`.
                // If *any* arg has a name, the call uses keyword form
                // (`f(x=1, y=2)`) and positional matching no longer
                // applies — Codegen reorders by name. Skip the
                // positional gate in that case.
                bool anyKwarg = false;
                for (auto& a : node->args) if (!a.name.empty()) { anyKwarg = true; break; }
                // Arity gate: count required (no default) and total
                // params, allow anywhere in between. Skip when the
                // call uses kwargs (Codegen reorders) or when the
                // function is variadic.
                if (!anyKwarg) {
                    int required = 0, total = 0;
                    bool variadic = false;
                    for (const auto& p : fn->parameters) {
                        if (p.isVariadic) { variadic = true; break; }
                        total++;
                        if (!p.defaultValue) required++;
                    }
                    int provided = (int)node->args.size();
                    if (!variadic && (provided < required || provided > total)) {
                        std::string expect = (required == total)
                            ? std::to_string(required) + " argument" + (required == 1 ? "" : "s")
                            : "between " + std::to_string(required) + " and " + std::to_string(total) + " arguments";
                        fatalError(l->value + "() takes " + expect +
                                   " but " + std::to_string(provided) + " " +
                                   (provided == 1 ? "was" : "were") + " given",
                                   node->line, node->col, node->filePath);
                    }
                }
                for (size_t i = 0; !anyKwarg && i < argTypes.size() && i < fn->parameters.size(); ++i) {
                    const auto& param = fn->parameters[i];
                    if (param.isVariadic) break;
                    const std::string& paramType = param.type;
                    const std::string& argType   = argTypes[i];
                    if (paramType.empty() || argType.empty()) continue;
                    if (isCompatibleTypes(paramType, argType)) continue;
                    fatalError("argument " + std::to_string(i + 1) + " of " + l->value +
                               "() expected '" + paramType + "' but got '" + argType + "'",
                               node->line, node->col, node->filePath);
                }
                const std::string& ret = fn->returnType;
                return (ret.empty() || ret == "auto") ? std::string("void") : ret;
            }
            return vtype;
        }
    }

    if (auto m = dynamic_cast<MemberAccessNode *>(node->callee.get()))
    {
        // v2.3.1+: enum class-level accessors as methods.
        // `EnumName.values()` / `.names()` / `.variants()`.
        if (auto* eLit = dynamic_cast<LiteralNode*>(m->object.get())) {
            if (enumRegistry.count(eLit->value) &&
                (m->memberName == "values" ||
                 m->memberName == "names"  ||
                 m->memberName == "variants")) {
                if (!node->args.empty()) {
                    fatalError(eLit->value + "." + m->memberName + "() takes no arguments",
                               node->line, node->col, node->filePath);
                }
                return "List";
            }
        }

        // v2.3.1+: enum instance accessors as methods.
        // `g.value()` / `.ordinal()` / `.name()`. Only fire when
        // memberName is one of the three accessors AND the receiver
        // is plausibly an enum-typed binding. Without these guards
        // we'd call resolveVariable on string/number literals and
        // synthesise spurious "undefined variable" errors for every
        // f-string interpolation (which routes a string literal
        // through this checkCall path).
        if (m->memberName == "value" || m->memberName == "ordinal" || m->memberName == "name") {
            // Skip when the object is the enum class itself —
            // class-level access doesn't go through this branch.
            auto* oLit = dynamic_cast<LiteralNode*>(m->object.get());
            bool isClassLevel = oLit && enumRegistry.count(oLit->value);
            // Skip non-identifier literals (string / numeric / bool / null
            // literals stored as LiteralNode with the raw token text).
            auto isIdentLit = [](const std::string& v) {
                if (v.empty()) return false;
                if (!(std::isalpha((unsigned char)v[0]) || v[0] == '_')) return false;
                for (char c : v) if (!std::isalnum((unsigned char)c) && c != '_') return false;
                return v != "true" && v != "false" && v != "null";
            };
            if (!isClassLevel) {
                std::string objType;
                if (oLit) {
                    if (!isIdentLit(oLit->value)) objType = ""; // not a binding name
                    else objType = resolveVariable(oLit->value);
                } else {
                    objType = checkExpression(m->object.get());
                }
                if (!objType.empty() && objType.back() == '?') objType.pop_back();
                if (enumRegistry.count(objType)) {
                if (m->memberName == "value") {
                    if (!node->args.empty()) {
                        fatalError("value() takes no arguments",
                                   node->line, node->col, node->filePath);
                    }
                    EnumNode* en = enumRegistry[objType];
                    if (en->backingType.empty()) {
                        fatalError("'" + objType + "' has no backing type — `.value()` is only on backed enums",
                                   node->line, node->col, node->filePath);
                    }
                    return en->backingType;
                }
                if (m->memberName == "ordinal") {
                    if (!node->args.empty()) {
                        fatalError("ordinal() takes no arguments",
                                   node->line, node->col, node->filePath);
                    }
                    return "Int";
                }
                if (m->memberName == "name") {
                    if (!node->args.empty()) {
                        fatalError("name() takes no arguments",
                                   node->line, node->col, node->filePath);
                    }
                    return "String";
                }
                }  // close enumRegistry.count(objType)
            }      // close !isClassLevel
        }          // close memberName-in-set

        // `EnumName.parse(v)` — safe lookup. Returns `EnumName?`:
        // a boxed-Any-int ordinal on hit, null on miss. v2.3.0+
        // lowers nullable enums as i8*, so the proper `EnumName?`
        // return type now flows correctly through `match` and `??`.
        if (auto* eLit = dynamic_cast<LiteralNode*>(m->object.get())) {
            if (m->memberName == "parse" && enumRegistry.count(eLit->value) &&
                !enumRegistry[eLit->value]->backingType.empty()) {
                EnumNode* en = enumRegistry[eLit->value];
                for (auto& a : node->args) checkExpression(a.value.get());
                if (node->args.size() != 1) {
                    fatalError(eLit->value + ".parse(...) expects exactly one argument of type '" +
                               en->backingType + "' (got " + std::to_string(node->args.size()) + ")",
                               node->line, node->col, node->filePath);
                }
                return eLit->value + "?";
            }
        }

        std::string objType = checkExpression(m->object.get());
        // Strip Optional marker and generic type args before method lookup
        if (!objType.empty() && objType.back() == '?') objType.pop_back();
        objType = baseType(objType);

        // --- THE FIX: Handle Module Constructor Calls (e.g. io.File) ---
        if (objType.rfind("MODULE$", 0) == 0) {
            std::vector<std::string> argTypes;
            argTypes.reserve(node->args.size());
            for (auto& a : node->args) argTypes.push_back(checkExpression(a.value.get()));
            std::string funcName = m->memberName;

            // 1. Is it a Struct Constructor?
            if (structRegistry.count(funcName)) {
                return funcName;
            }

            // 2. Is it a standard Module Function?
            FunctionNode* fn = lookupTopLevel(funcName);
            if (fn) {
                bool anyKwarg = false;
                for (auto& a : node->args) if (!a.name.empty()) { anyKwarg = true; break; }
                if (!anyKwarg) {
                    // Arity gate — same shape as the free-fn check
                    // above. Catches `test.assert_eq(x,)` (1 arg
                    // when 2 required) before Codegen emits a call
                    // with too few operands and trips the verifier.
                    int required = 0, total = 0;
                    bool variadic = false;
                    for (const auto& p : fn->parameters) {
                        if (p.isVariadic) { variadic = true; break; }
                        total++;
                        if (!p.defaultValue) required++;
                    }
                    int provided = (int)node->args.size();
                    if (!variadic && (provided < required || provided > total)) {
                        std::string expect = (required == total)
                            ? std::to_string(required) + " argument" + (required == 1 ? "" : "s")
                            : "between " + std::to_string(required) + " and " + std::to_string(total) + " arguments";
                        fatalError(funcName + "() takes " + expect +
                                   " but " + std::to_string(provided) + " " +
                                   (provided == 1 ? "was" : "were") + " given",
                                   node->line, node->col, node->filePath);
                    }
                    // Type-check matching positional args.
                    for (size_t i = 0; i < argTypes.size() && i < fn->parameters.size(); ++i) {
                        const auto& param = fn->parameters[i];
                        if (param.isVariadic) break;
                        const std::string& paramType = param.type;
                        const std::string& argType   = argTypes[i];
                        if (paramType.empty() || argType.empty()) continue;
                        if (isCompatibleTypes(paramType, argType)) continue;
                        fatalError("argument " + std::to_string(i + 1) + " of " + funcName +
                                   "() expected '" + paramType + "' but got '" + argType + "'",
                                   node->line, node->col, node->filePath);
                    }
                }
                return fn->returnType;
            }

            // Neither a struct constructor nor a known top-level
            // function in any loaded module. Before v3.10.0 Sema
            // silently returned "void" here and Codegen failed late
            // with `Unknown function 'X'` and no hint. Surfacing the
            // error at Sema time lets us suggest near-by names from
            // the actual module the user wrote (`net.gte` → `get`).
            {
                std::string modName = objType.substr(7);  // strip MODULE$
                std::replace(modName.begin(), modName.end(), '/', '.');
                std::string msg = "module '" + modName + "' has no function '" + funcName + "'";
                msg += formatHint(suggestModuleFunctions(modName, funcName));
                fatalError(msg, node->line, node->col, node->filePath);
            }
            return "void";
        }
        // ---------------------------------------------------------------

        if (objType == "int") objType = "Int";
        else if (objType == "double") objType = "Double";
        else if (objType == "bool") objType = "Bool";
        else if (objType == "cstring" || objType == "string" || objType == "char") objType = "String";

        // Builtin method return types for core primitives.
        // These are defined in Quirk's core library files which may not always
        // be loaded, so we hard-code their return types here as a fallback to
        // prevent false type errors (e.g. String.to_int() resolving as 'void').
        static const std::map<std::string, std::map<std::string, std::string>> builtinMethods = {
            {"String", {
                {"to_int",     "Int"},
                {"to_float",   "Double"},
                {"to_double",  "Double"},
                {"to_bool",    "Bool"},
                {"lower",      "String"},
                {"upper",      "String"},
                {"is_alpha",   "Bool"},
                {"is_digit",   "Bool"},
                {"is_space",   "Bool"},
                {"is_upper",   "Bool"},
                {"is_lower",   "Bool"},
                {"trim",       "String"},
                {"strip",      "String"},
                {"split",      "List"},
                {"join",       "String"},
                {"find",       "Int"},
                {"contains",   "Bool"},
                {"startswith", "Bool"},
                {"endswith",   "Bool"},
                {"replace",    "String"},
                {"substring",  "String"},
                {"str",        "String"},
                {"format",     "String"},
            }},
            {"Int", {
                {"str",        "String"},
                {"to_float",   "Double"},
                {"to_double",  "Double"},
                {"parse",      "Int"},
            }},
            {"Double", {
                {"str",        "String"},
                {"to_int",     "Int"},
                {"parse",      "Double"},
            }},
            {"Bool", {
                {"str",        "String"},
                {"parse",      "Bool"},
            }},
            {"List", {
                {"get",        "Any"},
                {"length",     "Int"},
                {"append",     "void"},
                {"pop",        "Any"},
                {"join",       "String"},
            }},
            {"Map", {
                {"get",        "Any"},
                {"put",        "void"},
                {"contains",   "Bool"},
                {"length",     "Int"},
            }},
        };
        auto bmIt = builtinMethods.find(objType);
        if (bmIt != builtinMethods.end()) {
            auto mIt = bmIt->second.find(m->memberName);
            if (mIt != bmIt->second.end()) {
                for (auto& a : node->args) checkExpression(a.value.get());
                return mIt->second;
            }
        }

        if (structRegistry.count(objType))
        {
            auto searchMethod = [&](const std::string& currentType, auto& self) -> std::string {
                auto sIt = structRegistry.find(currentType);
                if (sIt == structRegistry.end()) return "";

                std::string funcName = currentType + "_" + m->memberName;
                // __init is stored as `<Type>__init` (single underscore
                // connector swallowed by the double-underscore method
                // name) while every other dunder lands at
                // `<Type>___<dunder>` (triple). Without this fallback,
                // `super().__init(complex_arg)` skipped the arg-type
                // check entirely — and `"x" * msg` slipped past Sema
                // → Codegen ICE on `mul %String*, %String*`.
                FunctionNode* func = findMethod(currentType, funcName);
                if (!func && m->memberName == "__init") {
                    func = findMethod(currentType, currentType + "__init");
                }
                if (func) {
                    for (size_t i = 0; i < node->args.size() && i < func->parameters.size(); ++i) {
                        if (func->parameters[i].isVariadic) break;
                        std::string argType = checkExpression(node->args[i].value.get());
                        const std::string& paramType = func->parameters[i].type;
                        if (!isCompatibleTypes(paramType, argType))
                            fatalError("argument " + std::to_string(i + 1) + " of '" + funcName +
                                       "' expected '" + paramType + "' but got '" + argType + "'",
                                       node->line, node->col, node->filePath);
                    }
                    const std::string& ret = func->returnType;
                    return (ret.empty() || ret == "auto") ? std::string("Any") : ret;
                }

                for (const std::string& parent : sIt->second->parents) {
                    std::string res = self(parent, self);
                    if (!res.empty()) return res;
                }
                return "";
            };

            std::string retType = searchMethod(objType, searchMethod);
            if (!retType.empty()) return retType;
        }
    }
    // Last resort: the callee is some other expression form, e.g. a chained
    // call like `dec(foo)(arg)` produced by decorator desugaring. Type-check
    // the callee; if it yields a Callable, treat the call's return type as
    // `Any` (same policy as calling a Callable variable above). Restricted
    // to callees we haven't already classified to avoid re-entering the
    // MemberAccess/Literal paths and changing existing error reporting.
    if (!dynamic_cast<LiteralNode*>(node->callee.get())
     && !dynamic_cast<MemberAccessNode*>(node->callee.get())) {
        for (auto& a : node->args) checkExpression(a.value.get());
        std::string calleeTy = checkExpression(node->callee.get());
        // Calling something whose type is `Callable` or `Any` produces an
        // unknown (Any) value. Calling something of any other concrete type
        // is a type error elsewhere; here we just bail out as void.
        if (calleeTy == "Callable" || calleeTy == "Any") return "Any";
    }
    return "void";
}

std::string Sema::checkListLiteral(ListLiteralNode *node)
{
    lastNode = node;
    if (!structRegistry.count("List"))
        fatalError("'List' type not available — is core loaded?", node->line, node->col, node->filePath);
    for (auto &elem : node->elements)
        checkExpression(elem.get());
    return "List";
}

std::string Sema::checkMapLiteral(MapLiteralNode *node)
{
    lastNode = node;
    if (!structRegistry.count("Map"))
        fatalError("'Map' type not available — is core loaded?", node->line, node->col, node->filePath);
    for (auto &pair : node->elements)
    {
        std::string keyType = checkExpression(pair.first.get());
        if (keyType != "String")
            fatalError("map keys must be 'String', got '" + keyType + "'",
                       node->line, node->col, node->filePath);
        checkExpression(pair.second.get());
    }
    return "Map";
}


// Primitive/builtin type names that are always "known"
static bool isKnownType(const std::string& t) {
    static const std::set<std::string> known = {
        "Int","Double","Bool","Char","String","Any","void","List","Map","Tuple",
        "Set","Queue","File","Callable","auto","unknown",
        "int","double","bool","char","string","cstring",
        "Null","null"
    };
    return known.count(t) > 0;
}

bool Sema::isGenericParam(const std::string& t) const {
    if (typeAliases.count(t) && typeAliases.at(t) == "Any") return true;
    return !isKnownType(t) && !structRegistry.count(t) && !enumRegistry.count(t);
}

bool Sema::isCompatibleTypes(const std::string &expected, const std::string &actual)
{
    if (expected == actual) return true;
    if (isGenericParam(expected) || isGenericParam(actual)) return true;
    if (expected == "Any" || actual == "Any") return true;

    // Null compatibility: `null` (actual="Null") is allowed for any
    // pointer-like target — nullable annotations (`T?`), structs,
    // collection/reference types, enums-nope (no null state), and
    // primitives-nope (no null state either). Without the primitive
    // and enum carve-outs, `User(name, age, null)` for `age: Int`
    // used to flow through Sema and resurface as malformed-IR / a
    // SIGSEGV on the first dereference.
    if (expected == "Null" || actual == "Null") {
        const std::string& other = (expected == "Null") ? actual : expected;
        if (other.empty()) return true;                  // untyped param — defer
        if (!other.empty() && other.back() == '?') return true;
        // Primitive scalars have no null state.
        if (other == "Int" || other == "int" ||
            other == "Double" || other == "double" ||
            other == "Bool" || other == "bool" ||
            other == "Char" || other == "char") return false;
        // Enums lower to i32 ordinals — no representation for null.
        if (enumRegistry.count(other)) return false;
        // Everything else (structs, Any, collections, Callable, …) is
        // reference-shaped and can hold a null pointer at the IR level.
        return true;
    }

    // Enum compatibility: same-enum already matched above. An enum lowers
    // to i32 at codegen time, so it's bidirectionally compatible with
    // Int — useful for e.g. `Direction.North.to_int()` and explicit
    // `Int` ↔ enum casts. But anything else against an enum (String,
    // Bool, a different enum) is a real mismatch — without this, every
    // arg-type check that mentions an enum used to silently pass.
    bool expIsEnum = enumRegistry.count(expected) > 0;
    bool actIsEnum = enumRegistry.count(actual) > 0;
    if (expIsEnum || actIsEnum) {
        const std::string& other = expIsEnum ? actual : expected;
        return other == "Int" || other == "int";
    }

    // Implicit widening coercions
    if (expected == "Double" && actual == "Int") return true;
    if (expected == "double" && actual == "Int") return true;
    if (expected == "Char"   && actual == "Int") return true;

    // Pointer compatibility
    bool expIsPtr = (expected == "Any" || expected == "String");
    bool actIsPtr = (actual   == "Any" || actual   == "String");
    if (expIsPtr && actIsPtr) return true;

    // Struct inheritance: `Ok` (parent `Result`) is compatible with
    // anything declared as `Result`. Walks the parent chain
    // transitively. Drives both struct subtyping in the wild and
    // tagged-union variant→union assignability (v2.4 — each variant
    // is desugared into a child StructNode with `parents = {union}`).
    if (structRegistry.count(actual)) {
        std::function<bool(const std::string&)> walks = [&](const std::string& s) -> bool {
            auto it = structRegistry.find(s);
            if (it == structRegistry.end()) return false;
            for (const auto& p : it->second->parents) {
                if (p == expected) return true;
                if (walks(p)) return true;
            }
            return false;
        };
        if (walks(actual)) return true;
    }

    return false;
}

bool Sema::inheritsFromException(const std::string& typeName, const std::string& baseType)
{
    if (typeName == baseType) return true;
    if (!structRegistry.count(typeName)) return false;
    for (const auto& parent : structRegistry.at(typeName)->parents) {
        if (inheritsFromException(parent, baseType)) return true;
    }
    return false;
}

// Find the right top-level FunctionNode for `name` from the
// perspective of the current call site. When two packages export
// the same name (e.g. `html.input` vs `console.input`), the
// single-slot `methodRegistry[""][name]` is whichever Pass 1 saw
// last — so console's internal `input(prompt_str)` would route
// through html's signature and trip arity. We track collisions in
// `topLevelOverloads` and disambiguate here by preferring a
// candidate visible from the calling function's module, falling
// back to the single-slot entry.
FunctionNode* Sema::lookupTopLevel(const std::string& name) {
    // v3.11.0: dereference import aliases from the caller's module.
    // `from url use { parse as url_parse }` records `url_parse →
    // parse` in the current module's importAliases; resolving
    // `url_parse(args)` substitutes the source name so the rest of
    // the lookup chain (overload disambig + methodRegistry) finds
    // the canonical FunctionNode and its linkage name.
    std::string effective = name;
    {
        const std::string& ctxMod = currentFunctionNode
            ? currentFunctionNode->moduleName : "main";
        auto mvIt = moduleVisibility.find(ctxMod);
        if (mvIt != moduleVisibility.end()) {
            auto aIt = mvIt->second.importAliases.find(name);
            if (aIt != mvIt->second.importAliases.end()) {
                effective = aIt->second;
            }
        }
    }
    const std::string& lookupName = effective;
    auto cit = topLevelOverloads.find(lookupName);
    if (cit != topLevelOverloads.end() && cit->second.size() > 1) {
        std::string contextModule = currentFunctionNode
            ? currentFunctionNode->moduleName : "main";
        // 1. Strong preference: same module as the caller. Console's
        //    own `prompt_default` calling `input(...)` resolves here
        //    directly to console.input regardless of which other
        //    packages have the same function name.
        for (FunctionNode* f : cit->second) {
            if (f->moduleName == contextModule) return f;
        }
        // 2. Caller wrote `from X use { name }` — pick the
        //    candidate whose package prefix matches the source
        //    the user explicitly named.
        //
        //    The user-written source ("html") rarely equals the
        //    canonical moduleName ("html.src.tag.index") because
        //    Sema derives moduleName from the file path. A prefix
        //    match keeps the comparison working: a candidate from
        //    "html.<anything>" matches a `from html use { ... }`
        //    import.
        auto pkgOf = [](const std::string& m) -> std::string {
            auto dot = m.find('.');
            return dot == std::string::npos ? m : m.substr(0, dot);
        };
        auto mvIt = moduleVisibility.find(contextModule);
        if (mvIt != moduleVisibility.end()) {
            auto srcIt = mvIt->second.visibleSymbolSources.find(name);
            if (srcIt != mvIt->second.visibleSymbolSources.end()) {
                const std::string& src = srcIt->second;
                std::string srcPkg = pkgOf(src);
                for (FunctionNode* f : cit->second) {
                    if (f->moduleName == src) return f;
                    if (pkgOf(f->moduleName) == srcPkg) return f;
                }
            }
            for (FunctionNode* f : cit->second) {
                if (mvIt->second.visibleModules.count(f->moduleName))
                    return f;
            }
        }
        // 3. Fallback: any visible candidate.
        for (FunctionNode* f : cit->second) {
            if (isVisible(name, f->moduleName, contextModule)) return f;
        }
        // 4. No clear winner — fall through to the single-slot
        //    entry below, which preserves the historical
        //    last-write-wins behaviour for cases that worked
        //    before this disambiguation existed.
    }
    auto it = methodRegistry[""].find(name);
    return it == methodRegistry[""].end() ? nullptr : it->second;
}

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope()
{
    if (scopeStack.empty()) return;
    scopeStack.pop_back();
}

void Sema::defineVariable(const std::string &name, const std::string &type, bool isConst, bool isParam)
{
    if (scopeStack.empty())
        enterScope();
    scopeStack.back()[name] = VarInfo{type, isConst, false, isParam, currentFilePath};
}

std::string Sema::resolveVariable(const std::string &name)
{
    // Every identifier reference Sema actually evaluates lands here.
    // Record the position so `--symbols-json` can ship a usage table
    // to the LSP for semantic find-references and rename. `lastNode`
    // is set by checkExpression entry points to the LiteralNode that
    // names the identifier, so its line/col is precise.
    if (lastNode && lastNode->line > 0) {
        usages.push_back({name, currentScope,
                          lastNode->filePath, lastNode->line, lastNode->col});
    }

    // 1. Check local scopes first.
    // Single find() per scope avoids the double-lookup (count + [] pattern)
    // — resolveVariable is hit on every identifier reference, so this is hot.
    for (int i = scopeStack.size() - 1; i >= 0; i--) {
        auto it = scopeStack[i].find(name);
        if (it != scopeStack[i].end()) {
            it->second.used = true;
            return it->second.type;
        }
    }

    // 2. Check Global Module Aliases
    auto gmaIt = globalModuleAliases.find(name);
    if (gmaIt != globalModuleAliases.end()) return gmaIt->second;

    std::string contextModule = currentFunctionNode ? currentFunctionNode->moduleName : "main";

    if (structRegistry.count(name))
    {
        StructNode *s = structRegistry[name];
        if (!isVisible(name, s->moduleName, contextModule))
            fatalError("symbol '" + name + "' (from '" + s->moduleName + "') is not visible here — did you 'use' it?");
        return name;
    }

    if (FunctionNode* f = lookupTopLevel(name))
    {
        if (!isVisible(name, f->moduleName, contextModule))
            fatalError("function '" + name + "' (from '" + f->moduleName + "') is not visible here — did you 'use' it?");
        std::string ret = f->returnType;
        return ret.empty() ? "void" : ret;
    }

    // Builtins
    if (name == "print" || name == "printf" || name == "free" || name == "exit"
        || name == "write" || name == "writeln")
        return "void";
    if (name == "type" || name == "str")
        return "String";
    if (name == "char_at")
        return "String";
    if (name == "set_char_at")
        return "void";
    if (name == "malloc" || name == "realloc")
        return "Any";
    if (name == "strlen")
        return "Int";
        
    // --- NEW: Super keyword support ---
    if (name == "super") {
        if (!currentClass.empty() && structRegistry.count(currentClass) && !structRegistry[currentClass]->parents.empty()) {
            return structRegistry[currentClass]->parents[0];
        }
        return "void";
    }
    // ----------------------------------

    if (!currentClass.empty())
    {
        if (auto* fn = findMethod(currentClass, currentClass + "_" + name)) {
            const std::string& ret = fn->returnType;
            return ret.empty() ? "void" : ret;
        }
    }

    reportError("undefined variable or function '" + name + "'",
                suggestNames(name));
    return "unknown";  // allow analysis to continue
}

std::string Sema::resolveMember(const std::string &sName, const std::string &mName)
{
    // Strip generic type args before lookup: "List[T]" -> "List"
    const std::string sBase = baseType(sName);

    // Tuple numeric index access: t.0, t.1, ...
    if (sBase == "Tuple" && !mName.empty()) {
        bool isNumeric = true;
        for (char c : mName) if (!std::isdigit((unsigned char)c)) { isNumeric = false; break; }
        if (isNumeric) return "Any";
    }

    // Built-in C-runtime struct fields (from types.h) with no Quirk-side declarations.
    // str.length, list.size etc. used as bare properties resolve correctly to "Int".
    static const std::map<std::string, std::map<std::string, std::string>> builtinFields = {
        {"String", {{"_length", "Int"}, {"_buffer", "Any"}}},
        {"List",   {{"_size",   "Int"}, {"_capacity", "Int"}}},
        {"Map",    {{"_size",   "Int"}, {"_capacity", "Int"}}},
        {"Tuple",  {{"_size", "Int"}}},
        {"File",   {{"_handle", "Any"}, {"is_open", "Bool"}}},
        {"Any",    {{"tag",    "Int"}, {"ival", "Int"}, {"dval", "Double"}}},
    };
    auto bIt = builtinFields.find(sBase);
    if (bIt != builtinFields.end()) {
        auto fIt = bIt->second.find(mName);
        if (fIt != bIt->second.end()) return fIt->second;
    }

    std::string lookupName = sBase;

    if (sBase == "int") lookupName = "Int";
    else if (sBase == "double") lookupName = "Double";
    else if (sBase == "bool") lookupName = "Bool";
    else if (sBase == "char") lookupName = "Char";
    else if (sBase == "string" || sBase == "cstring") lookupName = "String";

    if (!structRegistry.count(lookupName))
        return "unknown";

    auto searchMember = [&](const std::string& currentType, auto& self) -> std::string {
        auto sIt = structRegistry.find(currentType);
        if (sIt == structRegistry.end()) return "unknown";
        StructNode* st = sIt->second;

        for (const auto& f : st->fields)
            if (f.name == mName) return f.type;

        if (findMethod(currentType, currentType + "_" + mName))
            return "method";

        for (const std::string& parentName : st->parents) {
            std::string res = self(parentName, self);
            if (res != "unknown") return res;
        }
        return "unknown";
    };

    return searchMember(lookupName, searchMember);
}

void Sema::checkWith(WithNode *node)
{
    lastNode = node;
    std::string resType = checkExpression(node->resource.get());
    if (!structRegistry.count(resType))
        fatalError("'with' resource must be a struct, got '" + resType + "'",
                   node->line, node->col, node->filePath);
    if (!findMethod(resType, resType + "___enter") ||
        !findMethod(resType, resType + "___exit"))
        fatalError("'" + resType + "' must implement __enter and __exit for use in 'with'",
                   node->line, node->col, node->filePath);
    enterScope();
    defineVariable(node->varName, resType);
    for (auto &stmt : node->body)
        checkStatement(stmt.get());
    exitScope();
}

void Sema::checkReturn(ReturnNode *node)
{
    std::string actual = node->expression ? checkExpression(node->expression.get()) : "void";
    std::string &target = currentFunctionNode->returnType;

    // If the declared return type is a generic type param (e.g. "T"), treat it as "Any"
    // for the purpose of compatibility checking (type erasure).
    if (typeAliases.count(target)) {
        const std::string resolved = typeAliases[target];
        if (resolved == "Any") return; // any return value satisfies a generic return type
    }

    // Infer return type from first return statement when no annotation given
    if (target == "auto" || target.empty())
    {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    // "void" here means checkFunction already defaulted it before seeing this return —
    // treat it as infer-able rather than crashing (handles duplicate file loads)
    if (target == "void" && actual != "void")
    {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    if (!isCompatibleTypes(target, actual))
    {
        std::cerr << "Error: Function " << currentFunctionNode->name
                << " expected " << target << " but got " << actual << std::endl;
        exit(1);
    }
}