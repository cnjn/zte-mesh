/* Host tests for the 1905 decoder: the layouts it implements were checked against the Wireshark ieee1905
 * dissector (tshark 3.6.2) on constructed frames, and the vendor container against the 61 vendor TLVs of a
 * 107-frame capture from the lab. The fixture in captured_tests() is one of those frames verbatim, so a change
 * that only satisfies the constructed ones still has to survive real bytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller-1905.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "1905 codec failure line %d\n", __LINE__); exit(1); } } while (0)

static const uint8_t agent[6] = {2, 0, 0, 0, 0x19, 0xe3};
static const uint8_t controller[6] = {2, 0, 0, 0, 0x19, 0x03};

/* One captured frame: CMDU 0x0006 Link Metric Response from 02:00:00:00:19:e3, message id 0xcffe, 260 bytes.
 * Its Vendor Specific TLV (0x0b, 232 bytes) holds two 0x49 sub-TLVs of 110 bytes each; that shape is what the
 * vendor walk implements. */
static const char *captured_hex =
    "0200000019030200000019e3893a00000006cffe00800b00e80019c602000949"
    "006e0200000019e3000000000000000000000000000000000000000000000000"
    "0000000000000000000400000000080000000000000000080000000000000000"
    "0800000000000000000800000000000000000200000400000000040000000004"
    "0000000004000000000400000000010049006e02000000190300000000000000"
    "0000000000000000000000000000000000000000000000000000040000000008"
    "0000000000000000080000000000000000080000000000000000080000000000"
    "0000000200000400000000040000000004000000000400000000040000000001"
    "00000000";

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

/* Ethernet + CMDU header; callers append the TLVs. */
static size_t frame_open(uint8_t *frame, unsigned int type, unsigned int mid, uint8_t flags)
{
    memset(frame, 0, CONTROLLER_1905_MAX_BYTES);
    memcpy(frame, controller, 6);
    memcpy(frame + 6, agent, 6);
    frame[12] = 0x89;
    frame[13] = 0x3a;
    frame[16] = (uint8_t)(type >> 8);
    frame[17] = (uint8_t)type;
    frame[18] = (uint8_t)(mid >> 8);
    frame[19] = (uint8_t)mid;
    frame[21] = flags;
    return CONTROLLER_1905_HEADER;
}

static size_t put_header(uint8_t *frame, size_t at, unsigned int type, size_t length)
{
    frame[at] = (uint8_t)type;
    frame[at + 1] = (uint8_t)(length >> 8);
    frame[at + 2] = (uint8_t)length;
    return at + 3;
}

static size_t put_tlv(uint8_t *frame, size_t at, unsigned int type, const uint8_t *value, size_t length)
{
    at = put_header(frame, at, type, length);
    if (value && length)
        memcpy(frame + at, value, length);
    return at + length;
}

static size_t put_eom(uint8_t *frame, size_t at)
{
    return put_header(frame, at, 0, 0);
}

/* AP Operational BSS payload as the dissector lays it out: radio count, then per radio
 * {radio identifier[6], interface count}, then per interface {BSSID[6], SSID length, SSID}. Radios and
 * interfaces carry one-byte identifiers so a truncated copy is still a valid prefix. */
static size_t build_ap_bss(uint8_t *out, unsigned int radios, unsigned int interfaces, const uint8_t *bssid)
{
    size_t at = 0;
    unsigned int r, i;
    out[at++] = (uint8_t)radios;
    for (r = 0; r < radios; r++) {
        memcpy(out + at, "\x02MAP24", 6);
        at += 6;
        out[at++] = (uint8_t)interfaces;
        for (i = 0; i < interfaces; i++) {
            memcpy(out + at, bssid, 6);
            at += 6;
            out[at++] = 3;
            memcpy(out + at, "abc", 3);
            at += 3;
        }
    }
    return at;
}

static void header_tests(void)
{
    struct cmdu_header header;
    struct node_metrics metrics;
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    size_t length = frame_open(frame, 3, 0x1202, 0x80);

    CHECK(cmdu_decode(frame, length, &header));
    CHECK(header.type == 3 && header.message_id == 0x1202 && header.fragment == 0);
    CHECK(header.last_fragment == 1 && header.relayed == 0);
    CHECK(node_metrics_decode(frame, length, &metrics));
    CHECK(metrics.hops == 1 && metrics.fields == NODE_METRIC_HOPS);

    /* A relayed frame is one level further out, and that is all the header can say about depth. */
    frame[21] = 0xc0;
    CHECK(cmdu_decode(frame, length, &header) && header.relayed == 1 && header.last_fragment == 1);
    CHECK(node_metrics_decode(frame, length, &metrics) && metrics.hops == 2 && metrics.fields == NODE_METRIC_HOPS);
    frame[21] = 0x80;

    /* Every reason to reject a frame, one at a time. */
    CHECK(!cmdu_decode(NULL, length, &header));
    CHECK(!cmdu_decode(frame, length, NULL));
    CHECK(!cmdu_decode(frame, CONTROLLER_1905_HEADER - 1, &header));
    CHECK(!cmdu_decode(frame, CONTROLLER_1905_MAX_BYTES + 1, &header));
    frame[12] = 0x88; CHECK(!cmdu_decode(frame, length, &header)); frame[12] = 0x89;
    frame[13] = 0x3b; CHECK(!cmdu_decode(frame, length, &header)); frame[13] = 0x3a;
    frame[14] = 1; CHECK(!cmdu_decode(frame, length, &header)); frame[14] = 0;
    frame[15] = 1; CHECK(!cmdu_decode(frame, length, &header)); frame[15] = 0;
    CHECK(!node_metrics_decode(frame, 0, &metrics));
    CHECK(metrics.hops == -1 && metrics.fields == 0); /* a rejected frame leaves "unknown", not zero */
    CHECK(!node_metrics_decode(frame, length, NULL));
}

static void tlv_walk_tests(void)
{
    struct tlv_cursor cursor;
    unsigned int type;
    const uint8_t *value;
    size_t length, at;
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    static const uint8_t payload[4] = {1, 2, 3, 4};

    at = frame_open(frame, 3, 1, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AL_MAC, agent, 6);
    at = put_tlv(frame, at, 0x55, payload, 0);
    at = put_eom(frame, at);
    tlv_open(&cursor, frame, at);
    CHECK(tlv_next(&cursor, &type, &value, &length) && type == CONTROLLER_TLV_AL_MAC && length == 6 && !memcmp(value, agent, 6));
    CHECK(tlv_next(&cursor, &type, &value, &length) && type == 0x55 && length == 0);
    CHECK(!tlv_next(&cursor, &type, &value, &length)); /* End of Message ends the walk */
    CHECK(!tlv_next(&cursor, &type, &value, &length)); /* and stays ended */

    /* A TLV whose length runs past the frame yields everything before it and nothing after: padding after the
     * EOM is normal (Ethernet minimum frame size), a lying length is not. */
    at = frame_open(frame, 3, 1, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AL_MAC, agent, 6);
    put_header(frame, at, CONTROLLER_TLV_AP_METRICS, 0xffff);
    tlv_open(&cursor, frame, at + 3);
    CHECK(tlv_next(&cursor, &type, &value, &length) && type == CONTROLLER_TLV_AL_MAC);
    CHECK(!tlv_next(&cursor, &type, &value, &length));

    /* Nothing at all after the header. */
    at = frame_open(frame, 3, 1, 0x80);
    tlv_open(&cursor, frame, at);
    CHECK(!tlv_next(&cursor, &type, &value, &length));
}

static void metrics_tests(void)
{
    struct node_metrics metrics;
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    static const uint8_t bssid_a[6] = {2, 0, 0, 0, 0x19, 0xe4};
    static const uint8_t bssid_b[6] = {2, 0, 0, 0, 0x19, 0xe6};
    uint8_t ap_metrics[9], bss[96];
    size_t at, bss_length, declared;

    /* Two AP Metrics TLVs in one frame are summed: 3 + 2 stations. */
    memset(ap_metrics, 0, sizeof(ap_metrics));
    memcpy(ap_metrics, bssid_a, 6);
    ap_metrics[6] = 40; /* channel utilisation */
    ap_metrics[8] = 3;  /* big-endian u16 station count */
    at = frame_open(frame, 0x800c, 7, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, ap_metrics, 9);
    memcpy(ap_metrics, bssid_b, 6);
    ap_metrics[8] = 2;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, ap_metrics, 9);
    at = put_eom(frame, at);
    CHECK(node_metrics_decode(frame, at, &metrics));
    CHECK(metrics.stations == 5 && (metrics.fields & NODE_METRIC_STATIONS));
    CHECK(metrics.hops == 1 && !(metrics.fields & NODE_METRIC_BSS));

    /* Eight bytes is not an AP Metrics TLV (BSSID, utilisation and the u16 count are nine). */
    at = frame_open(frame, 0x800c, 7, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, ap_metrics, 8);
    at = put_eom(frame, at);
    CHECK(node_metrics_decode(frame, at, &metrics) && !(metrics.fields & NODE_METRIC_STATIONS));

    /* Two radios, one interface on the first and two on the second: three BSS. */
    bss_length = build_ap_bss(bss, 2, 1, bssid_a);
    at = frame_open(frame, 3, 9, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_OPERATIONAL_BSS, bss, bss_length);
    at = put_eom(frame, at);
    CHECK(node_metrics_decode(frame, at, &metrics));
    CHECK(metrics.bss == 2 && (metrics.fields & NODE_METRIC_BSS));
    CHECK(metrics.stations == 0 && !(metrics.fields & NODE_METRIC_STATIONS));
    CHECK(metrics.hops == 1);

    bss_length = build_ap_bss(bss, 1, 3, bssid_b);
    at = frame_open(frame, 3, 9, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_OPERATIONAL_BSS, bss, bss_length);
    at = put_eom(frame, at);
    CHECK(node_metrics_decode(frame, at, &metrics) && metrics.bss == 3);

    /* The same TLV cut short: the interfaces that no longer fit are not counted, and nothing past the frame is
     * read. 14 bytes is one complete interface, 20 is one and a half. */
    for (declared = 1; declared <= 20; declared++) {
        at = frame_open(frame, 3, 9, 0x80);
        at = put_header(frame, at, CONTROLLER_TLV_AP_OPERATIONAL_BSS, declared);
        memcpy(frame + at, bss, declared);
        at += declared;
        at = put_eom(frame, at);
        CHECK(node_metrics_decode(frame, at, &metrics));
        CHECK(metrics.bss <= 1);
    }

    /* A radio count far past the payload must not walk off it. */
    memset(bss, 0, sizeof(bss));
    bss[0] = 200;
    at = frame_open(frame, 3, 9, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_OPERATIONAL_BSS, bss, 12);
    at = put_eom(frame, at);
    CHECK(node_metrics_decode(frame, at, &metrics) && metrics.bss == 0);
}

static void bss_list_tests(void)
{
    struct ap_bss_metric list[CONTROLLER_AP_BSS_MAX];
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    static const uint8_t bssid_a[6] = {2, 0, 0, 0, 0x19, 0xe4};
    static const uint8_t bssid_b[6] = {2, 0, 0, 0, 0x19, 0xe6};
    static const uint8_t bssid_c[6] = {2, 0, 0, 0, 0x19, 0xe7};
    uint8_t body[24];
    size_t at;
    unsigned int k;

    /* Two BSSes in one frame: two entries, in the order the frame lists them, with the counts the frame carries.
     * node_metrics_decode() adds those two TLVs into one number; this is the other half of it. */
    at = frame_open(frame, 0x800c, 30, 0x80);
    memset(body, 0, sizeof(body));
    memcpy(body, bssid_a, 6);
    body[6] = 40;
    body[8] = 3;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    memcpy(body, bssid_b, 6);
    body[8] = 2;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 2);
    CHECK(!memcmp(list[0].bssid, bssid_a, 6) && list[0].stations == 3);
    CHECK(!memcmp(list[1].bssid, bssid_b, 6) && list[1].stations == 2);

    /* Bytes after the fixed nine are the optional estimated-service parameters: another field, and they do not
     * shift the count. */
    at = frame_open(frame, 0x800c, 31, 0x80);
    memset(body, 0, sizeof(body));
    memcpy(body, bssid_a, 6);
    body[6] = 11;
    body[8] = 7;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 20);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 1);
    CHECK(list[0].stations == 7);

    /* A BSS with nobody on it is an entry with zero in it, not a missing entry - the page has to tell an idle
     * radio from one that never answered. */
    at = frame_open(frame, 0x800c, 32, 0x80);
    memset(body, 0, sizeof(body));
    memcpy(body, bssid_a, 6);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 1);
    CHECK(list[0].stations == 0);

    /* Nothing to list: no AP Metrics TLV at all, and one that stops before the count. */
    at = frame_open(frame, 3, 33, 0x80);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 0);
    at = frame_open(frame, 0x800c, 34, 0x80);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 8);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 0);

    /* A frame that ends in the middle of a count keeps the entries before the damage: the walk stops where the
     * bytes stop. */
    at = frame_open(frame, 0x800c, 35, 0x80);
    memcpy(body, bssid_a, 6);
    body[8] = 3;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    memcpy(body, bssid_b, 6);
    at = put_header(frame, at, CONTROLLER_TLV_AP_METRICS, 9);
    memcpy(frame + at, body, 4);
    at += 4;
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 1);
    CHECK(!memcmp(list[0].bssid, bssid_a, 6));

    /* One BSS counted twice is not one number we can pick, so the whole list goes back and the caller uses the
     * frame's own total instead of a sum that would be too small. */
    at = frame_open(frame, 0x800c, 36, 0x80);
    memcpy(body, bssid_a, 6);
    body[8] = 4;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    body[8] = 5;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX) == 0);

    /* A list the caller cannot hold whole is no list either: three BSSes asked of a two-slot array come back as
     * nothing, while the same frame does list when the array fits. */
    at = frame_open(frame, 0x800c, 37, 0x80);
    memcpy(body, bssid_a, 6);
    body[8] = 1;
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    memcpy(body, bssid_b, 6);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    memcpy(body, bssid_c, 6);
    at = put_tlv(frame, at, CONTROLLER_TLV_AP_METRICS, body, 9);
    at = put_eom(frame, at);
    CHECK(ap_metrics_by_bss(frame, at, list, 2) == 0);
    CHECK(ap_metrics_by_bss(frame, at, list, 3) == 3);
    for (k = 0; k < 3; k++)
        CHECK(list[k].stations == 1);

    /* A caller that asks for more slots than the decoder ever fills, for none, or for no array at all gets
     * nothing: the limit is the array it handed over, not a number written here. */
    CHECK(ap_metrics_by_bss(frame, at, list, 0) == 0);
    CHECK(ap_metrics_by_bss(frame, at, list, CONTROLLER_AP_BSS_MAX + 1) == 0);
    CHECK(ap_metrics_by_bss(frame, at, NULL, CONTROLLER_AP_BSS_MAX) == 0);
}

static void vendor_tests(void)
{
    struct zte_cursor cursor;
    unsigned int type;
    const uint8_t *value;
    size_t length, at, payload;
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    uint8_t vendor[32];

    /* OUI 00:19:c6, three header bytes, two sub-TLVs {type[1], length[2], value}. */
    memset(vendor, 0, sizeof(vendor));
    vendor[0] = 0x00; vendor[1] = 0x19; vendor[2] = 0xc6;
    vendor[3] = 0x02; vendor[4] = 0x00; vendor[5] = 0x09;
    vendor[6] = 0x21; vendor[7] = 0x00; vendor[8] = 0x07;
    memcpy(vendor + 9, agent, 6);
    vendor[15] = 0x01;
    vendor[16] = 0x40; vendor[17] = 0x00; vendor[18] = 0x02;
    vendor[19] = 0xaa; vendor[20] = 0xbb;

    at = frame_open(frame, 0x000e, 3, 0x80);
    at = put_header(frame, at, CONTROLLER_TLV_VENDOR, 21);
    payload = at;
    memcpy(frame + payload, vendor, 21);
    at = payload + 21;
    at = put_eom(frame, at);

    CHECK(zte_vendor_open(&cursor, frame + payload, 21));
    CHECK(zte_vendor_next(&cursor, &type, &value, &length) && type == 0x21 && length == 7 && !memcmp(value, agent, 6));
    CHECK(zte_vendor_next(&cursor, &type, &value, &length) && type == 0x40 && length == 2 && value[1] == 0xbb);
    CHECK(!zte_vendor_next(&cursor, &type, &value, &length));
    CHECK(!zte_vendor_next(&cursor, &type, &value, &length)); /* stays ended */

    /* Another vendor's OUI is not parsed as ZTE's; a sub-TLV that overruns ends the walk. The bytes under test
     * are the ones in the frame, so they are patched there rather than in the template. */
    frame[payload + 2] = 0xc7;
    CHECK(!zte_vendor_open(&cursor, frame + payload, 21));
    frame[payload + 2] = 0xc6;
    frame[payload + 7] = 0x00;
    frame[payload + 8] = 0x40; /* claims 64 bytes, 12 remain */
    CHECK(zte_vendor_open(&cursor, frame + payload, 21));
    CHECK(!zte_vendor_next(&cursor, &type, &value, &length));
    CHECK(!zte_vendor_open(&cursor, NULL, 21));
    CHECK(!zte_vendor_open(&cursor, frame + payload, 8)); /* shorter than OUI + header */
}

static void inventory_tests(void)
{
    char out[CONTROLLER_INVENTORY_TEXT + 1];
    uint8_t body[256];
    size_t at, length;

    /* Device Inventory: [length][text] three times in wire order - serial, software, hardware - and only the
     * second one is wanted. The count byte and the per-radio records after it are never reached. */
    at = 0;
    body[at++] = 5; memcpy(body + at, "SN123", 5); at += 5;
    body[at++] = 15; memcpy(body + at, "V1.0.0.2B2.8000", 15); at += 15;
    body[at++] = 9; memcpy(body + at, "ZXHN Z506", 9); at += 9;
    body[at++] = 0; /* zero radios */
    CHECK(device_inventory_software(body, at, out, sizeof(out)));
    CHECK(!strcmp(out, "V1.0.0.2B2.8000"));

    /* Nothing at all is recorded when the frame stops short, and the buffer says so instead of keeping the
     * previous answer. */
    out[0] = 'x'; out[1] = 0;
    CHECK(!device_inventory_software(body, 6, out, sizeof(out))); /* only the serial fits */
    CHECK(!out[0]);
    out[0] = 'x'; out[1] = 0;
    CHECK(!device_inventory_software(body, 7, out, sizeof(out))); /* the length byte, no text after it */
    CHECK(!out[0]);
    out[0] = 'x'; out[1] = 0;
    CHECK(!device_inventory_software(body, 12, out, sizeof(out))); /* half of the version */
    CHECK(!out[0]);
    CHECK(!device_inventory_software(body, 0, out, sizeof(out)));
    CHECK(!device_inventory_software(NULL, at, out, sizeof(out)));
    CHECK(!device_inventory_software(body, at, out, 1)); /* a buffer that cannot hold a string */

    /* An empty field is "not reported", a control byte is not a version, and both leave "" behind. */
    at = 0;
    body[at++] = 1; body[at++] = 'A';
    body[at++] = 0;
    CHECK(!device_inventory_software(body, at, out, sizeof(out)));
    CHECK(!out[0]);
    at = 0;
    body[at++] = 1; body[at++] = 'A';
    body[at++] = 3; body[at++] = 'V'; body[at++] = 0; body[at++] = '1';
    CHECK(!device_inventory_software(body, at, out, sizeof(out)));
    CHECK(!out[0]);

    /* 64 bytes is the longest field the factory receiver copies into its char array, and 65 is refused the same
     * way it refuses one: nothing is recorded, and the bytes after it are not the version. */
    memset(body, 'v', sizeof(body));
    body[0] = 1;
    body[1] = 'A';
    body[2] = CONTROLLER_INVENTORY_TEXT;
    length = 3 + CONTROLLER_INVENTORY_TEXT;
    body[length++] = 3;
    body[length++] = 'V';
    body[length++] = '9';
    body[length++] = '9';
    body[length++] = 0;
    CHECK(device_inventory_software(body, length, out, sizeof(out)));
    CHECK(strlen(out) == CONTROLLER_INVENTORY_TEXT);
    body[2] = CONTROLLER_INVENTORY_TEXT + 1;
    CHECK(!device_inventory_software(body, length, out, sizeof(out)));
    CHECK(!out[0]);

    /* A version longer than the caller's buffer is copied as far as it fits: a truncated string on the page is
     * still the node's own version, and nothing outside the buffer is touched. */
    at = 0;
    body[at++] = 1; body[at++] = 'A';
    body[at++] = 15; memcpy(body + at, "V1.0.0.2B2.8000", 15); at += 15;
    memset(out, 0x5a, sizeof(out));
    CHECK(device_inventory_software(body, at, out, 8));
    CHECK(!strcmp(out, "V1.0.0."));
    CHECK(out[8] == 0x5a);

    /* Every cut of a complete inventory, with the bytes after it fenced off: the answer is the whole version or
     * nothing, and a decoder that runs past the cut shows up as a broken fence. */
    at = 0;
    body[at++] = 5; memcpy(body + at, "SN123", 5); at += 5;
    body[at++] = 15; memcpy(body + at, "V1.0.0.2B2.8000", 15); at += 15;
    body[at++] = 9; memcpy(body + at, "ZXHN Z506", 9); at += 9;
    body[at++] = 0;
    length = at;
    for (at = 0; at <= length; at++) {
        int found;
        memset(body + length, 0xa5, 8);
        found = device_inventory_software(body, at, out, sizeof(out));
        CHECK(!found || !strcmp(out, "V1.0.0.2B2.8000"));
        CHECK(body[length] == 0xa5);
    }
    CHECK(device_inventory_software(body, length, out, sizeof(out))); /* the last cut is the whole thing */
}

static void captured_tests(void)
{
    static uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    struct cmdu_header header;
    struct node_metrics metrics;
    struct tlv_cursor cursor;
    struct zte_cursor zte;
    unsigned int type, entries = 0;
    const uint8_t *value;
    size_t length, bytes = unhex(captured_hex, frame, sizeof(frame));

    CHECK(bytes == 260);
    CHECK(cmdu_decode(frame, bytes, &header));
    CHECK(header.type == 6 && header.message_id == 0xcffe);
    CHECK(header.last_fragment == 1 && header.relayed == 0);
    CHECK(!memcmp(frame + 6, agent, 6));

    /* A Link Metric Response carries no station or BSS counts: only the hop count is known, and an unknown
     * count must stay "not reported" instead of becoming zero stations. */
    CHECK(node_metrics_decode(frame, bytes, &metrics));
    CHECK(metrics.fields == NODE_METRIC_HOPS && metrics.hops == 1);
    CHECK(metrics.stations == 0 && metrics.bss == 0);

    /* The captured vendor TLV: OUI, three header bytes, two 0x49 sub-TLVs of 110 bytes each, consuming the
     * 232-byte payload exactly. Entry 0 describes the sender, entry 1 the router it answers. */
    tlv_open(&cursor, frame, bytes);
    CHECK(tlv_next(&cursor, &type, &value, &length) && type == CONTROLLER_TLV_VENDOR && length == 232);
    CHECK(zte_vendor_open(&zte, value, length));
    while (zte_vendor_next(&zte, &type, &value, &length)) {
        CHECK(type == 0x49 && length == 110);
        CHECK(!memcmp(value, entries ? controller : agent, 6));
        entries++;
    }
    CHECK(entries == 2);
    CHECK(!tlv_next(&cursor, &type, &value, &length)); /* the capture ends with EOM and Ethernet padding */
}

/* The master's half of the station-count exchange: the query this repository builds is the one its own decoder
 * reads, in the shape the vendor's I5TlvApMetricsQueryInsert writes - and with no BSSID listed, which is how the
 * question says "every access point you run". */
static void query_build_tests(void)
{
    uint8_t frame[CONTROLLER_1905_MAX_BYTES], runt[59];
    struct cmdu_header header;
    uint8_t asked[CONTROLLER_AP_BSS_MAX][6];
    unsigned int listed = 7, k;
    size_t used = ap_metrics_query_build(frame, sizeof(frame), agent, controller, 0x4100);
    CHECK(used == 60); /* the padding is part of what goes on the wire: a raw socket does not add it */
    CHECK(!memcmp(frame, agent, 6) && !memcmp(frame + 6, controller, 6));
    CHECK(frame[12] == 0x89 && frame[13] == 0x3a);
    CHECK(frame[14] == 0 && frame[15] == 0); /* revision and reserved */
    CHECK(frame[16] == 0x80 && frame[17] == 0x0b);
    CHECK(frame[18] == 0x41 && frame[19] == 0x00);
    CHECK(frame[20] == 0 && frame[21] == 0x80); /* last fragment, not relayed: it is unicast to one agent */
    CHECK(frame[22] == CONTROLLER_TLV_AP_METRICS_QUERY && frame[23] == 0 && frame[24] == 1);
    CHECK(frame[25] == 0); /* the count byte, and it counts nothing */
    CHECK(frame[26] == 0 && frame[27] == 0 && frame[28] == 0); /* End of Message */
    for (k = 29; k < 60; k++)
        CHECK(frame[k] == 0);
    CHECK(ap_metrics_query_decode(frame, used, &header, asked, &listed));
    CHECK(header.type == CONTROLLER_MSG_AP_METRICS_QUERY && header.message_id == 0x4100);
    CHECK(header.fragment == 0 && header.last_fragment && !header.relayed);
    CHECK(listed == 0);
    used = ap_metrics_query_build(frame, sizeof(frame), agent, controller, 0);
    CHECK(used == 60 && frame[18] == 0 && frame[19] == 0); /* id 0 is a legal id, not a refusal */
    CHECK(ap_metrics_query_build(runt, sizeof(runt), agent, controller, 1) == 0);
    CHECK(ap_metrics_query_build(frame, sizeof(frame), NULL, controller, 1) == 0);
    CHECK(ap_metrics_query_build(frame, sizeof(frame), agent, NULL, 1) == 0);
}

int main(void)
{
    static uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    uint32_t seed = 0x19051203;
    unsigned int i;

    header_tests();
    tlv_walk_tests();
    metrics_tests();
    query_build_tests();
    bss_list_tests();
    vendor_tests();
    inventory_tests();
    captured_tests();

    /* Deterministic bounded mutations: take the captured frame, flip one bit and cut it at a random length.
     * The eight guard bytes after it must survive, which is what catches a decoder that reads past the end. */
    for (i = 0; i < 100000; i++) {
        struct node_metrics metrics;
        size_t bytes = unhex(captured_hex, frame, sizeof(frame) - 8), cut;
        memset(frame + bytes, 0xa5, 8);
        seed = seed * 1664525u + 1013904223u;
        frame[seed % bytes] ^= (uint8_t)(1u << ((seed >> 24) & 7));
        seed = seed * 1664525u + 1013904223u;
        cut = seed % (bytes + 1);
        if (node_metrics_decode(frame, cut, &metrics) && metrics.hops != 1 && metrics.hops != 2)
            return 1;
        if (frame[bytes] != 0xa5)
            return 1;
    }
    printf("controller-1905-codec: %u assertions; 100000 bounded mutations passed\n", checks);
    return 0;
}
