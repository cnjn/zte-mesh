/* enroller: the controller half of the enrollment handshake, without the controller.
 *
 * The factory agent only sends its M1 when something answers, and the answer is an
 * AP-Autoconfiguration Response (CMDU 0x0008): _acdcConfigHandler's event 8 calls
 * _acdcSendApAutoConfigWSCM1, the same sender the Search timer's second expiry uses.
 * The vendor controller in the sandbox never sends one, because its per-radio search
 * flag ([basic+0x114+radio*12]) is only set by cspd, which does not exist there.
 *
 * So this tool does the one thing the controller is missing: it watches a real
 * interface for the agent's AP-Autoconfiguration Search (CMDU 0x0007) and answers it
 * with the Response frame shape that is already known to make the factory agent emit
 * M1 (controller-packet.c:controller_autoconfig_response, proven under
 * `db-engine-probe --agent-startup`). The M1 then goes to the address this tool
 * answered from, so --al-mac defaults to the controller's AL MAC: the sandbox
 * controller receives the M1 and runs the real M2.
 *
 * Wire format as the factory daemon writes it (measured, tools/lab/README.md):
 *   8-byte CMDU header  (version, reserved, type BE, id BE, fragment, flags)
 *   TLVs from offset 8  (1-byte type, 2-byte BE length, value)
 *
 *   enroller <iface> [seconds] [--al-mac aa:bb:cc:dd:ee:ff] [--band N] [--quiet]
 *
 * Static build for both lab devices:  zig cc -target {x86_64,aarch64}-linux-musl -static
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETHERTYPE_1905 0x893a
#define HEADER_BYTES 8          /* 1905 header, TLVs start right after it */
#define FRAME_BYTES 22          /* ethernet + 1905 header, i.e. where TLVs start */
#define GROUP { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x13 }

static const uint8_t group[6] = GROUP;

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

static int parse_mac(const char *text, uint8_t out[6])
{
    unsigned int b[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return -1;
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xff)
            return -1;
        out[i] = (uint8_t)b[i];
    }
    return 0;
}

static void hexdump(const char *prefix, const uint8_t *data, unsigned int bytes)
{
    unsigned int i;
    printf("%s", prefix);
    for (i = 0; i < bytes && i < 48; ++i)
        printf("%02x", data[i]);
    if (bytes > 48)
        printf("..");
    printf(" (%u bytes)\n", bytes);
}

/* Walk the factory's TLV list: 1-byte type, 2-byte big-endian length. */
static void list_tlvs(const uint8_t *frame, unsigned int bytes)
{
    unsigned int at = FRAME_BYTES;
    while (at + 3 <= bytes) {
        unsigned int type = frame[at];
        unsigned int length = ((unsigned int)frame[at + 1] << 8) | frame[at + 2];
        if (!type)
            break;                       /* end of message */
        if (at + 3 + length > bytes) {
            printf("   tlv=0x%02x len=%u truncated\n", type, length);
            break;
        }
        printf("   tlv=0x%02x len=%u value=", type, length);
        for (unsigned int i = 0; i < length && i < 12; ++i)
            printf("%02x", frame[at + 3 + i]);
        if (length > 12)
            printf("..");
        printf("\n");
        at += 3 + length;
    }
    if (at + 3 > bytes)
        printf("   (no end-of-message TLV)\n");
}

static unsigned int tlv_value(const uint8_t *frame, unsigned int bytes, unsigned int type, int *found)
{
    unsigned int at = FRAME_BYTES;
    while (at + 3 <= bytes) {
        unsigned int length = ((unsigned int)frame[at + 1] << 8) | frame[at + 2];
        if (!frame[at])
            break;
        if (at + 3 + length > bytes)
            break;
        if (frame[at] == type && length >= 1) {
            *found = 1;
            return frame[at + 3];
        }
        at += 3 + length;
    }
    *found = 0;
    return 0;
}

static int send_frame(int fd, int index, const uint8_t *frame, unsigned int bytes, const uint8_t *to)
{
    struct sockaddr_ll target;
    memset(&target, 0, sizeof(target));
    target.sll_family = AF_PACKET;
    target.sll_protocol = htons(ETH_P_ALL);
    target.sll_ifindex = index;
    target.sll_halen = 6;
    memcpy(target.sll_addr, to, 6);
    if (sendto(fd, frame, bytes, 0, (struct sockaddr *)&target, sizeof(target)) != (ssize_t)bytes) {
        printf("enroller tx=failed errno=%d\n", errno);
        return -1;
    }
    return 0;
}

/* AP-Autoconfiguration Response, byte for byte what controller-packet.c builds: the
 * shape the factory agent answered M1 to in the lab. */
static unsigned int response_frame(uint8_t *frame, const uint8_t agent[6], const uint8_t controller[6],
                                   unsigned int band, unsigned int mid)
{
    unsigned int at = 0;
    memcpy(frame + at, agent, 6);         at += 6;
    memcpy(frame + at, controller, 6);    at += 6;
    frame[at++] = (uint8_t)(ETHERTYPE_1905 >> 8);
    frame[at++] = (uint8_t)(ETHERTYPE_1905 & 0xff);
    frame[at++] = 0;                      /* revision */
    frame[at++] = 0;                      /* reserved */
    frame[at++] = 0x00; frame[at++] = 0x08;   /* CMDU 0x0008 */
    frame[at++] = (uint8_t)(mid >> 8); frame[at++] = (uint8_t)(mid & 0xff);
    frame[at++] = 0;                      /* fragment id */
    frame[at++] = 0x80;                   /* last fragment */
    frame[at++] = 0x0f; frame[at++] = 0; frame[at++] = 1; frame[at++] = 0;               /* searched role: registrar */
    frame[at++] = 0x10; frame[at++] = 0; frame[at++] = 1; frame[at++] = (uint8_t)band;   /* frequency band */
    frame[at++] = 0x80; frame[at++] = 0; frame[at++] = 2; frame[at++] = 1; frame[at++] = 0; /* service: controller */
    return at;
}

/* AP-Autoconfiguration Search, as controller-packet.c writes it. Only used by --search:
 * it makes an agent that already gave up searching start over. */
static unsigned int search_frame(uint8_t *frame, const uint8_t controller[6], unsigned int band, unsigned int mid)
{
    unsigned int at = 0;
    memcpy(frame + at, group, 6);         at += 6;
    memcpy(frame + at, controller, 6);    at += 6;
    frame[at++] = (uint8_t)(ETHERTYPE_1905 >> 8);
    frame[at++] = (uint8_t)(ETHERTYPE_1905 & 0xff);
    frame[at++] = 0; frame[at++] = 0;
    frame[at++] = 0x00; frame[at++] = 0x07;   /* CMDU 0x0007 */
    frame[at++] = (uint8_t)(mid >> 8); frame[at++] = (uint8_t)(mid & 0xff);
    frame[at++] = 0;
    frame[at++] = 0x80;
    frame[at++] = 0x0d; frame[at++] = 0; frame[at++] = 1; frame[at++] = 0;               /* searched role: registrar */
    frame[at++] = 0x0e; frame[at++] = 0; frame[at++] = 1; frame[at++] = (uint8_t)band;   /* band */
    frame[at++] = 0x80; frame[at++] = 0; frame[at++] = 2; frame[at++] = 1; frame[at++] = 1; /* supported: agent */
    frame[at++] = 0x81; frame[at++] = 0; frame[at++] = 2; frame[at++] = 1; frame[at++] = 0; /* searched: controller */
    frame[at++] = 0xb3; frame[at++] = 0; frame[at++] = 1; frame[at++] = 1;               /* Multi-AP profile 1 */
    return at;
}

int main(int argc, char **argv)
{
    uint8_t controller[6] = { 0 }, iface_mac[6] = { 0 }, buffer[1600];
    unsigned int seconds = 60, band = 0, mid = 0x3000, replies = 0, heard = 0;
    int fd, index = 0, was_promiscuous = 0, quiet = 0, want_search = 0, status;
    const char *name;
    struct timespec start, now;
    if (argc < 2) {
        fprintf(stderr, "usage: enroller <iface> [seconds] [--al-mac aa:bb:cc:dd:ee:ff] [--band N] [--search] [--quiet]\n");
        return 64;
    }
    name = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--al-mac") && i + 1 < argc) {
            if (parse_mac(argv[++i], controller)) {
                fprintf(stderr, "enroller: bad --al-mac\n");
                return 64;
            }
        } else if (!strcmp(argv[i], "--band") && i + 1 < argc) {
            band = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "--search")) {
            want_search = 1;
        } else if (argv[i][0] != '-') {
            seconds = (unsigned int)atoi(argv[i]);
        }
    }
    if (interface_mac(name, iface_mac))
        memset(iface_mac, 0, sizeof(iface_mac));
    if (!controller[0] && !controller[1] && !controller[5])
        memcpy(controller, iface_mac, 6);   /* no spoof: answer as this interface */

    promiscuous(name, 1, &was_promiscuous);
    /* ETH_P_ALL, not 0x893a: a protocol-bound packet socket is delivered *after* the
     * bridge's rx_handler, so on a bridge port it never sees frames that arrive from
     * the wire (the bridge either forwards them or hands them to the bridge device).
     * tcpdump gets away with it because ETH_P_ALL taps run before bridging. Filtering
     * by ethertype is therefore done here, not by the kernel. */
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (fd < 0) {
        promiscuous(name, was_promiscuous, NULL);
        fprintf(stderr, "enroller: socket failed errno=%d\n", errno);
        return 1;
    }
    {
        struct sockaddr_ll address;
        memset(&address, 0, sizeof(address));
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = (int)if_nametoindex(name);
        if (!address.sll_ifindex || bind(fd, (struct sockaddr *)&address, sizeof(address))) {
            fprintf(stderr, "enroller: bind %s failed errno=%d\n", name, errno);
            close(fd);
            promiscuous(name, was_promiscuous, NULL);
            return 1;
        }
        index = address.sll_ifindex;
    }

    printf("enroller iface=%s controller=%02x:%02x:%02x:%02x:%02x:%02x iface_mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "seconds=%u\n", name, controller[0], controller[1], controller[2], controller[3], controller[4],
           controller[5], iface_mac[0], iface_mac[1], iface_mac[2], iface_mac[3], iface_mac[4], iface_mac[5],
           seconds);
    fflush(NULL);

    if (want_search) {
        uint8_t frame[128];
        unsigned int bytes = search_frame(frame, controller, band, mid++);
        status = send_frame(fd, index, frame, bytes, group);
        printf("enroller search band=%u bytes=%u status=%d\n", band, bytes, status);
        fflush(NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct pollfd watch = { .fd = fd, .events = POLLIN };
        long elapsed;
        ssize_t bytes;
        if (poll(&watch, 1, 500) > 0 && (watch.revents & POLLIN)) {
            bytes = recv(fd, buffer, sizeof(buffer), 0);
            if (bytes > FRAME_BYTES + 4 && buffer[12] == 0x89 && buffer[13] == 0x3a) {
                unsigned int cmdu = ((unsigned int)buffer[16] << 8) | buffer[17];
                unsigned int id = ((unsigned int)buffer[18] << 8) | buffer[19];
                heard++;
                if (!quiet) {
                    printf("enroller rx cmdu=0x%04x mid=0x%04x from=%02x:%02x:%02x:%02x:%02x:%02x bytes=%u\n", cmdu,
                           id, buffer[6], buffer[7], buffer[8], buffer[9], buffer[10], buffer[11], (unsigned)bytes);
                    list_tlvs(buffer, (unsigned int)bytes);
                    fflush(NULL);
                }
                if (cmdu == 0x0007) {
                    /* The agent is asking to be enrolled. Answer as its controller: the agent's M1 then goes to
                     * this frame's source address, which is why --al-mac exists. */
                    uint8_t reply[128];
                    int found = 0;
                    unsigned int answer_band = tlv_value(buffer, (unsigned int)bytes, 0x0e, &found);
                    unsigned int answer = response_frame(reply, buffer + 6, controller, found && answer_band < 2 ? answer_band : band, id);
                    status = send_frame(fd, index, reply, answer, buffer + 6);
                    replies++;
                    printf("enroller reply band=%u mid=0x%04x bytes=%u status=%d\n",
                           found && answer_band < 2 ? answer_band : band, id, answer, status);
                    fflush(NULL);
                }
            }
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            break;
        elapsed = (long)(now.tv_sec - start.tv_sec);
        if (elapsed >= (long)seconds)
            break;
    }
    close(fd);
    promiscuous(name, was_promiscuous, NULL);
    printf("enroller done heard=%u replies=%u\n", heard, replies);
    return 0;
}
