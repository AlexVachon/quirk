// OS headers for the compiler driver (Compiler.cpp, PackageManager.hpp).
// POSIX: exactly the headers they always used. Windows (native port,
// docs/WINDOWS-PORT.md): the Win32 / CRT equivalents plus shims for the
// calls that map one-to-one. Calls with no equivalent (fork / execv /
// waitpid, termios, ioctl) stay errors on Windows until their call sites
// get a Windows branch.
#pragma once

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <vector>
#include <cstdlib>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI           // wingdi.h's ERROR macro breaks `enum { OK, WARN, ERROR }`
#endif
#include <winsock2.h>
#include <windows.h>
// windows.h macros that are also Quirk token / enum names.
#undef ERROR
#undef CONST
#undef IN
#undef OUT
#undef OPTIONAL
#undef TRUE
#undef FALSE
#undef DELETE
#include <io.h>
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <string>

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
#ifndef F_OK
#define F_OK 0
#define X_OK 0          // Windows has no execute bit; existence is the check
#define W_OK 2
#define R_OK 4
#endif
#define isatty   _isatty
#define getcwd   _getcwd
#define chdir    _chdir
#define unlink   _unlink
#define access   _access
#define getpid   _getpid
#define popen    _popen
#define pclose   _pclose
#define strdup   _strdup
typedef int pid_t;
typedef intptr_t ssize_t;

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
inline unsigned int sleep(unsigned int s) { Sleep(s * 1000); return 0; }
inline int usleep(unsigned int us) { Sleep(us / 1000); return 0; }
// system() returns the exit code itself on Windows.
#define WIFEXITED(s)   (1)
#define WEXITSTATUS(s) (s)
#endif

#include <string>
#include <filesystem>
#include <random>

namespace qplat {

// Terminal width of stdout in columns, or `fallback` when it isn't a
// terminal.
inline int stdout_cols(int fallback) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (_isatty(1) && GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
    return fallback;
#else
    struct winsize ws;
    if (::isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return fallback;
#endif
}

// A fresh private directory `<tmp>/<prefix>XXXXXX`; "" on failure.
inline std::string make_temp_dir(const std::string& prefix) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return "";
    std::random_device rd;
    const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 100; attempt++) {
        std::string name = prefix;
        for (int i = 0; i < 6; i++) name += chars[rd() % 36];
        std::filesystem::path p = base / name;
        if (std::filesystem::create_directory(p, ec)) return p.string();
    }
    return "";
#else
    std::string tmpl = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    return dir ? std::string(dir) : std::string();
#endif
}

// ----- child processes -----
// A started program whose stdout is a pipe we read.
struct Child {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE out = nullptr;
#else
    pid_t pid = -1;
    int out = -1;
#endif
};

#ifdef _WIN32
// One argument quoted for CreateProcess's command line (the MSVC CRT
// parsing rules: backslashes only matter before a quote).
inline std::string win_quote_arg(const std::string& a) {
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
inline std::string win_command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) { if (i) cmd += ' '; cmd += win_quote_arg(argv[i]); }
    return cmd;
}
#endif

// Start argv[0] (looked up on PATH) with stdout to a pipe and stderr
// discarded. False if it couldn't be started.
inline bool spawn_capture(const std::vector<std::string>& argv, Child& c) {
#ifdef _WIN32
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
    std::string cmd = win_command_line(argv);
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    CloseHandle(pi.hThread);
    c.process = pi.hProcess;
    c.out = rd;
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipefd[1]);
    c.pid = pid;
    c.out = pipefd[0];
    return true;
#endif
}

// Everything left on the child's stdout (blocks until it closes).
inline std::string read_all(Child& c) {
    std::string out;
    char buf[4096];
#ifdef _WIN32
    DWORD n = 0;
    while (c.out && ReadFile(c.out, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    if (c.out) { CloseHandle(c.out); c.out = nullptr; }
#else
    ssize_t n;
    while (c.out >= 0 && (n = read(c.out, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    if (c.out >= 0) { close(c.out); c.out = -1; }
#endif
    return out;
}

// True once the child has exited, with its exit code in *code (-1: it
// died some other way). Doesn't wait.
inline bool try_wait(Child& c, int* code) {
#ifdef _WIN32
    if (WaitForSingleObject(c.process, 0) != WAIT_OBJECT_0) return false;
    DWORD ec = 0;
    GetExitCodeProcess(c.process, &ec);
    CloseHandle(c.process);
    c.process = nullptr;
    *code = (int)ec;
    return true;
#else
    int status = 0;
    if (waitpid(c.pid, &status, WNOHANG) == 0) return false;
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
#endif
}

// Wait for the child; its exit code (-1: it died some other way).
inline int wait_exit(Child& c) {
#ifdef _WIN32
    WaitForSingleObject(c.process, INFINITE);
    int code = -1;
    try_wait(c, &code);
    return code;
#else
    int status = 0;
    if (waitpid(c.pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Start argv[0] in the background, detached from this process (its own
// session / no console), output discarded, at lower priority. Doesn't
// wait for it and leaves no zombie.
inline void spawn_detached(const std::vector<std::string>& argv) {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = win_command_line(argv);
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() != 0) _exit(0);      // the grandchild runs it; the child exits
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
        if (nice(10) == -1) { /* best effort */ }
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(args[0], args.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);               // the child exits at once
#endif
}

// Run fn(argc, argv) on a thread with a `stack`-byte stack and return
// its result; runs it right here if the thread can't be made.
inline int run_on_big_stack(int (*fn)(int, char**), int argc, char** argv, size_t stack) {
    struct Args { int (*fn)(int, char**); int argc; char** argv; int result; };
    Args a{fn, argc, argv, 0};
#ifdef _WIN32
    HANDLE t = CreateThread(nullptr, stack,
        [](LPVOID p) -> DWORD { auto* x = static_cast<Args*>(p); x->result = x->fn(x->argc, x->argv); return 0; },
        &a, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return fn(argc, argv);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return a.result;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t worker;
    int rc = pthread_create(&worker, &attr,
        [](void* p) -> void* { auto* x = static_cast<Args*>(p); x->result = x->fn(x->argc, x->argv); return nullptr; },
        &a);
    pthread_attr_destroy(&attr);
    if (rc != 0) return fn(argc, argv);
    pthread_join(worker, nullptr);
    return a.result;
#endif
}

}  // namespace qplat
