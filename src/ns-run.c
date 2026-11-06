/* Isolated DB experiment launcher. Failure never falls back to the host. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern int controller_private_network(void);
extern int controller_private_network_service(int host_net);

#ifndef SERVICE_ROOT
#define SERVICE_ROOT "/usr/lib/zte-mesh/root" /* test builds may override with -DSERVICE_ROOT */
#endif
#define SERVICE_STATE "/run/zte-mesh"
static volatile sig_atomic_t stop_requested;
static int emulated_lab;
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void fail(const char *operation)
{
    fprintf(stderr, "ZTE_DB_SANDBOX error=%s errno=%d\n", operation, errno);
    _exit(80);
}

/* The factory rootfs keeps the usual absolute links: /var/tmp, /var/run and
 * /var/log all point at /tmp. mount(2) resolves the final component, so a tmpfs
 * "on <root>/var/tmp" would land on the HOST /tmp instead of inside the sandbox.
 * On a procd system /run lives there as well (/run -> /var/run -> /tmp/run), and
 * hiding it makes the exchange directory disappear. Inside the chroot that same
 * link already resolves to the tmpfs mounted at /tmp, so the link is left alone;
 * any other symlink is refused rather than followed onto a host path.
 */
static void private_tmpfs(const char *root, const char *relative, const char *options, const char *label)
{
    char path[4096], target[4096];
    struct stat info;
    ssize_t length;
    if ((size_t)snprintf(path, sizeof(path), "%s%s", root, relative) >= sizeof(path)) {
        errno = ENAMETOOLONG;
        fail(label);
    }
    if (lstat(path, &info))
        fail(label);
    if (S_ISLNK(info.st_mode)) {
        if ((length = readlink(path, target, sizeof(target) - 1)) < 0)
            fail(label);
        target[length] = '\0';
        if (!strcmp(target, "/tmp"))
            return;
        errno = ELOOP;
        fail(label);
    }
    if (mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, options))
        fail(label);
}

static void isolate(const char *root, char **command)
{
    struct stat old_ipc, old_net, old_mnt, ipc, net, mnt;
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    struct rlimit core = {0, 0}, memory = {128UL * 1024 * 1024, 128UL * 1024 * 1024};
    struct rlimit cpu = {5, 5};
    char path[4096], emulator[4096];
    int cap, host_net = -1, have_emulator = 0;
    int service = command[4] && !strcmp(command[4], "--controller-service");
    int agent = command[4] && !strcmp(command[4], "--agent-startup");
    int startup = service || agent || (command[4] && !strcmp(command[4], "--controller-startup"));
    /* The agent observation is bounded by wall time (55 s), not by a CPU budget: under qemu-user the emulator's own
     * CPU time is what the limit would cut. It keeps the lab wire (lab-peer stays inside the sandbox). */
    if (service || agent)
        cpu.rlim_cur = cpu.rlim_max = RLIM_INFINITY;
    if (setpgid(0, 0) < 0)
        fail("setpgid");
    if (service && (host_net = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC)) < 0)
        fail("host-network-reference");
    if (stat("/proc/self/ns/ipc", &old_ipc) || stat("/proc/self/ns/net", &old_net) || stat("/proc/self/ns/mnt", &old_mnt))
        fail("namespace-stat-before");
    if (unshare(CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWNS))
        fail("unshare-ipc-net-mount");
    if (stat("/proc/self/ns/ipc", &ipc) || stat("/proc/self/ns/net", &net) || stat("/proc/self/ns/mnt", &mnt))
        fail("namespace-stat-after");
    if (old_ipc.st_ino == ipc.st_ino || old_net.st_ino == net.st_ino || old_mnt.st_ino == mnt.st_ino) {
        errno = EPERM;
        fail("namespace-not-private");
    }
    if (service ? controller_private_network_service(host_net) : (startup && controller_private_network()))
        fail(service ? "private-service-network" : "private-lab-network");
    if (host_net >= 0)
        close(host_net);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        fail("private-mount-propagation");
    if (mount(root, root, NULL, MS_BIND, NULL) || mount(NULL, root, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL))
        fail("read-only-root");
    private_tmpfs(root, "/tmp", "size=8m,mode=1777", "private-tmp");
    /* The daemon's OSS sockets live in /var/tmp on the factory device (unix_bind /var/tmp/multiapd.base_asy), so the
     * private root needs that directory writable too - the same size-capped tmpfs, still inside the namespace. */
    private_tmpfs(root, "/var/tmp", "size=8m,mode=1777", "private-var-tmp");
    private_tmpfs(root, "/dev/shm", "size=1m,mode=1777", "private-dev-shm");
    if (service || agent) {
        struct stat source = {0}, target = {0};
        int stage = 0;
        /* The only writable host path: a root-owned private exchange directory
         * for the host-written config and the daemon's state report. The agent
         * observation needs it too: the daemon binds its OSS socket there. */
        if (snprintf(path, sizeof(path), "%s" SERVICE_STATE, root) >= (int)sizeof(path))
            stage = 1, errno = ENAMETOOLONG;
        else if (lstat(SERVICE_STATE, &source))
            stage = 1;
        else if (!S_ISDIR(source.st_mode) || source.st_uid != 0 || (source.st_mode & 022))
            stage = 2, errno = EINVAL;
        else if (stat(path, &target) || !S_ISDIR(target.st_mode))
            stage = 3;
        if (stage) {
            /* The label is what the tests and the docs grep for; the detail line
             * says which of the three conditions lost, because "EINVAL" alone
             * cannot distinguish a symlinked /run from a missing directory. */
            fprintf(stderr, "ZTE_DB_SANDBOX exchange-detail stage=%d host=%s mode=%03o uid=%u sandbox=%s errno=%d\n",
                    stage, SERVICE_STATE, (unsigned)(source.st_mode & 07777), (unsigned)source.st_uid, path, errno);
            errno = EINVAL;
            fail("service-exchange-directory");
        }
        if (mount(SERVICE_STATE, path, NULL, MS_BIND, NULL) ||
            mount(NULL, path, NULL, MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))
            fail("service-exchange-bind");
    }
    if (startup) {
        struct stat source, target;
        if (snprintf(path, sizeof(path), "%s/dev/urandom", root) >= (int)sizeof(path) ||
            stat("/dev/urandom", &source) || !S_ISCHR(source.st_mode) ||
            source.st_rdev != makedev(1, 9) || lstat(path, &target) ||
            !S_ISREG(target.st_mode) || target.st_size != 0) {
            errno = EINVAL;
            fail("readonly-random-source");
        }
        /* The one deliberately permitted character device: kernel entropy.
         * No wireless/switch/flash devices, /proc, or host /dev directory.
         */
        if (mount("/dev/urandom", path, NULL, MS_BIND, NULL) ||
            mount(NULL, path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NOEXEC, NULL))
            fail("readonly-random-bind");
    }
    if (chdir(root) || chroot(".") || chdir("/"))
        fail("chroot");
    /* A software-emulated run (qemu-user on x86_64, tools/lab/) cannot live inside
     * the 128 MB address-space limit: the emulator reserves the 32-bit guest address
     * space plus its own translation buffer, and a fixed-address shmat() needs the
     * preload to add SHM_REMAP. Opt-in, and only for the lab launcher — the device
     * never sets it, and it still requires root to reach this point.
     */
    emulated_lab = getenv("ZTE_MESH_EMULATED_LAB") != NULL;
    if (emulated_lab) {
        memory.rlim_cur = memory.rlim_max = RLIM_INFINITY;
        fprintf(stderr, "ZTE_DB_SANDBOX emulated_lab=1 address_space=unlimited\n");
    }
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu))
        fail("resource-limits");
    for (cap = 0; cap <= CAP_LAST_CAP; ++cap)
        if (cap != CAP_IPC_OWNER && (!startup || cap != CAP_NET_RAW) && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) && errno != EINVAL)
            fail("capability-bounding-set");
    caps[0].effective = (1U << CAP_IPC_OWNER) | (startup ? (1U << CAP_NET_RAW) : 0);
    caps[0].permitted = caps[0].effective;
    if (syscall(SYS_capset, &header, caps) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        fail("capabilities");
    /* Existing FDs must not provide an escape to a host directory/device,
     * including descriptors >=1024. COMPAT target supports close_range.
     */
    if (syscall(SYS_close_range, 3U, ~0U, 0U))
        fail("close-inherited-fds");
    {
        /* Captured before clearenv() wipes the inherited environment. Only this one
         * named pair survives into the sandbox, and only for the live service: a
         * public ModelName/Manufacturer opt-in, never a serial, never silent. */
        const char *manufacturer = service ? getenv("ZTE_MESH_REAL_MANUFACTURER") : NULL;
        const char *model_name = service ? getenv("ZTE_MESH_REAL_MODEL") : NULL;
        const char *market_sign = service ? getenv("ZTE_MESH_REAL_MARKET_SIGN") : NULL;
        /* A kernel without CONFIG_BINFMT_MISC cannot exec the 32-bit guest at all
         * (ENOEXEC), so on such a lab host the emulator is started explicitly. The
         * path is inside the private root, which is why it survives the chroot.
         * Opt-in through the same lab switch that lifts the address-space limit,
         * and captured here because clearenv() below wipes it. */
        const char *qemu = emulated_lab ? getenv("ZTE_MESH_QEMU") : NULL;
        char manufacturer_buf[64], model_buf[64], market_buf[64];
        if (qemu && strlen(qemu) < sizeof(emulator)) {
            strcpy(emulator, qemu);
            have_emulator = 1;
        }
        if (manufacturer && model_name && strlen(manufacturer) < sizeof(manufacturer_buf) &&
            strlen(model_name) < sizeof(model_buf)) {
            strcpy(manufacturer_buf, manufacturer);
            strcpy(model_buf, model_name);
        } else {
            manufacturer = model_name = NULL;
        }
        if (market_sign && strlen(market_sign) < sizeof(market_buf))
            strcpy(market_buf, market_sign);
        else
            market_sign = NULL;
        clearenv();
        if (setenv("ZTE_MESH_DB_SANDBOX", "1", 1) || setenv("LD_BIND_NOW", "1", 1) ||
            setenv("LD_PRELOAD", "/lib/libzte_mesh_shim.so", 1) ||
            setenv("ZTE_MESH_SHIM_MODE", "trace", 1) || setenv("ZTE_MESH_SHIM_ALLOW_TRACE", "1", 1))
            fail("environment");
        /* The preload needs the same marker: it adds SHM_REMAP to a fixed-address
         * shmat() that the emulator would otherwise reject. */
        if (emulated_lab && setenv("ZTE_MESH_EMULATED_LAB", "1", 1))
            fail("environment");
        /* The guest's own spawn of the daemon has to go through the emulator too;
         * the preload reads both variables. A device never sets either. */
        if (have_emulator && setenv("ZTE_MESH_QEMU", emulator, 1))
            fail("environment");
        if (manufacturer && (setenv("ZTE_MESH_REAL_MANUFACTURER", manufacturer_buf, 1) ||
                             setenv("ZTE_MESH_REAL_MODEL", model_buf, 1)))
            fail("environment");
        if (market_sign && setenv("ZTE_MESH_REAL_MARKET_SIGN", market_buf, 1))
            fail("environment");
    }
    fprintf(stderr, "ZTE_DB_SANDBOX private_ipc=1 private_net=1 readonly_root=1 caps=%s\n",
            service ? "IPC_OWNER,NET_RAW service_exchange=1 bridge_port=lab-peer" :
            startup ? "IPC_OWNER,NET_RAW private_lab_bridges=2 private_veth=1" : "IPC_OWNER");
    if (have_emulator) {
        /* argv[0] is the emulator, the guest loader and its arguments follow
         * unchanged: qemu-user then runs /lib/ld-linux.so.3 inside this root. */
        char *emulated[16];
        int i;
        emulated[0] = emulator;
        for (i = 0; i < 14 && command[i]; ++i)
            emulated[i + 1] = command[i];
        emulated[i + 1] = NULL;
        fprintf(stderr, "ZTE_DB_SANDBOX emulator=%s\n", emulator);
        execv(emulator, emulated);
        fail("execv-emulator");
    }
    execv(command[0], command);
    fail("execv");
}

int main(int argc, char **argv)
{
    struct timespec started, now, pause_time = {0, 20000000};
    char root[4096];
    int status, timeout_seconds;
    siginfo_t information;
    pid_t child, waited;
    int service = argc == 7 && !strcmp(argv[6], "--controller-service");
    int agent = argc == 7 && !strcmp(argv[6], "--agent-startup");
    /* The agent observation runs against the same tmpfs root the service does. */
    int service_root = service || agent;
    if ((argc != 6 && argc != 7) || !realpath(argv[1], root) ||
        (service_root ? strcmp(argv[1], SERVICE_ROOT) || strcmp(root, SERVICE_ROOT)
                 : strncmp(argv[1], "/tmp/zte-mesh-db.", 17) || strncmp(root, "/tmp/zte-mesh-db.", 17))) {
        fprintf(stderr, "usage: ns-run /tmp/zte-mesh-db.XXXXXX/root /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe [--bridge-test|--bridge-rollback-test|--controller-startup]\n"
                        "       ns-run " SERVICE_ROOT " /lib/ld-linux.so.3 --library-path /lib /bin/db-engine-probe --controller-service|--agent-startup\n");
        return 64;
    }
    if (strcmp(argv[2], "/lib/ld-linux.so.3") || strcmp(argv[3], "--library-path") ||
        strcmp(argv[4], "/lib") || strcmp(argv[5], "/bin/db-engine-probe")) {
        fprintf(stderr, "ZTE_DB_SANDBOX refuse=unexpected-executable\n");
        return 64;
    }
    if (argc == 7 && strcmp(argv[6], "--controller-startup") && !service_root) {
        struct stat input, output;
        if ((strcmp(argv[6], "--bridge-test") && strcmp(argv[6], "--bridge-rollback-test")) ||
            fstat(STDIN_FILENO, &input) || fstat(STDOUT_FILENO, &output) ||
            !S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode)) {
            fprintf(stderr, "ZTE_DB_SANDBOX refuse=bridge-requires-fixed-mode-and-pipes\n");
            return 64;
        }
    }
    timeout_seconds = service ? 0 : agent ? 75 : argc == 7 && !strcmp(argv[6], "--controller-startup") ? 50 : 15;
    if (service) {
        struct sigaction action = {0};
        action.sa_handler = request_stop;
        if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) || sigaction(SIGHUP, &action, NULL))
            fail("signal-handlers");
    }
    clock_gettime(CLOCK_MONOTONIC, &started);
    child = fork();
    if (child < 0)
        fail("fork");
    if (!child)
        isolate(root, argv + 2);
    (void)setpgid(child, child);
    for (;;) {
        memset(&information, 0, sizeof(information));
        if (waitid(P_PID, child, &information, WEXITED | WNOHANG | WNOWAIT)) {
            if (errno == EINTR)
                continue;
            fail("waitid");
        }
        if (information.si_pid == child)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (stop_requested) {
            (void)kill(-child, SIGKILL);
            (void)waitpid(child, &status, 0);
            fprintf(stderr, "ZTE_DB_SANDBOX stop=requested\n");
            return 0;
        }
        if (timeout_seconds && now.tv_sec - started.tv_sec >= timeout_seconds) {
            (void)kill(-child, SIGKILL);
            (void)waitpid(child, &status, 0);
            fprintf(stderr, "ZTE_DB_SANDBOX stop=timeout\n");
            return 124;
        }
        nanosleep(&pause_time, NULL);
    }
    /* Hold the zombie PID until group cleanup: no PID/PGID reuse race. */
    (void)kill(-child, SIGKILL);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child)
        fail("waitpid");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
