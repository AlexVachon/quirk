// OS layer for the compiler driver (Compiler.cpp, PackageManager.hpp).
//
// POSIX: the headers the driver always used, and the qplat:: helpers
// defined inline below. Windows (native port, docs/WINDOWS-PORT.md): CRT
// headers and one-to-one shims only — no <windows.h>, whose macros and
// names (ERROR, CONST, TRUE, even an enum constant `TokenType`) collide
// with the compiler's own. The Windows qplat:: functions are implemented
// in platform_win32.cpp, the one file that includes <windows.h>.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#else
#include <io.h>
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdint>

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
// system() returns the exit code itself on Windows.
#define WIFEXITED(s)   (1)
#define WEXITSTATUS(s) (s)
#endif

namespace qplat {

// The user's home directory: $HOME, or on Windows %USERPROFILE% when HOME
// isn't set (cmd.exe / PowerShell; Git Bash sets HOME). nullptr if neither.
inline const char* home_env() {
    const char* h = std::getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = std::getenv("USERPROFILE");
#endif
    return h;
}

// The tar to extract .tar.gz with. On Windows, the bsdtar Windows 10+
// ships in System32: a GNU tar found first on PATH (Git Bash, MSYS2)
// reads "C:\\..." as a remote host:path. --force-local covers the rest.
inline std::string tar_cmd() {
#ifdef _WIN32
    if (const char* root = std::getenv("SystemRoot")) {
        std::string t = std::string(root) + "\\System32\\tar.exe";
        std::error_code ec;
        if (std::filesystem::exists(t, ec)) return "\"" + t + "\"";
    }
    return "tar --force-local";
#else
    return "tar";
#endif
}

// std::system, quoted the way cmd.exe needs on Windows: it strips the
// outer quotes of a command that starts with a quoted program path
// ("The filename, directory name, or volume label syntax is incorrect"),
// so the whole command gets one more pair.
// Commands are written for sh; on Windows, cmd.exe spells /dev/null NUL.
inline std::string shell_fixup(std::string cmd) {
#ifdef _WIN32
    for (size_t at; (at = cmd.find("/dev/null")) != std::string::npos; ) cmd.replace(at, 9, "NUL");
#endif
    return cmd;
}

inline int run_shell(const std::string& cmd) {
#ifdef _WIN32
    return std::system(("\"" + shell_fixup(cmd) + "\"").c_str());
#else
    return std::system(cmd.c_str());
#endif
}

// popen with the same fix-ups (and cmd.exe's outer quotes) as run_shell.
inline FILE* popen_cmd(const std::string& cmd, const char* mode) {
#ifdef _WIN32
    return _popen(("\"" + shell_fixup(cmd) + "\"").c_str(), mode);
#else
    return ::popen(cmd.c_str(), mode);
#endif
}

// True if `name` is a program on PATH.
inline bool have_command(const std::string& name) {
#ifdef _WIN32
    return run_shell("where \"" + name + "\" >NUL 2>&1") == 0;
#else
    return run_shell("command -v '" + name + "' >/dev/null 2>&1") == 0;
#endif
}
inline int run_shell(const char* cmd) { return run_shell(std::string(cmd ? cmd : "")); }

// The runtime library next to the quirk binary, and the Boehm GC
// library names the JIT loads before it.
#ifdef _WIN32
inline constexpr const char* RUNTIME_LIB = "runtime.dll";
inline constexpr const char* GC_LIBS[] = {"gc.dll", "libgc.dll"};
#elif defined(__APPLE__)
inline constexpr const char* RUNTIME_LIB = "runtime.so";
inline constexpr const char* GC_LIBS[] = {"libgc.1.dylib", "libgc.dylib", "libgc.so.1", "libgc.so"};
#else
inline constexpr const char* RUNTIME_LIB = "runtime.so";
inline constexpr const char* GC_LIBS[] = {"libgc.so.1", "libgc.so"};
#endif

// A started program whose stdout is a pipe we read.
struct Child {
#ifdef _WIN32
    void* process = nullptr;    // HANDLE
    void* out = nullptr;        // HANDLE
#else
    pid_t pid = -1;
    int out = -1;
#endif
};

}  // namespace qplat

#ifdef _WIN32
namespace qplat {
// Implemented in platform_win32.cpp (see the POSIX versions below for
// what each one does).
int stdout_cols(int fallback);
std::string make_temp_dir(const std::string& prefix);
std::string write_temp_file(const std::string& prefix, const std::string& suffix, const std::string& content);
bool spawn_capture(const std::vector<std::string>& argv, Child& c);
std::string read_all(Child& c);
bool try_wait(Child& c, int* code);
int wait_exit(Child& c);
void spawn_detached(const std::vector<std::string>& argv);
// Run `program` with argv (argv[0] is what the child sees as its name) and
// this process's stdin / stdout / stderr; its exit code, -1 if it couldn't start.
int spawn_wait(const std::string& program, const std::vector<std::string>& argv);
int run_on_big_stack(int (*fn)(int, char**), int argc, char** argv, size_t stack);
std::string self_exe();
std::string native_arch();
void sleep_ms(unsigned ms);
}  // namespace qplat
inline unsigned int sleep(unsigned int s) { qplat::sleep_ms(s * 1000); return 0; }
inline int usleep(unsigned int us) { qplat::sleep_ms(us / 1000); return 0; }
#else
namespace qplat {


// Terminal width of stdout in columns, or `fallback` when it isn't a
// terminal.
inline int stdout_cols(int fallback) {
    struct winsize ws;
    if (::isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return fallback;
}

// A fresh private directory `<tmp>/<prefix>XXXXXX`; "" on failure.
inline std::string make_temp_dir(const std::string& prefix) {
    std::string tmpl = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    return dir ? std::string(dir) : std::string();
}



// Start argv[0] (looked up on PATH) with stdout to a pipe and stderr
// discarded. False if it couldn't be started.
inline bool spawn_capture(const std::vector<std::string>& argv, Child& c) {
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
}

// Everything left on the child's stdout (blocks until it closes).
inline std::string read_all(Child& c) {
    std::string out;
    char buf[4096];
    ssize_t n;
    while (c.out >= 0 && (n = read(c.out, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    if (c.out >= 0) { close(c.out); c.out = -1; }
    return out;
}

// True once the child has exited, with its exit code in *code (-1: it
// died some other way). Doesn't wait.
inline bool try_wait(Child& c, int* code) {
    int status = 0;
    if (waitpid(c.pid, &status, WNOHANG) == 0) return false;
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

// Wait for the child; its exit code (-1: it died some other way).
inline int wait_exit(Child& c) {
    int status = 0;
    if (waitpid(c.pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Start argv[0] in the background, detached from this process (its own
// session / no console), output discarded, at lower priority. Doesn't
// wait for it and leaves no zombie.
inline void spawn_detached(const std::vector<std::string>& argv) {
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
}

// Run fn(argc, argv) on a thread with a `stack`-byte stack and return
// its result; runs it right here if the thread can't be made.
inline int run_on_big_stack(int (*fn)(int, char**), int argc, char** argv, size_t stack) {
    struct Args { int (*fn)(int, char**); int argc; char** argv; int result; };
    Args a{fn, argc, argv, 0};
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
}

// Write `content` to a new private file `<tmp>/<prefix>XXXXXX<suffix>`;
// its path, or "" on failure.
inline std::string write_temp_file(const std::string& prefix, const std::string& suffix, const std::string& content) {
    std::string tmpl = (std::filesystem::temp_directory_path() / (prefix + "XXXXXX" + suffix)).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), (int)suffix.size());
    if (fd < 0) return "";
    bool ok = ::write(fd, content.data(), content.size()) == (ssize_t)content.size();
    ::close(fd);
    if (!ok) { std::remove(buf.data()); return ""; }
    return std::string(buf.data());
}

inline void sleep_ms(unsigned ms) { usleep(ms * 1000); }

}  // namespace qplat
#endif
