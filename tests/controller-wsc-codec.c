/* Structure/nonce parser only: zero crypto material is NOT authentication. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller-packet.h"
static uint8_t frame[1601], saved[1601];
static const uint8_t controller[6] = {2, 0, 0, 0, 0x19, 6};
static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
static const uint8_t nonce[16] = {0x11};
static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "WSC parser failure line %d\n", __LINE__); exit(1); } } while (0)
static size_t attribute(size_t at, unsigned int type, const uint8_t *data, size_t bytes)
{
    frame[at] = (uint8_t)(type >> 8); frame[at + 1] = (uint8_t)type;
    frame[at + 2] = (uint8_t)(bytes >> 8); frame[at + 3] = (uint8_t)bytes;
    memcpy(frame + at + 4, data, bytes); return at + 4 + bytes;
}
int main(void)
{
    uint8_t zero[192] = {0}, message = 5, wrong_nonce[16] = {0x22};
    unsigned int i;
    size_t at = 25, bytes, n;
    uint32_t seed = 0x1905;
    memcpy(frame, peer, 6); memcpy(frame + 6, controller, 6);
    frame[12] = 0x89; frame[13] = 0x3a; frame[17] = 9; frame[21] = 0x80; frame[22] = 0x11;
    at = attribute(at, 0x1022, &message, 1);
    at = attribute(at, 0x101a, nonce, 16);
    at = attribute(at, 0x1039, zero, 16);
    at = attribute(at, 0x1032, zero, 192);
    at = attribute(at, 0x1018, zero, 32);
    at = attribute(at, 0x1005, zero, 8);
    n = at - 25; frame[23] = (uint8_t)(n >> 8); frame[24] = (uint8_t)n; bytes = at + 3;
    memcpy(saved, frame, sizeof(frame));
    CHECK(controller_wsc_m2_matches(frame, bytes, controller, nonce));
    CHECK(controller_wsc_m2_matches(frame, 1600, controller, nonce));
    CHECK(!controller_wsc_m2_matches(frame, 1601, controller, nonce));
    CHECK(!controller_wsc_m2_matches(NULL, bytes, controller, nonce));
    CHECK(!controller_wsc_m2_matches(frame, bytes, NULL, nonce));
    CHECK(!controller_wsc_m2_matches(frame, bytes, controller, NULL));
    CHECK(!controller_wsc_m2_matches(frame, bytes, controller, wrong_nonce));
    for (i = 0; i < bytes; i++) CHECK(!controller_wsc_m2_matches(frame, i, controller, nonce));
    frame[29] = 4; CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce)); frame[29] = 5;
    frame[23] = 0xff; CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce)); memcpy(frame, saved, sizeof(frame));
    frame[28] = 2; CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce)); memcpy(frame, saved, sizeof(frame));
    frame[bytes] = 1; CHECK(!controller_wsc_m2_matches(frame, 1600, controller, nonce)); memcpy(frame, saved, sizeof(frame));
    frame[at] = 1; CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce)); memcpy(frame, saved, sizeof(frame));
    frame[22] = 0x85; CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce)); memcpy(frame, saved, sizeof(frame));
    /* Missing/duplicate required attributes must not become a match. */
    frame[25] = 0x10; frame[26] = 0x39;
    CHECK(!controller_wsc_m2_matches(frame, bytes, controller, nonce));
    for (i = 0; i < 100000; i++) {
        size_t size, pos; int result;
        memcpy(frame, saved, sizeof(frame));
        seed = seed * 1664525u + 1013904223u; pos = seed % sizeof(frame);
        frame[pos] ^= (uint8_t)(1u << ((seed >> 24) & 7));
        seed = seed * 1664525u + 1013904223u; size = seed % 1602;
        result = controller_wsc_m2_matches(frame, size, controller, nonce);
        if (result != 0 && result != 1) return 1;
    }
    printf("controller-wsc-codec: %u assertions; 100000 bounded mutations; structure-only NOT authenticated\n", checks);
    return 0;
}
