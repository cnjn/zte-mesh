#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller-packet.h"
static const uint8_t controller[6] = {2, 0, 0, 0, 0x19, 6};
static uint8_t frame[1601], base[1601];
static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "packet codec failure line %d\n", __LINE__); exit(1); } } while (0)
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
