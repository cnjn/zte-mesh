/* Controller-only L2 identity adapter, not a WAN/IP-routing implementation.
 * Exact 532-byte output and MAC+172 verified in vendor _baseALConfigInit.
 * Return success ONLY after actual private Linux interface ioctls succeed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))

/* Stock binds br0 but its socket-record classifier understands physical eth/
 * veth names, not br0. Map ONLY this private lab alias to the actual ingress
 * veth; do not forge hardware/WLAN success or alter recvfrom metadata.
 */
EXPORT int bind(int fd, const struct sockaddr *address, socklen_t bytes)
{
    int (*real)(int, const struct sockaddr *, socklen_t) = dlsym(RTLD_NEXT, "bind");
    const char *sandbox = getenv("ZTE_MESH_DB_SANDBOX");
    struct sockaddr_ll mapped;
    unsigned int bridge, link;
    int result, saved;
    if (!real) { errno = ENOSYS; return -1; }
    if (sandbox && !strcmp(sandbox, "1") && address && bytes == sizeof(mapped) &&
        address->sa_family == AF_PACKET) {
        memcpy(&mapped, address, sizeof(mapped));
        bridge = if_nametoindex("br0");
        if (bridge && mapped.sll_ifindex == (int)bridge &&
            (ntohs(mapped.sll_protocol) == 0x893a || ntohs(mapped.sll_protocol) == 4)) {
            char note[128];
            int n;
            link = if_nametoindex("veth-lab");
            if (!link) { errno = ENODEV; return -1; }
            mapped.sll_ifindex = (int)link;
            result = real(fd, (const struct sockaddr *)&mapped, sizeof(mapped)); saved = errno;
            n = snprintf(note, sizeof(note), "ZTE_CONTROLLER_WIRE actual_lab_bind=1 status=%d host_link=0\n", result);
            if (n > 0 && (size_t)n < sizeof(note)) (void)write(2, note, (size_t)n);
            errno = saved; return result;
        }
    }
    return real(fd, address, bytes);
}
EXPORT int GetRouteIFInfo(const char *instance, void *output)
{
    const char *sandbox = getenv("ZTE_MESH_DB_SANDBOX");
    struct ifreq request = {0};
    uint8_t data[532] = {0}, mac[6];
    uint32_t mtu;
    short flags;
    int fd, saved;
    if (!sandbox || strcmp(sandbox, "1") || !instance || !output || strcmp(instance, "DEV.IP.IF1")) {
        errno = EOPNOTSUPP;
        return -1;
    }
    memcpy(request.ifr_name, "br0", 4);
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (ioctl(fd, SIOCGIFHWADDR, &request))
        goto failed;
    memcpy(mac, request.ifr_hwaddr.sa_data, sizeof(mac));
    if (request.ifr_hwaddr.sa_family != 1 || !(mac[0] & 2) || (mac[0] & 1) ||
        ioctl(fd, SIOCGIFFLAGS, &request)) {
        errno = ENODEV;
        goto failed;
    }
    flags = request.ifr_flags;
    if (!(flags & IFF_UP) || ioctl(fd, SIOCGIFMTU, &request)) {
        errno = ENETDOWN;
        goto failed;
    }
    mtu = (uint32_t)request.ifr_mtu;
    if (mtu < 576 || mtu > 65535) {
        errno = EINVAL;
        goto failed;
    }
    close(fd);
    memcpy(data, "DEV.IP.IF1", 11);
    memcpy(data + 32, "br0", 4);
    data[120] = 1; /* L2 interface exists. */
    data[126] = 1; /* Bridge. No L3/DNS/routes are claimed. */
    memcpy(data + 172, mac, sizeof(mac));
    memcpy(data + 192, &mtu, sizeof(mtu));
    memcpy(output, data, sizeof(data));
    /* Fixed metadata only, no actual MAC/identity/profile value. */
    {
        static const char note[] = "ZTE_CONTROLLER_ROUTE actual_private_if=1 route_bytes=532 l3_supported=0\n";
        (void)write(STDERR_FILENO, note, sizeof(note) - 1);
    }
    return 0;
failed:
    saved = errno;
    close(fd); errno = saved;
    return -1;
}

/* Stand-in for cspd's multiapd_mgr on one request. After a physical link-up event the ACD arms a 3 s timer; when it
 * fires, acdsCheckMultiapdRestart asks cspd.cspd.multiapd_mgr (SSEND event 0xa96e, 6 s timeout) how often the daemon
 * has been started and sends the first AP-autoconfiguration Renew of both bands only if the count is above 1, i.e.
 * after a restart (REPLAY_RESULTS section 12; the factory log shows the Renew pair 3.3 s after each restart and none
 * after the first start). cspd is not running here, so the request fails and the Renew never goes out. The supervisor
 * counts its daemon starts in ZTE_MESH_START_COUNT; this answers that one request with it and forwards every other
 * message untouched. The reply is {i32 status 0, u32 unknown, u32 start count}: the caller reads the count at offset 8
 * and requires the first word to be non-negative. */
EXPORT int SSEND(unsigned int event, void *in, unsigned int length, unsigned int timeout, void *out, void *out_length,
                 const char *destination, void *a7, void *a8, void *a9)
{
    int (*real)(unsigned int, void *, unsigned int, unsigned int, void *, void *, const char *, void *, void *, void *) =
        dlsym(RTLD_NEXT, "SSEND");
    const char *count = getenv("ZTE_MESH_START_COUNT");
    if (event == 0xa96e && count && out && destination && !strcmp(destination, "cspd.cspd.multiapd_mgr")) {
        uint32_t reply[3] = { 0, 0, (uint32_t)strtoul(count, NULL, 10) };
        uint16_t reply_length = sizeof(reply);
        memcpy(out, reply, sizeof(reply));
        if (out_length)
            memcpy(out_length, &reply_length, sizeof(reply_length));
        return 0;
    }
    if (!real) { errno = ENOSYS; return -1; }
    return real(event, in, length, timeout, out, out_length, destination, a7, a8, a9);
}
