#define _POSIX_C_SOURCE 200809L
#include "mesh-bridge.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

void mb_wipe(void *memory, size_t length)
{
    volatile uint8_t *p = memory;
    while (length--)
        *p++ = 0;
}

static int ascii_slot(const char *p, unsigned int used, unsigned int total)
{
    unsigned int i;
    if (used >= total)
        return -1;
    for (i = 0; i < used; i++)
        if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126)
            return -1;
    for (; i < total; i++)
        if (p[i])
            return -1;
    return 0;
}

int mb_validate(const struct mb_profile *p)
{
    return !p || !p->request_id || p->radio > 1 || p->bss != 0 ||
        p->security != MB_WPA2_PSK_AES || p->enabled > 1 ||
        p->ssid_length < 1 || p->ssid_length > MB_MAX_SSID ||
        p->key_length < 8 || p->key_length > MB_MAX_KEY ||
        ascii_slot(p->ssid, p->ssid_length, sizeof(p->ssid)) ||
        ascii_slot(p->key, p->key_length, sizeof(p->key)) ? MB_BAD_CONFIG : MB_OK;
}

int mb_derive_name(const struct mb_profile *base, const char *suffix, struct mb_profile *out)
{
    size_t suffix_length = suffix ? strlen(suffix) : 0;
    if (!base || !out || suffix_length < 1 || suffix_length > 8 || base->ssid_length < 1 ||
        base->ssid_length > MB_MAX_SSID || base->ssid_length + suffix_length > MB_MAX_SSID ||
        ascii_slot(suffix, suffix_length, suffix_length + 1))
        return MB_BAD_CONFIG;
    memset(out->ssid, 0, sizeof(out->ssid));
    memcpy(out->ssid, base->ssid, base->ssid_length);
    memcpy(out->ssid + base->ssid_length, suffix, suffix_length);
    out->ssid_length = (uint8_t)(base->ssid_length + suffix_length);
    return MB_OK;
}

int mb_encode_request(const struct mb_profile *p, uint8_t *wire, size_t size)
{
    if (!wire || size != MB_REQUEST_SIZE)
        return MB_BAD_CONFIG;
    mb_wipe(wire, size);
    if (mb_validate(p))
        return MB_BAD_CONFIG;
    memcpy(wire, "ZMB1", 4);
    put16(wire + 4, 1);
    put16(wire + 6, 1); /* apply logical profile */
    put32(wire + 8, MB_REQUEST_SIZE);
    put32(wire + 12, p->request_id);
    wire[16] = p->radio; wire[17] = p->bss; wire[18] = p->security;
    wire[19] = p->enabled; wire[20] = p->ssid_length; wire[21] = p->key_length;
    memcpy(wire + 24, p->ssid, sizeof(p->ssid));
    memcpy(wire + 56, p->key, sizeof(p->key));
    return MB_OK;
}

int mb_decode_request(const uint8_t *wire, size_t size, struct mb_profile *p)
{
    int status;
    if (!p)
        return MB_BAD_FRAME;
    mb_wipe(p, sizeof(*p));
    if (!wire || size != MB_REQUEST_SIZE || memcmp(wire, "ZMB1", 4) ||
        get16(wire + 4) != 1 || get16(wire + 6) != 1 ||
        get32(wire + 8) != MB_REQUEST_SIZE || get16(wire + 22))
        return MB_BAD_FRAME;
    p->request_id = get32(wire + 12);
    p->radio = wire[16]; p->bss = wire[17]; p->security = wire[18];
    p->enabled = wire[19]; p->ssid_length = wire[20]; p->key_length = wire[21];
    memcpy(p->ssid, wire + 24, sizeof(p->ssid));
    memcpy(p->key, wire + 56, sizeof(p->key));
    status = mb_validate(p);
    if (status)
        mb_wipe(p, sizeof(*p));
    return status;
}

static int valid_result(const struct mb_result *r)
{
    if (!r || r->status > MB_TRANSPORT_ERROR || (r->flags & ~15U))
        return 0;
    if (r->status == MB_OK)
        return r->request_id && r->generation == 1 && r->flags == (MB_READBACK | MB_OTHER_ROW);
    if (r->generation)
        return 0;
    if (r->status == MB_BAD_FRAME || r->status == MB_BAD_CONFIG || r->status == MB_TRANSPORT_ERROR)
        return r->flags == MB_REJECTED;
    if (r->status == MB_ROLLBACK_ERROR)
        return r->flags == 0;
    return r->flags == MB_REJECTED || r->flags == MB_ROLLED_BACK;
}

int mb_encode_response(const struct mb_result *r, uint8_t *wire, size_t size)
{
    if (!wire || size != MB_RESPONSE_SIZE || !valid_result(r))
        return MB_BAD_FRAME;
    memset(wire, 0, size);
    memcpy(wire, "ZMR1", 4); put16(wire + 4, 1); put16(wire + 6, r->status);
    put32(wire + 8, MB_RESPONSE_SIZE); put32(wire + 12, r->request_id);
    put32(wire + 16, r->generation); put32(wire + 20, r->flags);
    return MB_OK;
}

int mb_decode_response(const uint8_t *wire, size_t size, struct mb_result *r)
{
    if (!r)
        return MB_BAD_FRAME;
    memset(r, 0, sizeof(*r));
    if (!wire || size != MB_RESPONSE_SIZE || memcmp(wire, "ZMR1", 4) ||
        get16(wire + 4) != 1 || get32(wire + 8) != MB_RESPONSE_SIZE)
        return MB_BAD_FRAME;
    r->status = get16(wire + 6); r->request_id = get32(wire + 12);
    r->generation = get32(wire + 16); r->flags = get32(wire + 20);
    if (!valid_result(r)) {
        memset(r, 0, sizeof(*r));
        return MB_BAD_FRAME;
    }
    return MB_OK;
}

static int64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ready(int fd, short event, int64_t deadline)
{
    struct pollfd p = {fd, event, 0};
    int64_t now;
    int n;
    for (;;) {
        now = milliseconds();
        if (now < 0 || deadline <= now)
            return -1;
        n = poll(&p, 1, (int)(deadline - now));
        if (n < 0 && errno == EINTR)
            continue;
        return n > 0 && !(p.revents & (POLLERR | POLLNVAL)) &&
            (p.revents & (event | POLLHUP)) ? 0 : -1;
    }
}

int mb_read_message(int fd, uint8_t *p, size_t size, int timeout)
{
    size_t done = 0;
    uint8_t extra;
    int64_t start = milliseconds(), deadline = start + timeout;
    if (!p || !size || size > MB_REQUEST_SIZE)
        return -1;
    if (start < 0 || timeout < 1 || timeout > 20000)
        goto failed;
    while (done < size) {
        ssize_t n;
        if (ready(fd, POLLIN, deadline))
            goto failed;
        n = read(fd, p + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto failed;
        done += n;
    }
    /* One-shot protocol: mandatory EOF rejects trailing bytes/second frames. */
    if (ready(fd, POLLIN, deadline) || read(fd, &extra, 1) != 0)
        goto failed;
    return 0;
failed:
    mb_wipe(p, size);
    return -1;
}

int mb_write_message(int fd, const uint8_t *p, size_t size, int timeout)
{
    size_t done = 0;
    int64_t start = milliseconds(), deadline = start + timeout;
    if (!p || !size || size > MB_REQUEST_SIZE || start < 0 || timeout < 1 || timeout > 20000)
        return -1;
    while (done < size) {
        ssize_t n;
        if (ready(fd, POLLOUT, deadline))
            return -1;
        n = write(fd, p + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}
