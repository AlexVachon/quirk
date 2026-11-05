// Quirk source file extensions — the one place that knows them.
//
// `.qk` (7.6+) is the preferred extension; `.quirk` keeps working
// everywhere. Wherever a module is looked up by name, each candidate
// path is tried as `.qk` first, then `.quirk`, so a project can mix the
// two and rename files one at a time.
#pragma once
#include <string>
#include <vector>

namespace quirk_src {

// Preferred first.
inline const std::vector<std::string>& exts() {
    static const std::vector<std::string> e = {".qk", ".quirk"};
    return e;
}

// What new files get (`quirk new`, `quirk init`, scaffolded tests).
inline const std::string& preferred() { return exts().front(); }

inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `app.qk` / `app.quirk` → true.
inline bool is_source(const std::string& path) {
    for (const auto& e : exts()) if (ends_with(path, e)) return true;
    return false;
}

// `lib/csv.qk` → `lib/csv`; anything else is returned unchanged.
inline std::string strip(const std::string& path) {
    for (const auto& e : exts())
        if (ends_with(path, e)) return path.substr(0, path.size() - e.size());
    return path;
}

// `*_test.qk` / `*_test.quirk`.
inline bool is_test_file(const std::string& filename) {
    for (const auto& e : exts()) if (ends_with(filename, "_test" + e)) return true;
    return false;
}

// Every extension variant of an extension-less stem, preferred first:
// `pkg/src/index` → {`pkg/src/index.qk`, `pkg/src/index.quirk`}.
inline std::vector<std::string> variants(const std::string& stem) {
    std::vector<std::string> out;
    for (const auto& e : exts()) out.push_back(stem + e);
    return out;
}

}  // namespace quirk_src
