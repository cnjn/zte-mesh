/* Host tests for the client-steering codec.
 *
 * The two halves are checked differently on purpose. Encoding is compared against a frame written out byte by
 * byte, because the message is short enough that the whole frame can be read in one line; that literal was
 * itself confirmed by feeding the same bytes to the Wireshark ieee1905 dissector (tshark 4.4.2) and reading
 * every field back (tests/test_steer_tshark.sh does that again on every run, and docs/research/STEER_1905.md
 * records what the dissector settled about the field order).
 *
 * Decoding is fed frames the dissector also accepts, then mutated: whatever an agent sends back, a decode must
 * stay inside the buffer it was given.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller-1905.h"
#include "../src/controller-steer.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "steer codec failure line %d\n", __LINE__); exit(1); } } while (0)

static const uint8_t agent[6] = {2, 0, 0, 0, 0x19, 0xe3};
static const uint8_t controller[6] = {2, 0, 0, 0, 0x19, 0x06};
static const uint8_t bssid[6] = {0x0c, 0x0e, 0x5a, 0x1b, 0x2c, 0x3d};
static const uint8_t client[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const uint8_t target[6] = {0x0c, 0x0e, 0x5a, 0x1b, 0x2c, 0x44};

static size_t unhex(const char *text, uint8_t *out, size_t room)
{
    size_t i, n = 0;
    for (i = 0; text[i] && text[i + 1] && n < room; i += 2) {
        unsigned int byte;
        if (sscanf(text + i, "%2x", &byte) != 1)
            break;
        out[n++] = (uint8_t)byte;
    }
    return n;
}

static void expect_hex(const uint8_t *frame, size_t bytes, const char *want, int line)
{
    size_t i;
    checks++;
    for (i = 0; i < bytes; i++) {
        unsigned int byte;
        if (sscanf(want + 2 * i, "%2x", &byte) != 1 || frame[i] != (uint8_t)byte) {
            fprintf(stderr, "steer codec: byte %u is %02x, expected %s (line %d)\n", (unsigned)i, frame[i],
                    want, line);
            exit(1);
        }
    }
    if (want[2 * bytes]) {
        fprintf(stderr, "steer codec: frame is %u bytes, expected more (line %d)\n", (unsigned)bytes, line);
        exit(1);
    }
}
#define EXPECT_HEX(frame, bytes, want) expect_hex(frame, bytes, want, __LINE__)

static struct steer_request plain_request(void)
{
    struct steer_request request;
    memset(&request, 0, sizeof(request));
    memcpy(request.al_mac, controller, 6);
    memcpy(request.bssid, bssid, 6);
    memcpy(request.client, client, 6);
    request.window = 200;
    request.disassoc_timer = 100;
    request.mandate = 1;
    request.disassoc_imminent = 1;
    return request;
}

/* --- 0x8014 ------------------------------------------------------------------------------------------- */

/* The frame a kick produces: unicast to the agent, client info naming the BSS the phone is on, a mandate with
 * disassociation imminent, and no target BSS (the agent picks the band). 71 bytes. */
static void request_bytes(void)
{
    static const char *want =
        "0200000019e3" "020000001906" "893a" "0000" "8014" "0001" "00" "80"
        "010006020000001906"
        "90000c" "0c0e5a1b2c3d" "aabbccddeeff"
        "9b0013" "0c0e5a1b2c3d" "c0" "00c8" "0064" "01" "aabbccddeeff" "00"
        "000000";
    uint8_t frame[STEER_FRAME_MAX];
    struct steer_request request = plain_request();
    size_t bytes = steer_build_request(frame, sizeof(frame), 1, agent, &request);

    CHECK(bytes == 71);
    EXPECT_HEX(frame, bytes, want);
}

/* One target BSS: the steering request TLV grows by the MAC, the operating class and the channel. */
static void request_with_target(void)
{
    static const char *want =
        "0200000019e3" "020000001906" "893a" "0000" "8014" "0001" "00" "80"
        "010006020000001906"
        "90000c" "0c0e5a1b2c3d" "aabbccddeeff"
        "9b001b" "0c0e5a1b2c3d" "c0" "00c8" "0064" "01" "aabbccddeeff" "01"
        "0c0e5a1b2c44" "51" "18"
        "000000";
    uint8_t frame[STEER_FRAME_MAX];
    struct steer_request request = plain_request();
    size_t bytes;

    request.target_count = 1;
    memcpy(request.target_bssid, target, 6);
    request.target_class = 81;
    request.target_channel = 24;
    bytes = steer_build_request(frame, sizeof(frame), 1, agent, &request);
    CHECK(bytes == 79);
    EXPECT_HEX(frame, bytes, want);

    /* Flags: dropping both means "steering opportunity, no disassociation", which must show as a clear byte. */
    request.mandate = 0;
    request.disassoc_imminent = 0;
    request.abridged = 1;
    bytes = steer_build_request(frame, sizeof(frame), 0xffff, agent, &request);
    CHECK(bytes == 79);
    CHECK(frame[55] == 0x20);   /* the flags byte inside the steering request TLV */
    CHECK(frame[18] == 0xff && frame[19] == 0xff);
}

static void request_rejects(void)
{
    uint8_t frame[STEER_FRAME_MAX];
    struct steer_request request = plain_request();

    CHECK(steer_build_request(NULL, sizeof(frame), 1, agent, &request) == 0);
    CHECK(steer_build_request(frame, 70, 1, agent, &request) == 0);          /* one byte short */
    CHECK(steer_build_request(frame, sizeof(frame), 1, NULL, &request) == 0);
    CHECK(steer_build_request(frame, sizeof(frame), 0x10000, agent, &request) == 0);
    CHECK(steer_build_request(frame, sizeof(frame), 1, agent, NULL) == 0);
    request.target_count = 2;
    CHECK(steer_build_request(frame, sizeof(frame), 1, agent, &request) == 0);
    request.target_count = -1;
    CHECK(steer_build_request(frame, sizeof(frame), 1, agent, &request) == 0);

    /* The smallest buffer that works: exactly the frame length. */
    request = plain_request();
    CHECK(steer_build_request(frame, 71, 1, agent, &request) == 71);
}

/* --- 0x8016 ------------------------------------------------------------------------------------------- */

static void assoc_control_bytes(void)
{
    static const char *want =
        "0200000019e3" "020000001906" "893a" "0000" "8016" "0002" "00" "80"
        "010006020000001906"
        "9d0010" "0c0e5a1b2c3d" "00" "0000" "01" "aabbccddeeff"
        "000000";
    uint8_t frame[STEER_FRAME_MAX];
    struct steer_ban ban;
    size_t bytes;

    memset(&ban, 0, sizeof(ban));
    memcpy(ban.al_mac, controller, 6);
    memcpy(ban.bssid, bssid, 6);
    memcpy(ban.clients[0], client, 6);
    ban.count = 1;
    bytes = steer_build_assoc_control(frame, sizeof(frame), 2, agent, &ban);
    CHECK(bytes == 53);
    EXPECT_HEX(frame, bytes, want);

    /* Two clients and a one-hour ban: the TLV is 10 + 6 per client, validity is big-endian. Offsets from the
     * frame start: 22 header + 9 AL MAC TLV + 3 TLV header = 34 is the first body byte. */
    memcpy(ban.clients[1], target, 6);
    ban.count = 2;
    ban.validity_seconds = 3600;
    bytes = steer_build_assoc_control(frame, sizeof(frame), 2, agent, &ban);
    CHECK(bytes == 59);
    CHECK(frame[32] == 0x00 && frame[33] == 0x16);   /* TLV length 22 = 10 + 12 */
    CHECK(frame[41] == 0x0e && frame[42] == 0x10);   /* 0x0e10 = 3600 seconds */
    CHECK(frame[43] == 0x02);                        /* STA count */
    CHECK(frame[50] == 0x0c);                        /* second client's first byte */
    CHECK(frame[34 + 6] == 0x00);                    /* control 0 = block */
}

static void assoc_control_rejects(void)
{
    uint8_t frame[STEER_FRAME_MAX];
    struct steer_ban ban;

    memset(&ban, 0, sizeof(ban));
    memcpy(ban.al_mac, controller, 6);
    memcpy(ban.bssid, bssid, 6);
    CHECK(steer_build_assoc_control(frame, sizeof(frame), 1, agent, &ban) == 0);   /* no clients */
    ban.count = STEER_MAX_CLIENTS + 1;
    CHECK(steer_build_assoc_control(frame, sizeof(frame), 1, agent, &ban) == 0);
    ban.count = 1;
    CHECK(steer_build_assoc_control(frame, 52, 1, agent, &ban) == 0);
    CHECK(steer_build_assoc_control(NULL, sizeof(frame), 1, agent, &ban) == 0);
}

/* --- decoders ---------------------------------------------------------------------------------------- */

/* A 0x8015 BTM report from the agent and a 0x8017 Steering Completed, both in the bytes the dissector reads. */
static void decode_reports(void)
{
    static const char *btm_hex =
        "020000001906" "0200000019e3" "893a" "0000" "8015" "0007" "00" "80"
        "0100060200000019e3"
        "9c0013" "0c0e5a1b2c3d" "aabbccddeeff" "01" "0c0e5a1b2c44"
        "000000";
    static const char *done_hex =
        "020000001906" "0200000019e3" "893a" "0000" "8017" "0008" "00" "80"
        "0100060200000019e3"
        "90000c" "0c0e5a1b2c3d" "aabbccddeeff"
        "b80006" "0c0e5a1b2c44"
        "000000";
    /* A report that stops after the status byte: the target BSSID is the optional part, the rest is not. */
    static const char *short_hex =
        "020000001906" "0200000019e3" "893a" "0000" "8015" "0007" "00" "80"
        "0100060200000019e3"
        "9c000d" "0c0e5a1b2c3d" "aabbccddeeff" "01"
        "000000";
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    struct steer_btm_report report;
    struct steer_completed done;
    size_t bytes;

    bytes = unhex(btm_hex, frame, sizeof(frame));
    CHECK(steer_decode_btm_report(frame, bytes, &report) == 1);
    CHECK(!memcmp(report.bssid, bssid, 6));
    CHECK(!memcmp(report.client, client, 6));
    CHECK(report.status == 1);
    CHECK(report.has_target == 1 && !memcmp(report.target, target, 6));

    bytes = unhex(short_hex, frame, sizeof(frame));
    CHECK(bytes == 50);
    CHECK(steer_decode_btm_report(frame, bytes, &report) == 1);
    CHECK(report.status == 1 && report.has_target == 0);

    /* A steering request is not a report, and a report cut inside the TLV is not a message. */
    bytes = unhex(btm_hex, frame, sizeof(frame));
    CHECK(steer_decode_btm_report(frame, 40, &report) == 0);

    bytes = unhex(done_hex, frame, sizeof(frame));
    CHECK(steer_decode_completed(frame, bytes, &done) == 1);
    CHECK(!memcmp(done.client, client, 6));
    CHECK(done.has_bssid == 1 && !memcmp(done.bssid, target, 6));

    /* Without the BSSID TLV the client info one still names the source BSS. */
    bytes = unhex(done_hex, frame, sizeof(frame));
    CHECK(steer_decode_completed(frame, bytes - 12, &done) == 1);
    CHECK(done.has_bssid == 0 && !memcmp(done.bssid, bssid, 6));
    CHECK(steer_decode_completed(frame, bytes, NULL) == 0);
}

/* Whatever arrives, a decode either says no or fills the struct from bytes it was allowed to read. The guard
 * bytes after the frame catch a read that ran past the end. */
static void bounded_mutations(void)
{
    uint8_t buffer[CONTROLLER_1905_MAX_BYTES + 8];
    struct steer_btm_report report;
    struct steer_completed done;
    unsigned int seed = 12345;

    memset(buffer, 0xa5, sizeof(buffer));
    CHECK(unhex("0200000019060200000019e3893a000080150007008001000602000000" "19e39c00130c0e5a1b2c3daabbccddeeff010c0e5a1b2c44000000",
                buffer, sizeof(buffer) - 8) > 0);
    for (unsigned int i = 0; i < 100000; i++) {
        uint8_t copy[CONTROLLER_1905_MAX_BYTES + 8];
        size_t cut;
        seed = seed * 1103515245u + 12345u;
        memcpy(copy, buffer, sizeof(copy));
        copy[seed % 60] ^= (uint8_t)(1u << ((seed >> 16) & 7));
        cut = seed % (sizeof(buffer) - 7);
        if (steer_decode_btm_report(copy, cut, &report)) {
            CHECK(report.status < 256);
        }
        if (steer_decode_completed(copy, cut, &done)) {
            CHECK(done.has_bssid == 0 || done.has_bssid == 1);
        }
        CHECK(buffer[sizeof(buffer) - 8] == 0xa5 && copy[sizeof(copy) - 1] == 0xa5);
    }
}

/* --- optional pcap dump for the tshark cross-check --------------------------------------------------- */

static void put_le32(uint8_t *at, unsigned long value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void write_pcap(const char *path)
{
    static const char *btm_hex =
        "020000001906" "0200000019e3" "893a" "0000" "8015" "0007" "00" "80"
        "0100060200000019e3"
        "9c0013" "0c0e5a1b2c3d" "aabbccddeeff" "01" "0c0e5a1b2c44"
        "000000";
    uint8_t request[STEER_FRAME_MAX], ban_frame[STEER_FRAME_MAX], report[CONTROLLER_1905_MAX_BYTES];
    struct steer_request req = plain_request();
    struct steer_ban entry;
    size_t request_bytes, ban_bytes, report_bytes;
    uint8_t global[24], record[16];
    FILE *out;
    int i;

    req.target_count = 1;
    memcpy(req.target_bssid, target, 6);
    req.target_class = 81;
    req.target_channel = 24;
    request_bytes = steer_build_request(request, sizeof(request), 1, agent, &req);

    memset(&entry, 0, sizeof(entry));
    memcpy(entry.al_mac, controller, 6);
    memcpy(entry.bssid, bssid, 6);
    memcpy(entry.clients[0], client, 6);
    memcpy(entry.clients[1], target, 6);
    entry.count = 2;
    entry.validity_seconds = 3600;
    ban_bytes = steer_build_assoc_control(ban_frame, sizeof(ban_frame), 2, agent, &entry);
    report_bytes = unhex(btm_hex, report, sizeof(report));

    out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "steer codec: cannot write %s\n", path);
        exit(1);
    }
    put_le32(global, 0xa1b2c3d4ul);          /* magic, microseconds, little endian */
    global[4] = 2; global[5] = 0;            /* version 2.4 */
    global[6] = 4; global[7] = 0;
    put_le32(global + 8, 0);                 /* thiszone */
    put_le32(global + 12, 0);                /* sigfigs */
    put_le32(global + 16, 65535);            /* snaplen */
    put_le32(global + 20, 1);                /* Ethernet */
    fwrite(global, 1, sizeof(global), out);
    for (i = 0; i < 3; i++) {
        const uint8_t *frame = i == 0 ? request : i == 1 ? ban_frame : report;
        size_t bytes = i == 0 ? request_bytes : i == 1 ? ban_bytes : report_bytes;
        put_le32(record, 1700000000ul + (unsigned long)i);
        put_le32(record + 4, 0);
        put_le32(record + 8, (unsigned long)bytes);
        put_le32(record + 12, (unsigned long)bytes);
        fwrite(record, 1, sizeof(record), out);
        fwrite(frame, 1, bytes, out);
    }
    fclose(out);
}

int main(int argc, char **argv)
{
    request_bytes();
    request_with_target();
    request_rejects();
    assoc_control_bytes();
    assoc_control_rejects();
    decode_reports();
    bounded_mutations();
    if (argc > 1 && !strcmp(argv[1], "--pcap")) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --pcap FILE\n", argv[0]);
            return 2;
        }
        write_pcap(argv[2]);
    }
    printf("controller-steer-codec: %u assertions; 100000 bounded mutations passed\n", checks);
    return 0;
}
