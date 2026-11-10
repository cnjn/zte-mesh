/* Temporary real-daemon bootstrap diagnostics. Preserve all vendor results.
 * No CM/driver success stubs; no log varargs/profile bytes are printed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "controller-log.h"
#include "controller-peer.h"
#include "agent-ap-metrics.h"
#define EXPORT __attribute__((visibility("default")))

static struct log_gate gate;
static int (*format_real)(char *, size_t, const char *, va_list);
/* stderr in this sandbox can be a pipe that is already gone, and a dropped
 * diagnostic is not actionable anyway — but the result still has to be consumed
 * by something other than a cast: glibc marks write() __warn_unused_result and
 * GCC rejects `(void)write(...)` under -Werror=unused-result (11.4 does). Assign
 * to a variable and discard that instead, so the warning stays available for the
 * calls where the result does matter. */
static void emit(const char *data, size_t len)
{
    ssize_t written = write(STDERR_FILENO, data, len);
    (void)written;
}
/* One formatted line, through the budgets. controller-log.c decides how much of each
 * class reaches the device log, against CLOCK_MONOTONIC; a host test drives the same
 * rules with its own clock and its own gate. */
static void emit_line(const char *line, size_t len)
{
    struct timespec monotonic;
    const char *notice = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic))
        monotonic.tv_sec = 0;
    if (!controller_log_admit(&gate, monotonic.tv_sec, line, &notice)) {
        if (notice)
            emit(notice, strlen(notice));
        return;
    }
    emit(line, len);
}
/* The firehose switch: a flag file in the one directory the sandbox leaves writable. */
static int verbose_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = access("/run/zte-mesh/trace-all", F_OK) == 0;
    return cached;
}
static void message(const char *fmt, ...)
{
    char buffer[384];
    va_list ap;
    int n, saved = errno;
    if (!format_real) return;
    va_start(ap, fmt);
    n = format_real(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (n > 0)
        emit_line(buffer, (size_t)n < sizeof(buffer) ? (size_t)n : sizeof(buffer) - 1);
    errno = saved;
}
static void *symbol(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (!p) {
        message("ZTE_CONTROLLER_TRACE missing=%s\n", name);
        _exit(79);
    }
    return p;
}
__attribute__((constructor)) static void init(void)
{
    const char *isolated = getenv("ZTE_MESH_DB_SANDBOX");
    if (!isolated || strcmp(isolated, "1"))
        _exit(79);
    format_real = (int (*)(char *, size_t, const char *, va_list))dlsym(RTLD_NEXT, "vsnprintf");
    if (!format_real)
        _exit(79);
    message("ZTE_CONTROLLER_TRACE active real_calls=1 credentials_logged=0\n");
}
EXPORT int OSSInitConfig(void *config)
{
    int (*real)(void *) = symbol("OSSInitConfig");
    int result, saved;
    message("ZTE_CONTROLLER_TRACE call=OSSInitConfig\n");
    result = real(config); saved = errno;
    message("ZTE_CONTROLLER_TRACE return=OSSInitConfig status=%d errno=%d\n", result, saved);
    errno = saved; return result;
}
EXPORT int OSSStart(void *config, void *arg1, void *arg2)
{
    int (*real)(void *, void *, void *) = symbol("OSSStart");
    int result, saved;
    message("ZTE_CONTROLLER_TRACE call=OSSStart\n");
    result = real(config, arg1, arg2); saved = errno;
    message("ZTE_CONTROLLER_TRACE return=OSSStart status=%d errno=%d\n", result, saved);
    errno = saved; return result;
}
EXPORT int socket(int family, int type, int protocol)
{
    int (*real)(int, int, int) = symbol("socket");
    int result = real(family, type, protocol), saved = errno;
    message("ZTE_CONTROLLER_TRACE socket family=%d type=%d protocol=%d success=%d errno=%d\n",
            family, type, protocol, result >= 0, saved);
    errno = saved; return result;
}
EXPORT ssize_t sendto(int fd, const void *data, size_t length, int flags,
                      const struct sockaddr *destination, socklen_t destination_size)
{
    ssize_t (*real)(int, const void *, size_t, int, const struct sockaddr *, socklen_t) = symbol("sendto");
    ssize_t result = real(fd, data, length, flags, destination, destination_size);
    int saved = errno;
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    if (data && length >= 22 && !getsockname(fd, (struct sockaddr *)&address, &size) && address.ss_family == AF_PACKET) {
        const unsigned char *bytes = data;
        unsigned int ether = ((unsigned int)bytes[12] << 8) | bytes[13];
        if (ether == 0x893a)
            message("ZTE_CONTROLLER_TRACE tx1905 cmdu=%u bytes=%u sent=%d errno=%d\n",
                    ((unsigned int)bytes[16] << 8) | bytes[17], (unsigned int)length, (int)result, result < 0 ? saved : 0);
    }
    errno = saved; return result;
}
/* The AL MAC this sandbox presents: br0 is created with it before the daemon starts, so it is the identity every
 * 1905 frame of this node carries. The station-count answer has to leave with it as its source - the master files
 * the counts under the reply's source, and any other MAC there invents a node on its page. Read once: the bridge's
 * address does not change while the daemon runs, and a sandbox without br0 has no identity to answer as. */
static const uint8_t *local_al(void)
{
    static uint8_t al[6];
    static int known;
    struct ifreq request;
    int fd;
    if (!known) {
        memset(&request, 0, sizeof(request));
        memcpy(request.ifr_name, "br0", 4);
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            if (!ioctl(fd, SIOCGIFHWADDR, &request)) {
                memcpy(al, request.ifr_hwaddr.sa_data, 6);
                known = 1;
            }
            close(fd);
        }
    }
    return known ? al : NULL;
}
/* The station-count stand-in. The vendor agent answers an AP Metric Query with nothing, because the numbers it
 * would report come from cspd and the driver layer and neither exists in this sandbox; what the child host
 * measured is in /run/zte-mesh/ap-sta. The reply goes out on the socket the query arrived on, which is the only
 * one on the right interface with the right source MAC - and, unlike a sendto() call from here, it does not go
 * through this file's own wrapper, so the daemon sees one transaction and the trace line says what it was. */
static void answer_ap_metrics(int fd, const uint8_t *frame, size_t bytes, const struct sockaddr_storage *address)
{
    static ssize_t (*send_real)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    /* The counts alone answer in sixty bytes; sixteen APs with a full client list and thirty-two-byte SSIDs need
     * about 1.2 kB, which still fits the 1500-byte frame the raw socket carries. */
    uint8_t reply[1400], dest[6];
    const uint8_t *al;
    struct sockaddr_ll target;
    ssize_t sent;
    size_t used;
    int saved;
    if (!send_real) {
        send_real = (ssize_t (*)(int, const void *, size_t, int, const struct sockaddr *, socklen_t))
                    dlsym(RTLD_NEXT, "sendto");
        if (!send_real)
            return;
    }
    al = local_al();
    if (!al)
        return;
    used = agent_ap_metrics_answer(frame, bytes, reply, sizeof(reply), al, dest);
    if (!used)
        return;
    memset(&target, 0, sizeof(target));
    target.sll_family = AF_PACKET;
    target.sll_protocol = htons(CONTROLLER_1905_ETHER);
    target.sll_ifindex = ((const struct sockaddr_ll *)address)->sll_ifindex;
    target.sll_halen = 6;
    memcpy(target.sll_addr, dest, 6);
    sent = send_real(fd, reply, used, 0, (struct sockaddr *)&target, sizeof(target));
    saved = errno;
    message("ZTE_CONTROLLER_TRACE ap_metrics_answer bytes=%u ifindex=%d sent=%d errno=%d\n",
            (unsigned int)used, target.sll_ifindex, (int)sent, sent < 0 ? saved : 0);
    errno = saved;
}
EXPORT ssize_t recvfrom(int fd, void *data, size_t length, int flags,
                        struct sockaddr *source, socklen_t *source_size)
{
    ssize_t (*real)(int, void *, size_t, int, struct sockaddr *, socklen_t *) = symbol("recvfrom");
    ssize_t result = real(fd, data, length, flags, source, source_size);
    int saved = errno;
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    if (data && result >= 22 && !getsockname(fd, (struct sockaddr *)&address, &size) && address.ss_family == AF_PACKET) {
        const unsigned char *bytes = data;
        if (bytes[12] == 0x89 && bytes[13] == 0x3a) {
            controller_peer_note(bytes, (size_t)result);
            answer_ap_metrics(fd, bytes, (size_t)result, &address);
        }
        if (bytes[12] == 0x89 && bytes[13] == 0x3a)
            message("ZTE_CONTROLLER_TRACE rx1905 cmdu=%u bytes=%u ifindex=%d protocol=%u\n",
                    ((unsigned int)bytes[16] << 8) | bytes[17], (unsigned int)result,
                    ((struct sockaddr_ll *)&address)->sll_ifindex,
                    (unsigned int)ntohs(((struct sockaddr_ll *)&address)->sll_protocol));
    }
    errno = saved; return result;
}
EXPORT int ASEND(unsigned int event, const void *data, unsigned int length, void *destination)
{
    int (*real)(unsigned int, const void *, unsigned int, void *) = symbol("ASEND");
    int result = real(event, data, length, destination), saved = errno;
    if (event == 0x9092)
        message("ZTE_CONTROLLER_TRACE oss_dispatch event=37010 bytes=%u status=%d payload_logged=0\n", length, result);
    /* device on/off announcements (master_send_dev_on_msg and friends), for REPLAY_RESULTS section 15.1 */
    if ((event == 0xa937 || event == 0xa938) && destination)
        message("ZTE_CONTROLLER_TRACE device_event event=0x%x bytes=%u dest=%s status=%d epoch=%ld\n", event, length,
                (const char *)destination, result, (long)time(NULL));
    errno = saved; return result;
}
EXPORT int DH_generate_key(void *dh)
{
    int (*real)(void *) = symbol("DH_generate_key");
    int result = real(dh), saved = errno;
    message("ZTE_CONTROLLER_TRACE native_dh_keygen status=%d material_logged=0\n", result);
    errno = saved; return result;
}
EXPORT int dbAPILstView(const char *view, const char *instance, void *data)
{
    int (*real)(const char *, const char *, void *) = symbol("dbAPILstView");
    int result = real(view, instance, data), saved = errno;
    if (view && data && !strcmp(view, "P1905AcdBssCfg")) {
        uint32_t count = 0;
        if (!result) memcpy(&count, (uint8_t *)data + 8, 4);
        message("ZTE_CONTROLLER_TRACE bss_config list_status=%d count=%u body_logged=0\n", result, count);
    }
    errno = saved; return result;
}
EXPORT int dbAPIGetView(const char *view, const char *instance, void *data)
{
    int (*real)(const char *, const char *, void *) = symbol("dbAPIGetView");
    int result = real(view, instance, data), saved = errno;
    if (view && data && !strcmp(view, "P1905AcdBssCfg")) {
        uint32_t tag = 0, enabled = 0;
        if (!result) {
            memcpy(&tag, (uint8_t *)data + 228, 4);
            memcpy(&enabled, (uint8_t *)data + 260, 4);
        }
        message("ZTE_CONTROLLER_TRACE bss_config get_status=%d tag=%u enabled=%u body_logged=0\n", result, tag, enabled);
    }
    if (view && (!strcmp(view, "IPInterface") || !strcmp(view, "MultiAPDomain")))
        message("ZTE_CONTROLLER_TRACE db_get view=%s instance=%.36s status=%d\n", view, instance ? instance : "", result);
    else if (result || (view && !strcmp(view, "P1905ComCfg")))
        message("ZTE_CONTROLLER_TRACE db_get view=%.36s status=%d\n", view ? view : "", result);
    errno = saved; return result;
}
EXPORT int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    int (*real)(int, const struct sockaddr *, socklen_t) = symbol("bind");
    int result = real(fd, address, length), saved = errno;
    if (address && length >= sizeof(struct sockaddr_ll) && address->sa_family == AF_PACKET)
        message("ZTE_CONTROLLER_TRACE packet_bind ifindex=%d protocol=%u status=%d errno=%d\n",
                ((const struct sockaddr_ll *)address)->sll_ifindex,
                (unsigned int)ntohs(((const struct sockaddr_ll *)address)->sll_protocol), result, saved);
    else if (address && address->sa_family == AF_UNIX && length > sizeof(sa_family_t)) {
        /* The daemon's own OSS socket: its directory has to exist in the private root, so the path is the useful part of
         * a failure here. A socket name carries no credential. */
        char path[108];
        size_t room = length - offsetof(struct sockaddr_un, sun_path);
        if (room >= sizeof(path))
            room = sizeof(path) - 1;
        memcpy(path, ((const struct sockaddr_un *)address)->sun_path, room);
        path[room] = 0;
        message("ZTE_CONTROLLER_TRACE unix_bind path=%.90s status=%d errno=%d\n", path, result, saved);
    } else
        message("ZTE_CONTROLLER_TRACE bind status=%d errno=%d\n", result, saved);
    errno = saved; return result;
}
EXPORT int OssCreateSubProc(const char *name, void *callback, void *arg1, void *arg2)
{
    int (*real)(const char *, void *, void *, void *) = symbol("OssCreateSubProc");
    int result = real(name, callback, arg1, arg2), saved = errno;
    message("ZTE_CONTROLLER_TRACE return=OssCreateSubProc status=%d errno=%d\n", result, saved);
    errno = saved; return result;
}
/* The three WLAN setters that carry an M2 into the configuration database are not traced here: the
 * runtime stand-in for the configuration manager (libzte_agent_mm.so) owns them, because the status it
 * reports is what the daemon acts on and a second interposer would only hide it.
 */
EXPORT int OssSetThdInfo(void *name, unsigned int index, unsigned int priority, unsigned int options)
{
    int (*real)(void *, unsigned int, unsigned int, unsigned int) = symbol("OssSetThdInfo");
    int result = real(name, index, priority, options), saved = errno;
    message("ZTE_CONTROLLER_TRACE return=OssSetThdInfo status=%d errno=%d\n", result, saved);
    errno = saved; return result;
}
EXPORT int vsnprintf(char *buffer, size_t size, const char *format, va_list ap)
{
    Dl_info info;
    char text[241];
    unsigned int i;
    int result, saved;
    if (!format_real)
        format_real = (int (*)(char *, size_t, const char *, va_list))dlsym(RTLD_NEXT, "vsnprintf");
    if (!format_real)
        _exit(79);
    result = format_real(buffer, size, format, ap); saved = errno;
    if (dladdr(__builtin_return_address(0), &info) && info.dli_sname &&
        (!strcmp(info.dli_sname, "ProcUserLog") || !strcmp(info.dli_sname, "ProcOssLogType"))) {
        if (strstr(format, "M1") || strstr(format, "M2") || strstr(format, "WSC"))
            message("ZTE_CONTROLLER_TRACE wsc_status vendor_format=%.120s\n", format);
        /* Opt-in firehose: while a frame shape is still being matched, the daemon's own objections are the
         * only evidence, and they do not all mention M1/M2/WSC. Enabled by a flag file, not an environment
         * variable, because the sandbox hands the daemon a single-variable environment. Its budget is the
         * gate's, so turning it on cannot outlast one long run. */
        if (verbose_enabled())
            message("ZTE_TRACE_ALL vendor_format=%.150s\n", format);
        /* Values rather than shapes, for the handful of lines the agent-mode criterion
         * turns on: whether the M2 was applied (cfgChg), which BSS it went to, what the
         * WLAN setter returned. controller-log.c owns that list, splits it into the four
         * per-event lines and the two the state machine repeats every few seconds, and
         * refuses any text carrying key material - a value line prints the formatted
         * result, so the format is the only thing checked here. */
        if (size > 0) {
            int shape = controller_log_shape(format);
            if (shape == ZTE_LOG_VALUE)
                message("ZTE_TRACE_VALUE text=%.200s\n", buffer);
            else if (shape == ZTE_LOG_POLL)
                message("ZTE_TRACE_POLL text=%.200s\n", buffer);
        }
        if (strstr(format, "Bss") || strstr(format, "bss") || strstr(format, "WLANCFG") ||
            strstr(format, "PWKey") || strstr(format, "WEP") || strstr(format, "Wep") || strstr(format, "BeaconType"))
            message("ZTE_CONTROLLER_TRACE bss_config vendor_format=%.120s\n", format);
        /* Fixed protocol rejection labels retain no MAC/SSID/key varargs and
         * have an independent packet budget; repeated DB errors cannot hide them. */
        if (strstr(format, "No one care the msg,type:"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=no_registered_handler\n");
        else if (strstr(format, "Unsupported version:"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=version\n");
        else if (strstr(format, "Bad length of the frame"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=short_frame\n");
        else if (strstr(format, "dest mac is") && strstr(format, "not local and multicast"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=destination_not_local\n");
        else if (strstr(format, "not allowed to MESH with the current device"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=peer_policy\n");
        else if (strstr(format, "had sent,ignore"))
            message("ZTE_CONTROLLER_TRACE packet_decision reject=self_source\n");
        /* The authentication type negotiated for an M2 (FEATURE_MAP section 12): the two vendor formats only, which carry
         * nothing but hex numbers (configured/agent-advertised/final authentication type bit masks). They are level-8
         * messages: nothing appears at the daemon's default log level (replay run 28), so the negotiated type was checked
         * through the agent's own BeaconType read-back instead. */
        if (strstr(format, "Change Auth Type to") || strstr(format, "slave_auth_type"))
            message("ZTE_CONTROLLER_TRACE auth %.80s\n", buffer);
        /* Suppress routine timer/config chatter, keeping startup/error formats.
         * Never copy substituted MAC/key/SSID/serial/body values.
         */
        if (!strstr(format, "fail") && !strstr(format, "Fail") && !strstr(format, "ERROR") &&
            !strstr(format, "error") && !strstr(format, "MASTER") &&
            !strstr(format, "Creating a new") && !strstr(format, "Server")) {
            errno = saved;
            return result;
        }
        for (i = 0; i < sizeof(text) - 1 && format[i]; i++)
            text[i] = (unsigned char)format[i] >= 32 && (unsigned char)format[i] <= 126 ? format[i] : ' ';
        text[i] = 0;
        message("ZTE_CONTROLLER_TRACE vendor_log_format=%s\n", text);
    }
    errno = saved; return result;
}
