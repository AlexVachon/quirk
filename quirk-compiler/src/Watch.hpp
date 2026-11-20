// `quirk run --watch <file>`: run the program, and run it again each time
// one of the project's files changes.
//
// A supervisor: the program runs as a child (the same command without
// --watch); this process polls the project's files (.qk .quirk .html .css
// .js .toml under the folder with quirk.toml, else the file's folder) for a
// new modification time or size. On a change it type-checks the entry file
// first (`--check`): when that fails the errors are shown and the running
// version keeps running; otherwise the child is stopped and started again.
// A live app's open tabs reload by themselves (their event stream
// reconnects to a server that doesn't know them, which answers "reload").
//
// Polling, not inotify / FSEvents / ReadDirectoryChangesW: the same code on
// every platform, and a few hundred files cost nothing every 300 ms.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "platform.hpp"

namespace qwatch {

namespace fs = std::filesystem;

static volatile std::sig_atomic_t g_stop = 0;

// "[watch] …" on stderr: coloured on a terminal, unless $NO_COLOR.
static void say(const std::string& msg) {
    static const bool color = ::isatty(STDERR_FILENO) && !std::getenv("NO_COLOR");
    std::cerr << (color ? "\033[36m[watch]\033[0m " : "[watch] ") << msg << std::endl;
}

// ---- the child -------------------------------------------------------------

#ifdef _WIN32
// <windows.h> stays in platform_win32.cpp: spawn_job / kill_job there.
using Proc = qplat::Child;

static void on_ctrl() { g_stop = 1; }       // the child gets the Ctrl+C too (same console)

static bool start(const std::vector<std::string>& argv, Proc& p, bool quiet_out = false) {
    return qplat::spawn_job(argv, p, quiet_out);
}

static bool exited(Proc& p, int* code) {
    return p.process && qplat::try_wait(p, code);
}

static void stop(Proc& p) {
    qplat::kill_job(p);
}

static int run_wait(const std::vector<std::string>& argv) {
    Proc p;
    if (!start(argv, p, true)) return -1;
    int code = qplat::wait_exit(p);
    qplat::kill_job(p);                      // (already done: frees the job)
    return code;
}
#else
struct Proc {
    pid_t pid = -1;
};

static void on_signal(int) { g_stop = 1; }

// quiet_out: its stdout goes nowhere (the check's "main.qk: OK").
static bool start(const std::vector<std::string>& argv, Proc& p, bool quiet_out = false) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (quiet_out) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
        }
        // Same process group as the terminal's: it can read the keyboard,
        // and Ctrl+C reaches it as it would without --watch.
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(args[0], args.data());
        _exit(127);
    }
    p.pid = pid;
    return true;
}

static bool exited(Proc& p, int* code) {
    if (p.pid <= 0) return false;
    int status = 0;
    if (waitpid(p.pid, &status, WNOHANG) != p.pid) return false;
    *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    p.pid = -1;
    return true;
}

// SIGTERM, then SIGKILL if it's still there after 3 s. (The program runs
// in this pid: the JIT in-process, a cached binary through exec.)
static void stop(Proc& p) {
    if (p.pid <= 0) return;
    kill(p.pid, SIGTERM);
    for (int i = 0; i < 60; i++) {
        int status = 0;
        if (waitpid(p.pid, &status, WNOHANG) == p.pid) { p.pid = -1; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(p.pid, SIGKILL);
    int status = 0;
    waitpid(p.pid, &status, 0);
    p.pid = -1;
}

static int run_wait(const std::vector<std::string>& argv) {
    Proc p;
    if (!start(argv, p, true)) return -1;
    int status = 0;
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

// ---- the files -------------------------------------------------------------

using Snap = std::map<std::string, std::pair<long long, uintmax_t>>;

static bool watched_file(const fs::path& p) {
    std::string e = p.extension().string();
    return e == ".qk" || e == ".quirk" || e == ".html" || e == ".css" || e == ".js" || e == ".toml";
}

// Not looked into: hidden folders (.git, .venv), installed packages,
// node_modules, build output.
static bool skipped_dir(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "node_modules" || name == "packages" ||
           name == "build" || name == "target" || name == "__pycache__";
}

static Snap snapshot(const fs::path& root) {
    Snap s;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        std::error_code ec2;
        if (e.is_directory(ec2)) {
            if (skipped_dir(e.path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!watched_file(e.path())) continue;
        auto t = fs::last_write_time(e.path(), ec2);
        if (ec2) continue;
        uintmax_t size = fs::file_size(e.path(), ec2);
        if (ec2) size = 0;
        s[e.path().string()] = {(long long)t.time_since_epoch().count(), size};
    }
    return s;
}

// A file that changed, appeared or went away between a and b (relative to root).
static std::string what_changed(const Snap& a, const Snap& b, const fs::path& root) {
    auto rel = [&](const std::string& p) {
        std::error_code ec;
        fs::path r = fs::relative(p, root, ec);
        return ec ? p : r.generic_string();
    };
    for (auto& [path, st] : b) {
        auto it = a.find(path);
        if (it == a.end()) return rel(path) + " (new)";
        if (it->second != st) return rel(path);
    }
    for (auto& [path, st] : a)
        if (!b.count(path)) return rel(path) + " (removed)";
    return "a file";
}

// The project's folder: the nearest one above `file` with a quirk.toml,
// else the file's own.
static fs::path project_root(const std::string& file) {
    std::error_code ec;
    fs::path abs = fs::absolute(file, ec);
    fs::path dir = fs::is_directory(abs, ec) ? abs : abs.parent_path();
    fs::path p = dir;
    for (int i = 0; i < 12 && !p.empty(); i++) {
        if (fs::exists(p / "quirk.toml", ec)) return p;
        if (p == p.root_path()) break;
        p = p.parent_path();
    }
    return dir;
}

// ---- the loop --------------------------------------------------------------

// `self`: this quirk binary. `args`: the command without --watch (args[0]
// is replaced by `self`). `entry`: the file being run (or, for `quirk test
// --watch`, the file / folder of tests). `check`: type-check `entry` before
// running again, and keep the running version when it fails (a server);
// `quirk test` shows its own compile errors — and has nothing to keep.
static int run(const std::string& self, std::vector<std::string> args, const std::string& entry, bool check = true) {
    args[0] = self;
#ifdef _WIN32
    qplat::on_ctrl_c(on_ctrl);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
    fs::path root = project_root(entry);
    Snap snap = snapshot(root);
    say("watching " + root.string() + " (" + std::to_string(snap.size()) + " files) — Ctrl+C to stop");

    Proc child;
    bool running = start(args, child);
    if (!running) { say("couldn't start " + self); return 1; }

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (g_stop) break;
        int code = 0;
        if (running && exited(child, &code)) {
            running = false;
            stop(child);                       // (frees what's left: the job on Windows)
            say(entry + (code == 0 ? std::string(" finished") : " exited with code " + std::to_string(code)) +
                " — waiting for a change");
        }
        Snap now = snapshot(root);
        if (now == snap) continue;
        // An editor saving several files (or writing one in steps): wait
        // until nothing has moved for a moment.
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            Snap again = snapshot(root);
            if (again == now) break;
            now = again;
        }
        std::string what = what_changed(snap, now, root);
        snap = now;
        int rc = check ? run_wait({self, "--check", entry}) : 0;
        if (g_stop) break;
        if (rc != 0) {
            say(what + " changed, but it doesn't compile" +
                (running ? std::string(" — still running the last version") : std::string("")));
            continue;
        }
        if (running) stop(child);
        say(what + (check ? " changed — restarting" : " changed — running again"));
        running = start(args, child);
        if (!running) say("couldn't start " + self);
    }
    if (running) stop(child);
    std::cerr << std::endl;
    say("stopped");
    return 0;
}

}  // namespace qwatch
