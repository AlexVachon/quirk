// Windows implementations of the qplat:: functions declared in
// platform.hpp (native Windows port, docs/WINDOWS-PORT.md). The only
// compiler source that includes <windows.h> — its macros and names clash
// with the compiler's own headers. Built on Windows only.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "platform.hpp"

namespace qplat {

int stdout_cols(int fallback) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (_isatty(1) && GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
    return fallback;
}

static std::string random_name(const std::string& prefix, const std::string& suffix) {
    static std::random_device rd;
    const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string name = prefix;
    for (int i = 0; i < 6; i++) name += chars[rd() % 36];
    return name + suffix;
}

std::string make_temp_dir(const std::string& prefix) {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return "";
    for (int attempt = 0; attempt < 100; attempt++) {
        std::filesystem::path p = base / random_name(prefix, "");
        if (std::filesystem::create_directory(p, ec)) return p.string();
    }
    return "";
}

std::string write_temp_file(const std::string& prefix, const std::string& suffix, const std::string& content) {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return "";
    for (int attempt = 0; attempt < 100; attempt++) {
        std::string path = (base / random_name(prefix, suffix)).string();
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return "";
        }
        DWORD written = 0;
        BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
        CloseHandle(h);
        if (!ok || written != content.size()) { DeleteFileA(path.c_str()); return ""; }
        return path;
    }
    return "";
}

// One argument quoted for CreateProcess's command line (the MSVC CRT
// parsing rules: backslashes only matter before a quote).
static std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t bs = 0;
    for (char ch : a) {
        if (ch == '\\') { bs++; continue; }
        if (ch == '"') { q.append(bs * 2 + 1, '\\'); q += '"'; bs = 0; continue; }
        q.append(bs, '\\'); bs = 0;
        q += ch;
    }
    q.append(bs * 2, '\\');
    q += '"';
    return q;
}

static std::string command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) { if (i) cmd += ' '; cmd += quote_arg(argv[i]); }
    return cmd;
}

bool spawn_capture(const std::vector<std::string>& argv, Child& c) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::string cmd = command_line(argv);
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    CloseHandle(pi.hThread);
    c.process = pi.hProcess;
    c.out = rd;
    return true;
}

std::string read_all(Child& c) {
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (c.out && ReadFile((HANDLE)c.out, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    if (c.out) { CloseHandle((HANDLE)c.out); c.out = nullptr; }
    return out;
}

bool try_wait(Child& c, int* code) {
    if (!c.process || WaitForSingleObject((HANDLE)c.process, 0) != WAIT_OBJECT_0) return false;
    DWORD ec = 0;
    GetExitCodeProcess((HANDLE)c.process, &ec);
    CloseHandle((HANDLE)c.process);
    c.process = nullptr;
    *code = (int)ec;
    return true;
}

int wait_exit(Child& c) {
    if (!c.process) return -1;
    WaitForSingleObject((HANDLE)c.process, INFINITE);
    int code = -1;
    try_wait(c, &code);
    return code;
}

void spawn_detached(const std::vector<std::string>& argv) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = command_line(argv);
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int spawn_wait(const std::string& program, const std::vector<std::string>& argv) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = command_line(argv);
    if (!CreateProcessA(program.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

int run_on_big_stack(int (*fn)(int, char**), int argc, char** argv, size_t stack) {
    struct Args { int (*fn)(int, char**); int argc; char** argv; int result; };
    Args a{fn, argc, argv, 0};
    HANDLE t = CreateThread(nullptr, stack,
        [](LPVOID p) -> DWORD { auto* x = static_cast<Args*>(p); x->result = x->fn(x->argc, x->argv); return 0; },
        &a, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return fn(argc, argv);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return a.result;
}

std::string self_exe() {
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
    return "quirk";
}

std::string native_arch() {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86_64";
}

void sleep_ms(unsigned ms) { Sleep(ms); }

}  // namespace qplat
#endif  // _WIN32
