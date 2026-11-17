#pragma once
// Per-module type names. Struct / enum / interface names are one global
// namespace inside the compiler, so when two files define the same name
// (a user `Button` and `ui.Button`) the compiler driver gives every
// definition outside the main file an internal name — `Button_in_ui` —
// and rewrites the references to it (Compiler.cpp, renameCollidingTypes).
// This map turns those internal names back into what the user wrote
// (`ui.Button`) wherever a name is shown: diagnostics, repr, type().
#include <cctype>
#include <map>
#include <string>

namespace quirk {

inline std::map<std::string, std::string>& typeDisplayNames() {
    static std::map<std::string, std::string> names;
    return names;
}

// The display form of one type name (unchanged when it isn't renamed).
// A method's internal name reads as `module.Type.method`
// (`Response_in_web_send` → `web.Response.send`).
inline std::string displayTypeName(const std::string& name) {
    auto& m = typeDisplayNames();
    auto it = m.find(name);
    if (it != m.end()) return it->second;
    if (name.find("_in_") == std::string::npos) return name;
    for (const auto& [internal, shown] : m) {
        if (name.size() > internal.size() + 1 && name.compare(0, internal.size(), internal) == 0 &&
            name[internal.size()] == '_') {
            std::string rest = name.substr(internal.size() + 1);
            while (!rest.empty() && rest[0] == '_') rest.erase(0, 1);   // `__init` → `init`
            return shown + "." + rest;
        }
    }
    return name;
}

// Every internal name in `text` (a whole identifier) replaced by its
// display form — for diagnostic messages built from type names.
inline std::string displayTypeNames(const std::string& text) {
    auto& m = typeDisplayNames();
    if (m.empty() || text.find("_in_") == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) j++;
            out += displayTypeName(text.substr(i, j - i));
            i = j;
        } else {
            out += text[i++];
        }
    }
    return out;
}

// A display name without its module: `ui.Button` → `Button`,
// `shapes.Box[Int]` → `Box[Int]` — what type() gives.
inline std::string withoutModule(const std::string& shown) {
    size_t br = shown.find('[');
    size_t dot = shown.rfind('.', br == std::string::npos ? std::string::npos : br);
    return dot == std::string::npos ? shown : shown.substr(dot + 1);
}

}  // namespace quirk
