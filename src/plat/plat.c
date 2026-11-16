/* The platform's side of prism: see plat.h. */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

int plat_ncpu(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (int)n : 1;
}

long plat_mem_available_mb(void) {
    MEMORYSTATUSEX m = { .dwLength = sizeof m };
    return GlobalMemoryStatusEx(&m) ? (long)(m.ullAvailPhys >> 20) : -1;
}

const char *plat_self(void) {
    static char path[MAX_PATH * 4];
    if (!path[0]) GetModuleFileNameA(NULL, path, sizeof path);
    return path;
}

long long plat_self_mtime(void) {
    struct stat st;
    return stat(plat_self(), &st) == 0 ? (long long)st.st_mtime : 0;
}

int plat_mkdir(const char *path) {
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

/* A file the viewer is reading can't be replaced for that moment: tried
 * again for a second. */
int plat_rename(const char *from, const char *to) {
    for (int i = 0; i < 20; i++) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return 0;
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) return -1;
        Sleep(50);
    }
    return -1;
}

void plat_rmtree(const char *path) {
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileA(path); return; }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) { /* a junction goes, not what it points to */
        char pat[MAX_PATH * 4];
        snprintf(pat, sizeof pat, "%s\\*", path);
        WIN32_FIND_DATAA f;
        HANDLE h = FindFirstFileA(pat, &f);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..")) continue;
                char sub[MAX_PATH * 4];
                snprintf(sub, sizeof sub, "%s\\%s", path, f.cFileName);
                plat_rmtree(sub);
            } while (FindNextFileA(h, &f));
            FindClose(h);
        }
    }
    RemoveDirectoryA(path);
}

void plat_setenv(const char *name, const char *value) { _putenv_s(name, value); }
int plat_is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
void plat_sleep_ms(int ms) { Sleep((DWORD)ms); }
void plat_console_utf8(void) { SetConsoleOutputCP(CP_UTF8); }
void plat_open(const char *what) { ShellExecuteA(NULL, "open", what, NULL, NULL, SW_SHOWNORMAL); }

/* argv as one command line, quoted the way the C runtime splits it again */
static void quote_arg(char *out, size_t len, size_t *n, const char *a) {
#define PUT(c) do { if (*n + 1 < len) out[(*n)++] = (c); } while (0)
    if (*n) PUT(' ');
    if (*a && !strpbrk(a, " \t\"")) { while (*a) PUT(*a++); return; }
    PUT('"');
    for (;; a++) {
        size_t bs = 0;
        while (*a == '\\') { a++; bs++; }
        if (!*a) { for (size_t i = 0; i < bs * 2; i++) PUT('\\'); break; }
        if (*a == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) PUT('\\'); }
        else for (size_t i = 0; i < bs; i++) PUT('\\');
        PUT(*a);
    }
    PUT('"');
#undef PUT
}

int plat_proc_start(plat_proc_t *p, const char *const *argv, const char *log) {
    static char cmd[32768];
    size_t n = 0;
    quote_arg(cmd, sizeof cmd, &n, plat_self());
    for (int i = 1; argv[i]; i++) quote_arg(cmd, sizeof cmd, &n, argv[i]);
    cmd[n] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out = CreateFileA(log ? log : "NUL", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return -1;
    /* only the log is inherited (not the server's sockets) */
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = malloc(asz);
    STARTUPINFOEXA si = { 0 };
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &asz) ||
        !UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, NULL, NULL)) {
        free(al);
        CloseHandle(out);
        return -1;
    }
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                             NULL, NULL, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(al);
    free(al);
    CloseHandle(out);
    if (!ok) return -1;
    /* a job: killing it ends what it started too, and so does this
       process's end (its handle closing) */
    HANDLE job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->pid = (intptr_t)pi.hProcess;
    p->job = (intptr_t)job;
    return 0;
}

int plat_proc_poll(plat_proc_t *p, int *code) {
    if (WaitForSingleObject((HANDLE)p->pid, 0) != WAIT_OBJECT_0) return 0;
    DWORD c = 1;
    GetExitCodeProcess((HANDLE)p->pid, &c);
    CloseHandle((HANDLE)p->pid);
    if (p->job) CloseHandle((HANDLE)p->job);
    p->pid = p->job = 0;
    *code = (int)c;
    return 1;
}

int plat_proc_wait(plat_proc_t *p) {
    WaitForSingleObject((HANDLE)p->pid, INFINITE);
    int c = -1;
    plat_proc_poll(p, &c);
    return c;
}

void plat_proc_kill(plat_proc_t *p) {
    if (p->job) TerminateJobObject((HANDLE)p->job, 1);
    else if (p->pid) TerminateProcess((HANDLE)p->pid, 1);
}

#else /* POSIX */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

int plat_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

long plat_mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

const char *plat_self(void) {
    static char path[4096];
    if (!path[0]) {
#ifdef __APPLE__
        uint32_t n = sizeof path;
        if (_NSGetExecutablePath(path, &n)) path[0] = 0;
#else
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        path[n > 0 ? n : 0] = 0;
#endif
    }
    return path;
}

long long plat_self_mtime(void) {
#ifdef __EMSCRIPTEN__ /* no file of its own: when it was linked (the Makefile writes it) */
    extern const long long prism_built;
    return prism_built;
#else
    struct stat st;
    return stat(plat_self(), &st) == 0 ? (long long)st.st_mtime : 0;
#endif
}

int plat_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }
int plat_rename(const char *from, const char *to) { return rename(from, to); }

void plat_rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        for (struct dirent *e; d && (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[4096];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            plat_rmtree(sub);
        }
        if (d) closedir(d);
        rmdir(path);
    } else remove(path);
}

void plat_setenv(const char *name, const char *value) { setenv(name, value, 1); }
int plat_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
void plat_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}
void plat_console_utf8(void) {}

void plat_open(const char *what) {
#ifdef __APPLE__
    const char *argv[] = { "open", what, NULL };
#else
    const char *argv[] = { "xdg-open", what, NULL };
#endif
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) == 0) waitpid(pid, NULL, WNOHANG);
    posix_spawn_file_actions_destroy(&fa);
}

int plat_proc_start(plat_proc_t *p, const char *const *argv, const char *log) {
    const char *self = plat_self();
    int n = 0;
    while (argv[n]) n++;
    const char **av = malloc((size_t)(n + 1) * sizeof *av);
    if (!av) return -1;
    av[0] = self;
    for (int i = 1; i <= n; i++) av[i] = argv[i];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log ? log : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP); /* its own group, killed together */
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, self, &fa, &at, (char *const *)av, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(av);
    if (rc) return -1;
    p->pid = pid;
    p->job = 0;
    return 0;
}

static int exit_code(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0); }

int plat_proc_poll(plat_proc_t *p, int *code) {
    int status;
    pid_t r = waitpid((pid_t)p->pid, &status, WNOHANG);
    if (r == 0) return 0;
    *code = r < 0 ? -1 : exit_code(status);
    p->pid = 0;
    return 1;
}

int plat_proc_wait(plat_proc_t *p) {
    int status;
    pid_t r;
    while ((r = waitpid((pid_t)p->pid, &status, 0)) < 0 && errno == EINTR) {}
    p->pid = 0;
    return r < 0 ? -1 : exit_code(status);
}

void plat_proc_kill(plat_proc_t *p) {
    if (p->pid > 0) kill(-(pid_t)p->pid, SIGTERM);
}
#endif
