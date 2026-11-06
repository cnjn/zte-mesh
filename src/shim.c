/* Factory ARM EABI soft-float boundary probe. No guessed database backend. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <unistd.h>

#if !defined(__arm__) && !defined(ZTE_MESH_HOST_TEST)
#error "The vendor preload must be compiled for ARM32, not the OpenWrt host ABI"
#endif

#define EXPORT __attribute__((visibility("default")))
#define PROBE_STOP 78
#define BINDING_FAILURE 79

static int trace_mode;
static int emulated_lab;
static unsigned int records;
static __thread int logging;

/* Log API metadata only: no instance/key, returned buffers, PSKs or pointers. */
static void log_record(const char *format, ...)
{
    int saved_errno = errno;
    char buffer[384];
    va_list args;
    int length;
    if (logging)
        return;
    if (__sync_fetch_and_add(&records, 1) >= 128) {
        errno = saved_errno;
        return;
    }
    logging = 1;
    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        size_t total = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1;
        size_t offset = 0;
        while (offset < total) {
            ssize_t count = write(STDERR_FILENO, buffer + offset, total - offset);
            if (count > 0)
                offset += (size_t)count;
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
    }
    logging = 0;
    errno = saved_errno;
}

__attribute__((constructor)) static void shim_loaded(void)
{
    const char *mode = getenv("ZTE_MESH_SHIM_MODE");
    const char *allow = getenv("ZTE_MESH_SHIM_ALLOW_TRACE");
    const char *emulated = getenv("ZTE_MESH_EMULATED_LAB");
    emulated_lab = emulated && !strcmp(emulated, "1");
    if (!mode || !strcmp(mode, "probe")) {
        log_record("ZTE_MESH_SHIM loaded mode=probe\n");
        return;
    }
    if (!strcmp(mode, "trace") && allow && !strcmp(allow, "1")) {
        trace_mode = 1;
        log_record("ZTE_MESH_SHIM loaded mode=trace\n");
        return;
    }
    log_record("ZTE_MESH_SHIM stop reason=unsupported-or-unapproved-mode exit=78\n");
    _exit(PROBE_STOP);
}

static void *next_symbol(const char *name)
{
    int saved_errno = errno;
    void *function = dlsym(RTLD_NEXT, name);
    if (!function) {
        log_record("ZTE_MESH_SHIM stop reason=missing-real-symbol symbol=%s exit=79\n", name);
        _exit(BINDING_FAILURE);
    }
    errno = saved_errno;
    return function;
}

/* qemu-user cannot honour a fixed-address shmat(): the attach is emulated as an
 * mmap() over the reservation that holds the guest address space, and that mmap
 * fails with EINVAL unless SHM_REMAP lets it replace the placeholder mapping
 * (measured on qemu 6.2 with tools/lab/shm-fixed.c: every fixed address fails,
 * the same address with SHM_REMAP succeeds). The factory database engine attaches
 * its segment at a fixed address and dereferences the result, so an emulated lab
 * run needs the flag. A device run must keep the exact semantics the daemon
 * expects, which is why this is gated on the lab marker and not unconditional.
 */
EXPORT void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    void *(*real_attach)(int, const void *, int) = (void *(*)(int, const void *, int))next_symbol("shmat");
    void *result = real_attach(shmid, shmaddr, shmflg);
    if (result == (void *)-1 && shmaddr && emulated_lab && errno == EINVAL) {
        result = real_attach(shmid, shmaddr, shmflg | SHM_REMAP);
        if (result != (void *)-1)
            log_record("ZTE_MESH_SHIM emulated=shmat-fixed-address-retry\n");
    }
    return result;
}

/* qemu-user performs the guest's execve() as a HOST execve() of the same path,
 * which only lands back in the emulator when binfmt_misc is registered. A lab
 * host without it (ImmortalWrt x86_64 has no CONFIG_BINFMT_MISC at all) gets
 * ENOEXEC there, and the controller's daemon spawn dies on it (exit 82). Start
 * the emulator directly instead, handing it the guest loader as its program.
 * Opt-in through the two variables the launcher already sets, so a device run
 * keeps exactly the exec chain the daemon expects.
 */
static int emulated_exec(const char *path, char *const argv[])
{
    int (*real_execv)(const char *, char *const *) =
        (int (*)(const char *, char *const *))next_symbol("execv");
    const char *emulator = emulated_lab ? getenv("ZTE_MESH_QEMU") : NULL;
    if (emulator && *emulator && path && !strcmp(path, "/lib/ld-linux.so.3")) {
        char *qemu_argv[32];
        int i = 0, j;
        qemu_argv[i++] = (char *)emulator;
        qemu_argv[i++] = (char *)path;
        for (j = 1; argv[j] && i < 31; ++j)
            qemu_argv[i++] = argv[j];
        qemu_argv[i] = NULL;
        log_record("ZTE_MESH_SHIM emulated=exec-through-emulator\n");
        return real_execv(emulator, qemu_argv);
    }
    return real_execv(path, argv);
}

EXPORT int execl(const char *path, const char *arg, ...)
{
    char *argv[32];
    va_list args;
    int n = 0;
    argv[n++] = (char *)arg;
    va_start(args, arg);
    while (n < 31) {
        argv[n] = va_arg(args, char *);
        if (!argv[n])
            break;
        ++n;
    }
    va_end(args);
    if (n >= 31) {
        errno = E2BIG;
        return -1;
    }
    argv[n] = NULL;
    return emulated_exec(path, argv);
}

EXPORT int execv(const char *path, char *const argv[])
{
    return emulated_exec(path, argv);
}

EXPORT int DBShmCliInit(void)
{
    int (*real_init)(void);
    int result;
    log_record("ZTE_MESH_SHIM call=DBShmCliInit\n");
    if (!trace_mode) {
        /* main ignores the original return value; returning an error is unsafe. */
        log_record("ZTE_MESH_SHIM stop reason=database-backend-not-implemented exit=78\n");
        _exit(PROBE_STOP);
    }
    real_init = (int (*)(void))next_symbol("DBShmCliInit");
    result = real_init();
    /* The factory implementation can return a shared-memory address.
     * Preserve it for ABI compatibility, but never log it as a status code.
     */
    log_record("ZTE_MESH_SHIM return=DBShmCliInit returned=1\n");
    return result;
}

EXPORT int dbAPIGetView(const char *view, const char *instance, void *data)
{
    int (*real_get)(const char *, const char *, void *);
    int result;
    log_record("ZTE_MESH_SHIM call=dbAPIGetView view=%.64s\n", view ? view : "(null)");
    if (!trace_mode) {
        log_record("ZTE_MESH_SHIM stop reason=unexpected-database-read-in-probe exit=78\n");
        _exit(PROBE_STOP);
    }
    real_get = (int (*)(const char *, const char *, void *))next_symbol("dbAPIGetView");
    result = real_get(view, instance, data);
    log_record("ZTE_MESH_SHIM return=dbAPIGetView status=%d\n", result);
    return result;
}
