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
#undef ERROR
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
#endif
