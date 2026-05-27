#include "CppUnsafeCatalog.h"

#include <unordered_set>

namespace topo::check {

UnsafeLevel CppUnsafeCatalog::classifyCall(const std::string& pattern) {
    // Level 4: Language escape mechanisms
    static const std::unordered_set<std::string> escape = {
        "reinterpret_cast", "const_cast",
        "setjmp", "longjmp", "_setjmp", "_longjmp",
        "c-style-pointer-cast", "inline-asm", "volatile", "goto",
        "union-in-function", "placement-new",
        "__attribute__", "__declspec", "pragma-link",
        "addr-of-dangerous", "std::launder", "std::bit_cast",
        // C++20 manual lifetime construction — bypass normal constructor/destructor flow
        "construct_at", "destroy_at",
        "start_lifetime_as", "start_lifetime_as_array",
    };
    if (escape.count(pattern)) return UnsafeLevel::Escape;

    // Level 4: raw pointer operations and asm are detected at syntax level
    // by the extractor, not through call classification.

    // Level 1: Standard library system calls
    static const std::unordered_set<std::string> systemCalls = {
        // File I/O
        "fopen", "fclose", "fread", "fwrite", "freopen", "tmpfile",
        "open", "close", "read", "write", "lseek", "mmap", "munmap",
        "pread", "pwrite", "mkstemp", "sendfile", "aio_read", "aio_write",
        "dup", "dup2", "unlink", "remove", "rename", "chmod", "truncate",
        // Network
        "socket", "connect", "bind", "listen", "accept", "send", "recv",
        "sendto", "recvfrom", "sendmsg", "recvmsg", "getaddrinfo",
        "gethostbyname", "epoll_create", "epoll_ctl", "epoll_wait",
        "poll", "select", "socketpair", "pipe", "mkfifo",
        // Process / Thread
        "system", "fork", "popen", "posix_spawn", "execv", "execve", "execvp",
        "execl", "execle", "execlp", "waitpid", "wait", "kill",
        "signal", "ptrace", "CreateProcess", "_spawnl",
        "pthread_create", "thrd_create",
        // Process termination
        "exit", "_exit", "_Exit", "abort", "raise",
        "quick_exit", "atexit", "at_quick_exit",
        // Process control
        "clone", "vfork", "daemon", "setsid", "prctl", "sigaction",
        // Dynamic loading
        "dlopen", "dlsym", "dlclose", "LoadLibrary", "GetProcAddress",
        // Memory protection
        "mprotect", "VirtualAlloc", "VirtualProtect",
        // Shared memory
        "shmget", "shmat", "shmdt", "shmctl", "shm_open", "shm_unlink",
        // Permissions
        "setuid", "setgid", "seteuid", "setegid",
        "chown", "fchown", "chroot",
        // Filesystem
        "access", "chdir", "creat", "fchmod", "fstat", "ftruncate",
        "link", "lstat", "mkdir", "openat", "opendir",
        "readlink", "rmdir", "stat", "symlink",
        // Stdout / formatted output
        "printf", "fprintf", "puts", "fputs", "sprintf", "snprintf",
        "vprintf", "vfprintf", "vsnprintf",
        "putchar", "putc", "fputc",
        "perror",
        // Device/IO
        "ioctl", "fcntl",
        // Environment
        "getenv", "setenv", "putenv", "unsetenv",
        // Memory — manual allocation (heap escape from RAII)
        "malloc", "calloc", "realloc", "reallocarray", "free",
        "aligned_alloc", "posix_memalign", "valloc", "pvalloc", "memalign",
        // Memory — untyped buffer operations (type-punning / overflow prone)
        "memcpy", "memmove", "memset", "memchr", "memcmp",
        "bcopy", "bzero", "bcmp",
        // Memory — unbounded string operations (buffer overflow classics)
        "strcpy", "strncpy", "strcat", "strncat", "strdup", "strndup",
    };
    if (systemCalls.count(pattern)) return UnsafeLevel::System;

    return UnsafeLevel::Safe;
}

UnsafeLevel CppUnsafeCatalog::classifyImport(const std::string& path) {
    // Level 1: System headers
    static const std::unordered_set<std::string> systemImports = {
        // File I/O
        "fstream", "cstdio", "filesystem",
        // Stream I/O
        "iostream", "ostream", "istream", "sstream",
        // C I/O (stdio.h covered below)
        "print",
        "fcntl.h", "sys/stat.h", "stdio.h", "unistd.h", "io.h",
        "sys/mman.h", "sys/sendfile.h", "aio.h",
        // Network
        "sys/socket.h", "netinet/in.h", "netinet/tcp.h",
        "arpa/inet.h", "netdb.h", "winsock2.h", "ws2tcpip.h",
        "sys/epoll.h", "poll.h", "net/if.h",
        // Process / Thread
        "cstdlib", "spawn.h", "sys/wait.h", "windows.h",
        "process.h", "sys/ptrace.h", "signal.h",
        "thread", "pthread.h", "csetjmp",
        // Dynamic loading
        "dlfcn.h", "ltdl.h", "libloaderapi.h",
        // IPC/Shared memory
        "sys/shm.h", "sys/ipc.h", "sys/sem.h", "sys/msg.h",
        // System control
        "sys/ioctl.h", "sys/prctl.h", "sys/resource.h", "sys/uio.h",
        // Directory/users
        "dirent.h", "pwd.h", "grp.h",
        // Semaphore/MQ
        "semaphore.h", "mqueue.h",
        // Memory / C string operations
        "cstring", "string.h", "memory.h", "strings.h",
        // Note: <memory> deliberately omitted — mostly smart pointers (safe),
        // construct_at / destroy_at are flagged at call-site level instead.
        // Other
        "stdlib.h", "csignal", "future",
    };
    if (systemImports.count(path)) return UnsafeLevel::System;

    // Level 2: Third-party (exact match)
    static const std::unordered_set<std::string> thirdPartyExact = {
        "sqlite3.h", "uv.h", "ev.h", "pq-fe.h", "libpq-fe.h",
    };
    if (thirdPartyExact.count(path)) return UnsafeLevel::Dep;

    // Level 2: Third-party (prefix-based)
    if (path.size() >= 4 && path.substr(0, 4) == "aws/") return UnsafeLevel::Dep;
    if (path.size() >= 5 && path.substr(0, 5) == "bson/") return UnsafeLevel::Dep;
    if (path.size() >= 6 && path.substr(0, 6) == "boost/") return UnsafeLevel::Dep;
    if (path.size() >= 5 && path.substr(0, 5) == "curl/") return UnsafeLevel::Dep;
    if (path.size() >= 4 && path.substr(0, 4) == "fmt/") return UnsafeLevel::Dep;
    if (path.size() >= 5 && path.substr(0, 5) == "grpc/") return UnsafeLevel::Dep;
    if (path.size() >= 7 && path.substr(0, 7) == "grpcpp/") return UnsafeLevel::Dep;
    if (path.size() >= 9 && path.substr(0, 9) == "hiredis/") return UnsafeLevel::Dep;
    if (path.size() >= 7 && path.substr(0, 7) == "mongoc/") return UnsafeLevel::Dep;
    if (path.size() >= 6 && path.substr(0, 6) == "mysql/") return UnsafeLevel::Dep;
    if (path.size() >= 8 && path.substr(0, 8) == "openssl/") return UnsafeLevel::Dep;
    if (path.size() >= 7 && path.substr(0, 7) == "spdlog/") return UnsafeLevel::Dep;
    if (path.size() >= 4 && path.substr(0, 4) == "zmq/") return UnsafeLevel::Dep;

    return UnsafeLevel::Safe;
}

} // namespace topo::check
