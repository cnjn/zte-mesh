/* Bounded PUBLIC 1905 fixtures and header-only peer validation in private netns.
 * No host link, credential/config TLV logging, or synthetic daemon responses.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "controller-packet.h"

static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
static unsigned int be16(const uint8_t *p) { return ((unsigned int)p[0] << 8) | p[1]; }
/* TLV type:length list of a frame - no values, so it stays safe to log next to a WSC payload. Off unless
 * ZTE_MESH_TRACE_TLV=1; WSC messages (CMDU 0x0009) always get it, because which TLVs an M1/M2 carries is the
 * one thing that has to be compared by hand while that exchange is being matched. */
static void trace_tlvs(const char *what, const uint8_t *frame, size_t bytes)
{
    char line[320];
    size_t used = 0, at = 22;
    if (!getenv("ZTE_MESH_TRACE_TLV") && be16(frame + 16) != 9)
        return;
    line[0] = 0;
    while (at + 3 <= bytes && frame[at] && used < sizeof(line) - 24) {
        size_t length = be16(frame + at + 1);
        if (at + 3 + length > bytes)
            break;
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%02x:%u", used ? "," : "", frame[at],
                                 (unsigned int)length);
        at += 3 + length;
    }
    printf("ZTE_PACKET_TLV %s cmdu=0x%04x bytes=%u tlvs=%s\n", what, be16(frame + 16), (unsigned int)bytes, line);
    fflush(NULL);
}
int controller_topology_response_matches(const uint8_t *frame, size_t bytes, const uint8_t controller[6])
{
    size_t at = 22, n;
    unsigned int device_info = 0;
    if (!frame || !controller || bytes < 25 || bytes > 1600 ||
        memcmp(frame, peer, 6) || memcmp(frame + 6, controller, 6) ||
        be16(frame + 12) != 0x893a || frame[14] || frame[15] ||
        be16(frame + 16) != 3 || be16(frame + 18) != 0x1202 ||
        frame[20] || frame[21] != 0x80)
        return 0;
    while (bytes - at >= 3) {
        unsigned int type = frame[at];
        n = be16(frame + at + 1); at += 3;
        if (n > bytes - at) return 0;
        if (!type) {
            if (n || device_info != 1) return 0;
            /* Ethernet padding may follow the EOM, but not another message. */
            for (; at < bytes; at++) if (frame[at]) return 0;
            return 1;
        }
        if (type == 3) {
            size_t p = 7;
            unsigned int i;
            if (++device_info != 1 || n < 7 || memcmp(frame + at, controller, 6)) return 0;
            for (i = 0; i < frame[at + 6]; i++) {
                unsigned int media_bytes;
                if (n - p < 9) return 0;
                media_bytes = frame[at + p + 8]; p += 9;
                if (media_bytes > n - p) return 0;
                p += media_bytes;
            }
            if (p != n) return 0;
        }
        at += n;
    }
    return 0;
}
int controller_autoconfig_response_matches(const uint8_t *frame, size_t bytes, const uint8_t controller[6], unsigned int band)
{
    size_t at = 22, n;
    unsigned int role = 0, frequency = 0, service = 0;
    if (!frame || !controller || band > 1 || bytes < 25 || bytes > 1600 ||
        memcmp(frame, peer, 6) || memcmp(frame + 6, controller, 6) ||
        be16(frame + 12) != 0x893a || frame[14] || frame[15] ||
        be16(frame + 16) != 8 || be16(frame + 18) != 0x1203 + band ||
        frame[20] || frame[21] != 0x80) return 0;
    while (bytes - at >= 3) {
        unsigned int type = frame[at];
        n = be16(frame + at + 1); at += 3;
        if (n > bytes - at) return 0;
        if (!type) {
            if (n || role != 1 || frequency != 1 || service != 1) return 0;
            for (; at < bytes; at++) if (frame[at]) return 0;
            return 1;
        }
        if (type == 0x0f && (n != 1 || frame[at] || ++role != 1)) return 0;
        if (type == 0x10 && (n != 1 || frame[at] != band || ++frequency != 1)) return 0;
        if (type == 0x80) {
            size_t i;
            if (!n || frame[at] != n - 1) return 0;
            for (i = 1; i < n; i++) if (!frame[at + i]) service++;
        }
        at += n;
    }
    return 0;
}
static int wait_response(int fd, const uint8_t controller[6], int query)
{
    struct timespec start, now;
    struct pollfd ready = { .fd = fd, .events = POLLIN };
    uint8_t frame[1601];
    unsigned int attempts;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    for (attempts = 0; attempts < 512; attempts++) {
        long elapsed;
        ssize_t bytes;
        int status;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= 4000) break;
        status = poll(&ready, 1, (int)(4000 - elapsed));
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0 || !(ready.revents & POLLIN)) break;
        bytes = recv(fd, frame, sizeof(frame), MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (bytes < 0) break;
        if ((query == 1 && controller_topology_response_matches(frame, (size_t)bytes, controller)) ||
            (query >= 2 && controller_autoconfig_response_matches(frame, (size_t)bytes, controller, (unsigned int)query - 2))) {
            trace_tlvs(query == 1 ? "topology-response" : "autoconfig-response", frame, (size_t)bytes);
            if (query == 1)
                printf("ZTE_CONTROLLER_PACKET fixture=topology-response private_only=1 peer_received=1 cmdu=3 mid_match=1 tlv_sane=1 device_info=1 bytes=%u body_logged=0\n", (unsigned int)bytes);
            else
                printf("ZTE_CONTROLLER_PACKET fixture=autoconfig-response private_only=1 peer_received=1 cmdu=8 band=%u mid_match=1 controller_service=1 bytes=%u body_logged=0\n", (unsigned int)query - 2, (unsigned int)bytes);
            memset(frame, 0, sizeof(frame));
            return 0;
        }
    }
    memset(frame, 0, sizeof(frame));
    errno = ETIMEDOUT; return -1;
}
int controller_inject_packet(int query)
{
    static const uint8_t group[6] = {1, 0x80, 0xc2, 0, 0, 0x13};
    struct ifreq interface;
    uint8_t controller[6] = {0};
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    uint8_t frame[60] = {0};
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    int fd, saved, result;
    if (!private || strcmp(private, "1") || (query < 0 || query > 3)) return -1;
    target.sll_ifindex = (int)if_nametoindex("lab-peer");
    if (!target.sll_ifindex) return -1;
    if (query) {
        memset(&interface, 0, sizeof(interface)); strcpy(interface.ifr_name, "br0");
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        result = ioctl(fd, SIOCGIFHWADDR, &interface); saved = errno;
        close(fd); errno = saved;
        if (result || interface.ifr_hwaddr.sa_family != 1) return -1;
        memcpy(controller, interface.ifr_hwaddr.sa_data, 6);
    }
    memcpy(frame, query ? controller : group, 6); memcpy(frame + 6, peer, 6);
    frame[12] = 0x89; frame[13] = 0x3a;
    frame[17] = query ? 2 : 0;
    frame[18] = 0x12; frame[19] = query ? 2 : 1; frame[21] = 0x80;
    if (!query) {
        frame[22] = 1; frame[24] = 6; memcpy(frame + 25, peer, 6);
        frame[31] = 2; frame[33] = 6; memcpy(frame + 34, peer, 6);
    }
    if (query >= 2) {
        static const uint8_t search[] = {
            0x0d, 0, 1, 0, /* Searched role: registrar. */
            0x0e, 0, 1, 0, /* Band overwritten below. */
            0x80, 0, 2, 1, 1, /* Supported service: MAP Agent. */
            0x81, 0, 2, 1, 0, /* Searched service: MAP Controller. */
            0xb3, 0, 1, 1 /* MultiAP profile 1; no proprietary inputs. */
        };
        memset(frame, 0, sizeof(frame));
        memcpy(frame, group, 6); memcpy(frame + 6, peer, 6);
        frame[12] = 0x89; frame[13] = 0x3a; frame[17] = 7;
        frame[18] = 0x12; frame[19] = (uint8_t)(query + 1); frame[21] = 0xc0;
        frame[22] = 1; frame[24] = 6; memcpy(frame + 25, peer, 6);
        memcpy(frame + 31, search, sizeof(search)); frame[38] = (uint8_t)(query - 2);
    }
    memcpy(target.sll_addr, frame, 6);
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(0x893a));
    if (fd < 0) return -1;
    /* Listen before sending; the daemon may respond immediately. */
    if (bind(fd, (struct sockaddr *)&target, sizeof(target))) { saved = errno; close(fd); errno = saved; return -1; }
    result = sendto(fd, frame, sizeof(frame), 0, (struct sockaddr *)&target, sizeof(target)) == (ssize_t)sizeof(frame) ? 0 : -1;
    if (!result && query) result = wait_response(fd, controller, query);
    saved = errno; close(fd); errno = saved;
    return result;
}

/* Listen on the private wire. Open this BEFORE triggering the daemon: it may
 * answer immediately and a late listener would miss the frame. */
int controller_cmdu_listener(void)
{
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    int fd, saved;
    if (!private || strcmp(private, "1")) return -1;
    target.sll_ifindex = (int)if_nametoindex("lab-peer");
    if (!target.sll_ifindex) return -1;
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(0x893a));
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&target, sizeof(target))) { saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}

/* Wait for any frame carrying the given 1905 CMDU type; only the type is
 * inspected, nothing from the frame is kept or logged. Closes the listener. */
int controller_wait_cmdu(int fd, unsigned int type, unsigned int timeout_ms)
{
    struct pollfd ready;
    uint8_t frame[1600];
    struct timespec start, now;
    int result = -1, saved;
    if (fd < 0 || clock_gettime(CLOCK_MONOTONIC, &start)) goto done;
    ready.fd = fd; ready.events = POLLIN;
    for (;;) {
        long elapsed;
        ssize_t bytes;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto done;
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= (long)timeout_ms) { errno = ETIMEDOUT; goto done; }
        if (poll(&ready, 1, (int)((long)timeout_ms - elapsed)) <= 0) { errno = ETIMEDOUT; goto done; }
        bytes = recv(fd, frame, sizeof(frame), MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (bytes < 0) goto done;
        if (bytes >= 22 && frame[12] == 0x89 && frame[13] == 0x3a &&
            (((unsigned int)frame[16] << 8) | frame[17]) == type) { result = 0; goto done; }
    }
done:
    saved = errno; if (fd >= 0) close(fd); memset(frame, 0, sizeof(frame)); errno = saved;
    return result;
}

/* ---- Channel Selection Request (see controller-packet.h) ---- */
static uint8_t opclass_20mhz(unsigned int radio, int channel)
{
    if (radio == 0)
        return channel == 14 ? 82 : 81;
    if (channel <= 48) return 115;
    if (channel <= 64) return 118;
    if (channel <= 144) return 121;
    if (channel <= 161) return 124;
    return 125;
}

int controller_radio_select(const uint8_t agent[6], unsigned int radio, int channel, int power, int *code)
{
    /* The fixed radio identifiers a ZTE agent uses ("MAP24G"/"MAP5GL"; seen in its own
     * AP-autoconfig and operating-channel reports and in `sendcmd multiapd.map.slave dev`). */
    static const uint8_t radio_id[2][6] = { {'M', 'A', 'P', '2', '4', 'G'}, {'M', 'A', 'P', '5', 'G', 'L'} };
    static uint16_t message_id = 0xe000;
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    struct ifreq interface;
    uint8_t frame[96], reply[1600], source[6];
    struct timespec start, now;
    size_t used = 22;
    int fd = -1, result = -1, saved, probe;
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    uint16_t mid;
    if (!agent || !code || radio > 1 || !private || strcmp(private, "1") || channel < -1 || channel > 165 ||
        power < -1 || power > 30 || (channel < 0 && power < 0))
        return -1;
    *code = -1;
    target.sll_ifindex = (int)if_nametoindex("veth-lab"); /* the daemon's actual port */
    if (!target.sll_ifindex)
        return -1;
    memset(&interface, 0, sizeof(interface)); strcpy(interface.ifr_name, "br0");
    probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return -1;
    saved = ioctl(probe, SIOCGIFHWADDR, &interface); close(probe);
    if (saved || interface.ifr_hwaddr.sa_family != 1) return -1;
    memcpy(source, interface.ifr_hwaddr.sa_data, 6);
    mid = message_id++;
    memset(frame, 0, sizeof(frame));
    memcpy(frame, agent, 6); memcpy(frame + 6, source, 6);
    frame[12] = 0x89; frame[13] = 0x3a;
    frame[16] = 0x80; frame[17] = 0x06;            /* CMDU 0x8006 */
    frame[18] = (uint8_t)(mid >> 8); frame[19] = (uint8_t)mid;
    frame[21] = 0x80;                              /* last fragment */
    if (channel >= 0) {                            /* Channel Preference TLV */
        frame[used++] = 0x8b; frame[used++] = 0; frame[used++] = 11;
        memcpy(frame + used, radio_id[radio], 6); used += 6;
        frame[used++] = 1;                         /* one operating class */
        frame[used++] = opclass_20mhz(radio, channel);
        frame[used++] = 1;                         /* with exactly one channel */
        frame[used++] = (uint8_t)channel;
        frame[used++] = 14 << 4;                   /* preference 14, reason 0 */
    }
    if (power >= 0) {                              /* Transmit Power Limit TLV */
        frame[used++] = 0x8d; frame[used++] = 0; frame[used++] = 7;
        memcpy(frame + used, radio_id[radio], 6); used += 6;
        frame[used++] = (uint8_t)power;
    }
    frame[used++] = 0; frame[used++] = 0; frame[used++] = 0; /* End of Message */
    if (used < 60) used = 60;
    memcpy(target.sll_addr, agent, 6);
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(0x893a));
    if (fd < 0) return -1;
    /* Listen before sending: the agent answers within milliseconds. */
    if (bind(fd, (struct sockaddr *)&target, sizeof(target))) goto done;
    if (sendto(fd, frame, used, 0, (struct sockaddr *)&target, sizeof(target)) != (ssize_t)used) goto done;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) goto done;
    for (;;) {
        struct pollfd ready = { .fd = fd, .events = POLLIN };
        long elapsed;
        ssize_t bytes;
        size_t at;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto done;
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= 3000) { result = -2; goto done; }
        if (poll(&ready, 1, (int)(3000 - elapsed)) <= 0) { result = -2; goto done; }
        bytes = recv(fd, reply, sizeof(reply), MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (bytes < 25) { if (bytes < 0) goto done; continue; }
        if (memcmp(reply + 6, agent, 6) || reply[12] != 0x89 || reply[13] != 0x3a ||
            be16(reply + 16) != 0x8007 || be16(reply + 18) != mid)
            continue;
        for (at = 22; at + 3 <= (size_t)bytes;) {
            size_t length = be16(reply + at + 1);
            if (reply[at] == 0 || at + 3 + length > (size_t)bytes) break;
            if (reply[at] == 0x8e && length >= 7 && !memcmp(reply + at + 3, radio_id[radio], 6)) {
                *code = reply[at + 9];
                result = 0;
                goto done;
            }
            at += 3 + length;
        }
        *code = -1; result = 0; goto done; /* a response without our radio's status */
    }
done:
    saved = errno;
    if (fd >= 0) close(fd);
    memset(reply, 0, sizeof(reply));
    errno = saved;
    return result;
}

/* The daemon's own frames, header and WSC message type only. */
int controller_read_frame(int fd, struct controller_frame *frame)
{
    uint8_t buffer[1600];
    ssize_t bytes;
    size_t at;
    if (fd < 0 || !frame)
        return -1;
    memset(frame, 0, sizeof(*frame));
    bytes = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (bytes < 0)
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (bytes < 25 || buffer[12] != 0x89 || buffer[13] != 0x3a)
        return 0;
    trace_tlvs("daemon", buffer, (size_t)bytes);
    memcpy(frame->src, buffer + 6, 6);
    frame->cmdu = be16(buffer + 16);
    frame->mid = be16(buffer + 18);
    frame->bytes = (unsigned int)bytes;
    for (at = 22; at + 3 <= (size_t)bytes && buffer[at];) {
        size_t length = be16(buffer + at + 1);
        if (at + 3 + length > (size_t)bytes)
            break;
        if (buffer[at] == 0x11) {
            size_t p = 0;
            if (length <= sizeof(frame->wsc)) {
                memcpy(frame->wsc, buffer + at + 3, length);
                frame->wsc_bytes = length;
            }
            while (length - p >= 4) {
                unsigned int tag = be16(buffer + at + 3 + p), body = be16(buffer + at + 5 + p);
                p += 4;
                if (body > length - p)
                    break;
                if (tag == 0x1022 && body == 1)
                    frame->wsc_message = buffer[at + 3 + p];
                p += body;
            }
        }
        if (buffer[at] == 0x0e && length == 1)
            frame->band = buffer[at + 3]; /* Autoconfiguration Frequency Band */
        if (buffer[at] == 0x80 && length >= 1) {
            frame->service_count = buffer[at + 3]; /* entry count, then one value per entry */
            frame->service_first = length >= 2 ? buffer[at + 4] : 0;
        }
        /* AP Radio Basic Capabilities. Its payload opens with the Radio Unique Identifier, which is what the
         * M2 has to name back as its AP Radio Identifier; acdGetApRadioBasicCapabilitiesType() reads exactly
         * these six bytes (memcpy(out, payload, 6)) before the operating-class list. */
        if (buffer[at] == 0x85 && length >= 6) {
            memcpy(frame->radio, buffer + at + 3, 6);
            frame->radio_bytes = 6;
        }
        at += 3 + length;
    }
    memset(buffer, 0, sizeof(buffer));
    return 1;
}

/* AP Autoconfiguration Response, built the way the factory controller builds it (the shape the
 * controller_autoconfig_response_matches() validator was recovered against). */
int controller_autoconfig_response(const uint8_t agent[6], unsigned int band, unsigned int mid)
{
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    uint8_t frame[64];
    size_t used = 22;
    int fd, result, saved;
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    if (!agent || band > 1 || mid > 0xffff || !private || strcmp(private, "1"))
        return -1;
    target.sll_ifindex = (int)if_nametoindex("lab-peer");
    if (!target.sll_ifindex)
        return -1;
    memset(frame, 0, sizeof(frame));
    memcpy(frame, agent, 6);
    memcpy(frame + 6, peer, 6);
    frame[12] = 0x89; frame[13] = 0x3a;
    frame[16] = 0x00; frame[17] = 0x08;              /* CMDU 0x0008 */
    frame[18] = (uint8_t)(mid >> 8); frame[19] = (uint8_t)mid;
    frame[21] = 0x80;                                /* last fragment */
    frame[used++] = 0x0f; frame[used++] = 0; frame[used++] = 1; frame[used++] = 0;      /* Searched role: registrar */
    frame[used++] = 0x10; frame[used++] = 0; frame[used++] = 1; frame[used++] = (uint8_t)band; /* frequency band */
    /* Supported service: one entry, value 0. That value is a role, not a bit count - the agent declares 1
     * for itself, so 0 is the Controller entry the factory response carries (and the shape
     * controller_autoconfig_response_matches() was recovered from). */
    frame[used++] = 0x80; frame[used++] = 0; frame[used++] = 2; frame[used++] = 1; frame[used++] = 0;
    memcpy(target.sll_addr, agent, 6);
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x893a));
    if (fd < 0)
        return -1;
    result = sendto(fd, frame, used, 0, (struct sockaddr *)&target, sizeof(target)) == (ssize_t)used ? 0 : -1;
    saved = errno;
    close(fd);
    errno = saved;
    return result;
}

/* The agent's Topology Discovery (CMDU 0x0000) asks every other 1905 device to identify itself. A Multi-AP
 * Controller answers with a Topology Query (CMDU 0x0002); the Supported Service TLV inside it is what the
 * agent records, and until that record exists it drops the Autoconfiguration Response that follows.
 * `service` selects the encoding of that TLV (I5TlvSupportedServiceTypeInsert fills it per role):
 *   0 = one entry, value 0 (controller only)
 *   1 = one entry, value 1 (agent only)
 *   2 = two entries, values 0 and 1 (controller and agent) - what the factory writes for the combined role */
int controller_topology_query(const uint8_t agent[6], unsigned int mid, unsigned int service)
{
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    uint8_t frame[64];
    size_t used = 22;
    int fd, result, saved;
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    if (!agent || mid > 0xffff || service > 0xff || !private || strcmp(private, "1"))
        return -1;
    target.sll_ifindex = (int)if_nametoindex("lab-peer");
    if (!target.sll_ifindex)
        return -1;
    memset(frame, 0, sizeof(frame));
    memcpy(frame, agent, 6);
    memcpy(frame + 6, peer, 6);
    frame[12] = 0x89; frame[13] = 0x3a;
    frame[16] = 0x00; frame[17] = 0x02;              /* CMDU 0x0002 */
    frame[18] = (uint8_t)(mid >> 8); frame[19] = (uint8_t)mid;
    frame[21] = 0x80;                                /* last fragment */
    frame[used++] = 0x01; frame[used++] = 0; frame[used++] = 6; memcpy(frame + used, peer, 6); used += 6; /* AL MAC */
    frame[used++] = 0x02; frame[used++] = 0; frame[used++] = 6; memcpy(frame + used, peer, 6); used += 6; /* MAC address */
    frame[used++] = 0x80;
    if (service > 1) {                       /* two entries: controller and agent */
        frame[used++] = 0; frame[used++] = 3;
        frame[used++] = 2; frame[used++] = 0; frame[used++] = 1;
    } else {                                 /* one entry, value = service */
        frame[used++] = 0; frame[used++] = 2;
        frame[used++] = 1; frame[used++] = (uint8_t)service;
    }
    memcpy(target.sll_addr, agent, 6);
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x893a));
    if (fd < 0)
        return -1;
    result = sendto(fd, frame, used, 0, (struct sockaddr *)&target, sizeof(target)) == (ssize_t)used ? 0 : -1;
    saved = errno;
    close(fd);
    errno = saved;
    return result;
}
