/* What differs between Windows and the POSIX systems, in one place
 * (src/plat/plat.c): the rest of prism calls these instead. Paths are
 * UTF-8 everywhere (the Windows build's manifest makes that the process's
 * code page, see cli/win/prism.manifest). */
#ifndef PRISM_PLAT_H
#define PRISM_PLAT_H

#include <stddef.h>
#include <stdint.h>

int plat_ncpu(void);                  /* logical processors, at least 1 */
long plat_mem_available_mb(void);     /* memory free for use, or -1 */
const char *plat_self(void);          /* this program's own path */
long long plat_self_mtime(void);      /* when it was built (its file's mtime), or 0 */
int plat_mkdir(const char *path);     /* 0 when it is there after (made or already) */
int plat_rename(const char *from, const char *to); /* replaces a file at to; 0 when done */
void plat_rmtree(const char *path);   /* a file or a folder and all in it, links not followed */
void plat_setenv(const char *name, const char *value);
int plat_is_dir(const char *path);
void plat_sleep_ms(int ms);
void plat_console_utf8(void);         /* a Windows console shows UTF-8 output as such */
void plat_open(const char *what);     /* a URL in the browser, a folder in the file manager */

/* A run of this program with other arguments (argv[0] is ignored: it is
 * plat_self()), its output (stdout and stderr) appended to log, or thrown
 * away when log is NULL. It and the processes it starts form a group
 * plat_proc_kill ends together. */
typedef struct { intptr_t pid, job; } plat_proc_t;
int plat_proc_start(plat_proc_t *p, const char *const *argv, const char *log);
int plat_proc_poll(plat_proc_t *p, int *code);  /* 1 and its exit code once it has ended */
int plat_proc_wait(plat_proc_t *p);             /* its exit code, or -1 */
void plat_proc_kill(plat_proc_t *p);

#endif
