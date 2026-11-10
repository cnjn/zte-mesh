#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/controller-actions.h"
#include "../src/controller-packet.h"
static const uint8_t controller[6] = {2, 0, 0, 0, 0x19, 6};
static uint8_t frame[1601], base[1601];
static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "packet codec failure line %d\n", __LINE__); exit(1); } } while (0)
enum send_failure { SEND_OK, NO_PORT, NO_SOCKET, NO_MAC, WRONG_FAMILY, SEND_ERROR };
static enum send_failure send_failure;
static unsigned int opened, closed, sent;
static uint8_t local_mac[6] = {2, 0, 0, 0, 0x19, 0x73};
static const uint8_t agent_mac[6] = {2, 0, 0, 0, 0x20, 1};
static uint8_t sent_frame[1600];
static size_t sent_bytes;

unsigned int if_nametoindex(const char *name)
{
    CHECK(!strcmp(name, "veth-lab"));
    if (send_failure == NO_PORT) {
        errno = ENODEV;
        return 0;
    }
    return 7;
}

int socket(int domain, int type, int protocol)
{
    CHECK(domain == AF_PACKET);
    CHECK(type == (SOCK_RAW | SOCK_CLOEXEC));
    CHECK(protocol == htons(0x893a));
    if (send_failure == NO_SOCKET) {
        errno = EPERM;
        return -1;
    }
    opened++;
    return 42;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    struct ifreq *interface;
    CHECK(fd == 42 && request == SIOCGIFHWADDR);
    va_start(args, request);
    interface = va_arg(args, struct ifreq *);
    va_end(args);
    CHECK(!strcmp(interface->ifr_name, "br0"));
    if (send_failure == NO_MAC) {
        errno = ENODEV;
        return -1;
    }
    interface->ifr_hwaddr.sa_family = send_failure == WRONG_FAMILY ? 0 : 1;
    memcpy(interface->ifr_hwaddr.sa_data, local_mac, 6);
    return 0;
}

ssize_t sendto(int fd, const void *buffer, size_t bytes, int flags,
               const struct sockaddr *address, socklen_t address_bytes)
{
    const struct sockaddr_ll *target = (const struct sockaddr_ll *)address;
    CHECK(fd == 42 && flags == 0 && address_bytes == sizeof(*target));
    CHECK(target->sll_family == AF_PACKET && target->sll_protocol == htons(0x893a));
    CHECK(target->sll_ifindex == 7 && target->sll_halen == 6);
    CHECK(!memcmp(target->sll_addr, agent_mac, 6));
    CHECK(bytes <= sizeof(sent_frame));
    sent++;
    memcpy(sent_frame, buffer, bytes);
    sent_bytes = bytes;
    if (send_failure == SEND_ERROR) {
        errno = ENETDOWN;
        return -1;
    }
    return (ssize_t)bytes;
}

int close(int fd)
{
    CHECK(fd == 42);
    closed++;
    errno = EBADF;
    return 0;
}

static void client_action_tests(void)
{
    static const uint8_t bssid[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t client[6] = {2, 1, 2, 3, 4, 5};
    unsigned int i;
    int kind;
    CHECK(unsetenv("ZTE_MESH_DB_SANDBOX") == 0);
    CHECK(controller_client_action(agent_mac, bssid, client, ACTION_KIND_BAN, 0, 0) == -1);
    CHECK(opened == 0 && sent == 0);
    CHECK(setenv("ZTE_MESH_DB_SANDBOX", "1", 1) == 0);
    for (kind = ACTION_KIND_KICK; kind <= ACTION_KIND_BAN; kind++) {
        for (i = 0; i < 2; i++) {
            local_mac[5] = (uint8_t)(0x73 + i);
            send_failure = SEND_OK;
            opened = closed = sent = 0;
            CHECK(controller_client_action(agent_mac, bssid, client, kind, (int)i, 30) == 0);
            CHECK(opened == 1 && closed == 1 && sent == 1);
            /* Builders add no Ethernet pad: 22 header + 9 AL MAC + 3 End of Message, plus 37 (kick) or 19 (ban). */
            CHECK(sent_bytes == (kind == ACTION_KIND_KICK ? 71u : 53u));
            CHECK(!memcmp(sent_frame, agent_mac, 6));
            CHECK(!memcmp(sent_frame + 6, local_mac, 6));
            CHECK(sent_frame[16] == 0x80 && sent_frame[17] == (kind == ACTION_KIND_KICK ? 0x14 : 0x16));
        }
    }
    for (send_failure = NO_PORT; send_failure <= SEND_ERROR; send_failure++) {
        opened = closed = sent = 0;
        CHECK(controller_client_action(agent_mac, bssid, client, ACTION_KIND_BAN, 0, 0) == -1);
        CHECK(opened == closed);
        CHECK(sent == (send_failure == SEND_ERROR ? 1u : 0u));
        if (send_failure == SEND_ERROR)
            CHECK(errno == ENETDOWN);
        if (send_failure == NO_MAC)
            CHECK(errno == ENODEV);
        if (send_failure == WRONG_FAMILY)
            CHECK(errno == EAFNOSUPPORT);
    }
    send_failure = SEND_OK;
    opened = closed = sent = 0;
    CHECK(controller_client_action(agent_mac, bssid, client, ACTION_KIND_NONE, 0, 0) == -1);
    CHECK(controller_client_action(NULL, bssid, client, ACTION_KIND_BAN, 0, 0) == -1);
    CHECK(opened == 0 && sent == 0);
    CHECK(unsetenv("ZTE_MESH_DB_SANDBOX") == 0);
}

static void reset(void) { memcpy(frame, base, sizeof(frame)); }
static void autoconfig_tests(void)
{
    static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
    unsigned int band, i;
    for (band = 0; band < 2; band++) {
        memset(frame, 0, sizeof(frame));
        memcpy(frame, peer, 6); memcpy(frame + 6, controller, 6);
        frame[12] = 0x89; frame[13] = 0x3a; frame[17] = 8;
        frame[18] = 0x12; frame[19] = (uint8_t)(3 + band); frame[21] = 0x80;
        frame[22] = 0x0f; frame[24] = 1;
        frame[26] = 0x10; frame[28] = 1; frame[29] = (uint8_t)band;
        frame[30] = 0x80; frame[32] = 2; frame[33] = 1;
        CHECK(controller_autoconfig_response_matches(frame, 38, controller, band));
        CHECK(controller_autoconfig_response_matches(frame, 60, controller, band));
        CHECK(!controller_autoconfig_response_matches(frame, 38, controller, 2));
        CHECK(!controller_autoconfig_response_matches(NULL, 38, controller, band));
        CHECK(!controller_autoconfig_response_matches(frame, 38, NULL, band));
        for (i = 0; i < 38; i++) CHECK(!controller_autoconfig_response_matches(frame, i, controller, band));
        frame[25] = 1; CHECK(!controller_autoconfig_response_matches(frame, 38, controller, band)); frame[25] = 0;
        frame[29] ^= 1; CHECK(!controller_autoconfig_response_matches(frame, 38, controller, band)); frame[29] ^= 1;
        frame[34] = 1; CHECK(!controller_autoconfig_response_matches(frame, 38, controller, band)); frame[34] = 0;
        frame[33] = 2; CHECK(!controller_autoconfig_response_matches(frame, 38, controller, band)); frame[33] = 1;
        frame[19] ^= 1; CHECK(!controller_autoconfig_response_matches(frame, 38, controller, band)); frame[19] ^= 1;
        frame[38] = 1; CHECK(!controller_autoconfig_response_matches(frame, 60, controller, band)); frame[38] = 0;
        frame[31] = 0xff; CHECK(!controller_autoconfig_response_matches(frame, 60, controller, band));
    }
}
int main(void)
{
    static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
    unsigned int i, offsets[] = {5, 11, 13, 14, 15, 17, 19, 20, 21, 25, 31, 34};
    uint32_t seed = 0x19051202;
    autoconfig_tests();
    client_action_tests();
    memcpy(base, peer, 6); memcpy(base + 6, controller, 6);
    base[12] = 0x89; base[13] = 0x3a; base[17] = 3;
    base[18] = 0x12; base[19] = 2; base[21] = 0x80;
    base[22] = 3; base[24] = 7; memcpy(base + 25, controller, 6);
    reset(); CHECK(controller_topology_response_matches(frame, 35, controller));
    CHECK(controller_topology_response_matches(frame, 60, controller));
    CHECK(controller_topology_response_matches(frame, 1600, controller));
    CHECK(!controller_topology_response_matches(frame, 1601, controller));
    CHECK(!controller_topology_response_matches(NULL, 35, controller));
    CHECK(!controller_topology_response_matches(frame, 35, NULL));
    for (i = 0; i < 35; i++) CHECK(!controller_topology_response_matches(frame, i, controller));
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        reset(); frame[offsets[i]] ^= 1;
        CHECK(!controller_topology_response_matches(frame, 35, controller));
    }
    reset(); frame[23] = 0xff; frame[24] = 0xff;
    CHECK(!controller_topology_response_matches(frame, 60, controller));
    reset(); frame[35] = 1;
    CHECK(!controller_topology_response_matches(frame, 60, controller));
    reset(); memcpy(frame + 32, frame + 22, 10);
    CHECK(!controller_topology_response_matches(frame, 45, controller));
    reset(); frame[22] = 4;
    CHECK(!controller_topology_response_matches(frame, 35, controller));
    reset(); memmove(frame + 36, frame + 32, 3); frame[32] = 0xf0; frame[34] = 1; frame[35] = 0x42;
    CHECK(controller_topology_response_matches(frame, 39, controller));
    reset(); frame[24] = 16; frame[31] = 1; memcpy(frame + 32, peer, 6);
    CHECK(controller_topology_response_matches(frame, 44, controller));
    frame[40] = 1;
    CHECK(!controller_topology_response_matches(frame, 44, controller));
    frame[24] = 17; frame[41] = 0xab;
    CHECK(controller_topology_response_matches(frame, 45, controller));
    frame[31] = 2;
    CHECK(!controller_topology_response_matches(frame, 45, controller));
    /* Deterministic bounded fuzz: no network or namespace access. */
    for (i = 0; i < 100000; i++) {
        size_t size, pos;
        int result;
        reset(); seed = seed * 1664525u + 1013904223u; pos = seed % sizeof(frame);
        frame[pos] ^= (uint8_t)(1u << ((seed >> 24) & 7));
        seed = seed * 1664525u + 1013904223u; size = seed % 1602;
        result = controller_topology_response_matches(frame, size, controller);
        if (result != 0 && result != 1) return 1;
    }
    printf("controller-packet-codec: %u assertions; 100000 bounded mutations passed\n", checks);
    return 0;
}
