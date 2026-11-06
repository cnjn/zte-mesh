/* spark: look at the 1905.1 wire from outside the sandbox.
 *
 * The vendor daemon only ever shows what it wants to show. This tool sits on a real
 * interface (host namespace) and either watches for IEEE 1905.1 frames (ethertype
 * 0x893a) or puts an AP-Autoconfiguration-free Topology Discovery broadcast on the
 * wire, which is the one frame every EasyMesh node has to answer. It prints what it
 * sends and what it hears, nothing else, and never parses beyond the CMDU header.
 *
 *   spark <iface> listen [seconds]
 *   spark <iface> discovery [seconds]     # discovery every 5 s, plus every received frame
 *
 * Static build for both lab devices:  zig cc -target {x86_64,aarch64}-linux-musl -static
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETHERTYPE_1905 0x893a
#define BROADCAST_LIMIT 4096

/* A packet socket only sees what the card hands the stack. 1905 traffic is multicast
 * (01:80:c2:00:00:13) or unicast to the node's AL MAC, so on a host interface that is
 * not a bridge port the NIC filters it away before the socket sees it - which is why
 * this sets promiscuous mode for the duration and puts the flag back on exit. */
static int promiscuous(const char *name, int on, int *was)
{
    struct ifreq request;
    int fd, status;
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    status = ioctl(fd, SIOCGIFFLAGS, &request);
    if (!status && was)
        *was = !!(request.ifr_flags & IFF_PROMISC);
    if (!status) {
        if (on)
            request.ifr_flags |= IFF_PROMISC;
        else
            request.ifr_flags &= ~IFF_PROMISC;
        status = ioctl(fd, SIOCSIFFLAGS, &request);
    }
    close(fd);
    return status;
}

static int interface_mac(const char *name, uint8_t out[6])
{
    struct ifreq request;
    int fd, status;
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    status = ioctl(fd, SIOCGIFHWADDR, &request);
    close(fd);
    if (status || request.ifr_hwaddr.sa_family != 1)
        return -1;
    memcpy(out, request.ifr_hwaddr.sa_data, 6);
    return 0;
}

static int wire_open(const char *name, int *index)
{
    struct sockaddr_ll address;
    int fd;
    memset(&address, 0, sizeof(address));
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETHERTYPE_1905);
    address.sll_ifindex = (int)if_nametoindex(name);
    if (!address.sll_ifindex) {
        fprintf(stderr, "spark: no such interface %s\n", name);
        return -1;
    }
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETHERTYPE_1905));
    if (fd < 0) {
        fprintf(stderr, "spark: socket failed errno=%d\n", errno);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address))) {
        fprintf(stderr, "spark: bind %s failed errno=%d\n", name, errno);
        close(fd);
        return -1;
    }
    *index = address.sll_ifindex;
    return fd;
}

/* Topology Discovery: AL MAC TLV + End of Message. The message id is the only field
 * that varies, so a receiver can tell two probes apart. */
static unsigned int discovery_frame(uint8_t *frame, const uint8_t mac[6], unsigned int id)
{
    const uint8_t destination[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    unsigned int at = 0;
    memcpy(frame + at, destination, 6);
    at += 6;
    memcpy(frame + at, mac, 6);
    at += 6;
    frame[at++] = (uint8_t)(ETHERTYPE_1905 >> 8);
    frame[at++] = (uint8_t)(ETHERTYPE_1905 & 0xff);
    frame[at++] = 0;                    /* 1905 revision */
    frame[at++] = 0;                    /* reserved */
    frame[at++] = 0x00;                 /* CMDU type 0x0000: Topology Discovery */
    frame[at++] = 0x00;
    frame[at++] = (uint8_t)(id >> 8);   /* message id */
    frame[at++] = (uint8_t)(id & 0xff);
    frame[at++] = 0;                    /* fragment id */
    frame[at++] = 0x80;                 /* last fragment */
    frame[at++] = 0x01;                 /* TLV 0x01: AL MAC address */
    frame[at++] = 0x00;
    frame[at++] = 6;
    memcpy(frame + at, mac, 6);
    at += 6;
    frame[at++] = 0x00;                 /* TLV 0x00: end of message */
    frame[at++] = 0x00;
    frame[at++] = 0x00;
    return at;
}

static int discovery_send(int fd, int index, const uint8_t *frame, unsigned int bytes)
{
    struct sockaddr_ll target;
    memset(&target, 0, sizeof(target));
    target.sll_family = AF_PACKET;
    target.sll_protocol = htons(ETHERTYPE_1905);
    target.sll_ifindex = index;
    target.sll_halen = 6;
    memset(target.sll_addr, 0xff, 6);
    if (sendto(fd, frame, bytes, 0, (struct sockaddr *)&target, sizeof(target)) != (ssize_t)bytes) {
        printf("spark tx=failed errno=%d\n", errno);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t mac[6] = { 0 }, probe[64];
    unsigned int seconds = 30, id = 0x2200, bytes, sent = 0, heard = 0;
    struct timespec start, now;
    int fd, index = 0, send_discovery, next_send = 0, was_promiscuous = 0;
    if (argc < 3 || (strcmp(argv[2], "listen") && strcmp(argv[2], "discovery"))) {
        fprintf(stderr, "usage: spark <iface> listen|discovery [seconds]\n");
        return 2;
    }
    send_discovery = !strcmp(argv[2], "discovery");
    if (argc > 3 && atoi(argv[3]) > 0)
        seconds = (unsigned int)atoi(argv[3]);
    if (interface_mac(argv[1], mac)) {
        fprintf(stderr, "spark: no hardware address for %s\n", argv[1]);
        return 1;
    }
    printf("spark iface=%s mode=%s al_mac=%02x:%02x:%02x:%02x:%02x:%02x\n", argv[1], argv[2], mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
    fflush(stdout);
    promiscuous(argv[1], 1, &was_promiscuous);
    fd = wire_open(argv[1], &index);
    if (fd < 0) {
        promiscuous(argv[1], was_promiscuous, NULL);
        return 1;
    }
    bytes = discovery_frame(probe, mac, id);
    if (clock_gettime(CLOCK_MONOTONIC, &start))
        return 1;
    for (;;) {
        struct pollfd ready = { .fd = fd, .events = POLLIN };
        long elapsed;
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            break;
        elapsed = now.tv_sec - start.tv_sec;
        if (elapsed >= (long)seconds)
            break;
        if (send_discovery && elapsed >= next_send) {
            next_send = (int)elapsed + 5;
            id++;
            bytes = discovery_frame(probe, mac, id);
            if (!discovery_send(fd, index, probe, bytes)) {
                sent++;
                printf("spark tx=discovery id=0x%04x bytes=%u\n", id, bytes);
                fflush(stdout);
            }
        }
        if (poll(&ready, 1, 500) <= 0) {
            if (errno == EINTR)
                continue;
            else if (errno != 0 && errno != EAGAIN && errno != ETIMEDOUT)
                break;
        }
        for (;;) {
            uint8_t frame[2048];
            ssize_t got = recv(fd, frame, sizeof(frame), MSG_DONTWAIT);
            if (got < 0)
                break;
            if (got >= 22 && frame[12] == 0x89 && frame[13] == 0x3a) {
                heard++;
                printf("spark rx src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x "
                       "cmdu=0x%04x mid=0x%04x bytes=%u\n",
                       frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], frame[0], frame[1], frame[2],
                       frame[3], frame[4], frame[5], ((unsigned int)frame[16] << 8) | frame[17],
                       ((unsigned int)frame[18] << 8) | frame[19], (unsigned int)got);
                fflush(stdout);
            }
        }
    }
    close(fd);
    promiscuous(argv[1], was_promiscuous, NULL);
    printf("spark done sent=%u heard=%u\n", sent, heard);
    return 0;
}
