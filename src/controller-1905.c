/* 1905 CMDU decoding for the fields the topology page shows.
 *
 * Everything here is a pure function of one Ethernet frame: the daemon's receive hook, a host test and a
 * future offline replay of a pcap all get the same answer from the same bytes. No sockets, no vendor
 * library, no logging - the caller decides what to do with a value.
 */
#include <string.h>
#include "controller-1905.h"

#define ZTE_OUI_BYTES 3

static unsigned int be16(const uint8_t *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

int cmdu_decode(const uint8_t *frame, size_t bytes, struct cmdu_header *header)
{
    if (!frame || !header || bytes < CONTROLLER_1905_HEADER || bytes > CONTROLLER_1905_MAX_BYTES)
        return 0;
    if (be16(frame + 12) != CONTROLLER_1905_ETHER || frame[14] || frame[15])
        return 0;
    header->type = be16(frame + 16);
    header->message_id = be16(frame + 18);
    header->fragment = frame[20];
    header->last_fragment = !!(frame[21] & 0x80);
    header->relayed = !!(frame[21] & 0x40);
    return 1;
}

void tlv_open(struct tlv_cursor *cursor, const uint8_t *frame, size_t bytes)
{
    cursor->frame = frame;
    cursor->bytes = bytes;
    cursor->at = CONTROLLER_1905_HEADER;
    cursor->done = 0;
}

int tlv_next(struct tlv_cursor *cursor, unsigned int *type, const uint8_t **value, size_t *length)
{
    unsigned int t;
    size_t len;
    while (!cursor->done && cursor->frame && cursor->at + 3 <= cursor->bytes) {
        t = cursor->frame[cursor->at];
        len = be16(cursor->frame + cursor->at + 1);
        if (cursor->at + 3 + len > cursor->bytes) {
            cursor->done = 1;
            break;
        }
        cursor->at += 3;
        if (!t) {
            /* End of Message. Ethernet pads short frames after it; nothing after this is a TLV. */
            cursor->done = 1;
            break;
        }
        *type = t;
        *value = cursor->frame + cursor->at;
        *length = len;
        cursor->at += len;
        return 1;
    }
    cursor->done = 1;
    return 0;
}

int zte_vendor_open(struct zte_cursor *cursor, const uint8_t *value, size_t length)
{
    static const uint8_t oui[ZTE_OUI_BYTES] = {0x00, 0x19, 0xc6};
    if (!cursor || !value || length < 6 + 3 || memcmp(value, oui, ZTE_OUI_BYTES))
        return 0;
    cursor->value = value;
    cursor->bytes = length;
    cursor->at = 6;
    cursor->valid = 1;
    return 1;
}

int zte_vendor_next(struct zte_cursor *cursor, unsigned int *type, const uint8_t **value, size_t *length)
{
    unsigned int t;
    size_t len;
    if (!cursor || !cursor->valid)
        return 0;
    while (cursor->at + 3 <= cursor->bytes) {
        t = cursor->value[cursor->at];
        len = be16(cursor->value + cursor->at + 1);
        if (cursor->at + 3 + len > cursor->bytes) {
            cursor->valid = 0;
            break;
        }
        cursor->at += 3;
        if (!t) {
            cursor->valid = 0;
            break;
        }
        *type = t;
        *value = cursor->value + cursor->at;
        *length = len;
        cursor->at += len;
        return 1;
    }
    cursor->valid = 0;
    return 0;
}

/* AP Operational BSS (0x83): radio count, then per radio {radio identifier[6], interface count}, then per
 * interface {BSSID[6], SSID length, SSID}. Returns how many BSS interfaces fit; a frame that stops in the
 * middle of a radio yields the interfaces before it instead of a guess. */
static unsigned int ap_bss_interfaces(const uint8_t *value, size_t length)
{
    unsigned int radios, total = 0, r;
    size_t at = 0;
    if (length < 1)
        return 0;
    radios = value[at++];
    for (r = 0; r < radios; r++) {
        unsigned int interfaces, i;
        if (length - at < 7)
            return total;
        at += 6;
        interfaces = value[at++];
        for (i = 0; i < interfaces; i++) {
            size_t ssid;
            if (length - at < 7)
                return total;
            at += 6;
            ssid = value[at++];
            if (ssid > length - at)
                return total;
            at += ssid;
            total++;
        }
    }
    return total;
}

/* One [length(1), text] child of the inventory, at `at`. Returns the bytes it consumes, 0 when the TLV stops short
 * of it; `text` and `size` get the field when the caller asks for them. */
static size_t inventory_child(const uint8_t *value, size_t length, size_t at, const uint8_t **text, size_t *size)
{
    size_t len;
    if (at >= length)
        return 0;
    len = value[at];
    if (len + 1 > length - at)
        return 0;
    if (text)
        *text = value + at + 1;
    if (size)
        *size = len;
    return len + 1;
}

/* A version is text, and the factory field is a char array: a byte under ' ' - the zero that would cut the string
 * short included - is damage or a forged field, not something to put on the page. */
static int inventory_is_text(const uint8_t *text, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (text[i] < ' ' || text[i] == 0x7f)
            return 0;
    return 1;
}

int device_inventory_software(const uint8_t *value, size_t length, char *out, size_t out_size)
{
    const uint8_t *text;
    size_t len, at;
    if (!value || !out || out_size < 2)
        return 0;
    out[0] = 0;
    /* The serial number comes first, and the version cannot be located without stepping over it. A field longer
     * than the factory's 64-byte member is stepped over too, as its receiver steps over it, so the fields after
     * it keep their places. The hardware version that follows is not read: the walk stops where the answer is. */
    at = inventory_child(value, length, 0, NULL, NULL);
    if (!at || !inventory_child(value, length, at, &text, &len))
        return 0;
    if (!len || len > CONTROLLER_INVENTORY_TEXT || !inventory_is_text(text, len))
        return 0;
    {
        size_t n = len < out_size - 1 ? len : out_size - 1;
        memcpy(out, text, n);
        out[n] = 0;
    }
    return 1;
}

int node_metrics_decode(const uint8_t *frame, size_t bytes, struct node_metrics *metrics)
{
    struct cmdu_header header;
    struct tlv_cursor cursor;
    unsigned int type;
    const uint8_t *value;
    size_t length;
    if (!metrics)
        return 0;
    memset(metrics, 0, sizeof(*metrics));
    metrics->hops = -1;
    if (!cmdu_decode(frame, bytes, &header))
        return 0;
    /* The relay bit is in the 1905 header, so it is known for every frame: 1 = the sender is a direct
     * neighbor of this router, 2 = a neighbor forwarded it and the sender sits one level further out. */
    metrics->hops = header.relayed ? 2 : 1;
    metrics->fields = NODE_METRIC_HOPS;
    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &type, &value, &length)) {
        if (type == CONTROLLER_TLV_AP_METRICS && length >= 9) {
            /* BSSID[6], channel utilisation, u16 station count. One BSS per TLV is what the factory
             * daemon sends; optional estimated-service-parameter bytes may follow, so the fixed nine are
             * the only ones read. Several TLVs in one frame are summed. */
            metrics->stations += be16(value + 7);
            metrics->fields |= NODE_METRIC_STATIONS;
        } else if (type == CONTROLLER_TLV_AP_OPERATIONAL_BSS) {
            unsigned int count = ap_bss_interfaces(value, length);
            if (count) {
                metrics->bss += count;
                metrics->fields |= NODE_METRIC_BSS;
            }
        }
    }
    return 1;
}

unsigned int ap_metrics_by_bss(const uint8_t *frame, size_t bytes, struct ap_bss_metric *out, unsigned int max)
{
    struct ap_bss_metric seen[CONTROLLER_AP_BSS_MAX];
    struct tlv_cursor cursor;
    unsigned int type, count = 0, k;
    const uint8_t *value;
    size_t length;

    if (!out || !max || max > CONTROLLER_AP_BSS_MAX)
        return 0;
    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &type, &value, &length)) {
        if (type != CONTROLLER_TLV_AP_METRICS || length < 9)
            continue;
        /* The same BSS counted twice in one frame is not two BSSes and not one number we can pick: the frame is
         * on its own about that BSS, so the whole breakdown goes back to the frame's total. */
        for (k = 0; k < count; k++)
            if (!memcmp(seen[k].bssid, value, 6))
                return 0;
        if (count == max)
            return 0;
        memcpy(seen[count].bssid, value, 6);
        seen[count].stations = be16(value + 7);
        count++;
    }
    memcpy(out, seen, count * sizeof(*seen));
    return count;
}

int ap_metrics_query_decode(const uint8_t *frame, size_t bytes, struct cmdu_header *header,
                            uint8_t bssids[CONTROLLER_AP_BSS_MAX][6], unsigned int *count)
{
    struct tlv_cursor cursor;
    const uint8_t *value;
    unsigned int type, listed;
    size_t length, k;
    if (!count)
        return 0;
    *count = 0;
    if (!bssids || !cmdu_decode(frame, bytes, header) || header->type != CONTROLLER_MSG_AP_METRICS_QUERY ||
        header->fragment || !header->last_fragment)
        return 0;
    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &type, &value, &length)) {
        if (type != CONTROLLER_TLV_AP_METRICS_QUERY)
            continue;
        /* The count byte is the list's own length, so a TLV whose two disagree is damaged and the walk cannot say
         * where the BSSIDs end. A list longer than the per-node cap is one this answer would have to truncate, and
         * a truncated answer is the master believing it asked everything. Either way: silence. */
        if (length < 1)
            return 0;
        listed = value[0];
        if (length != 1 + 6 * (size_t)listed || listed > CONTROLLER_AP_BSS_MAX)
            return 0;
        for (k = 0; k < listed; k++)
            memcpy(bssids[k], value + 1 + 6 * k, 6);
        *count = listed;
        return 1;
    }
    return 0; /* a query with no 0x93 asks nothing */
}

/* The same question from the other end of the wire: AP Metric Query (CMDU 0x800b) whose 0x93 lists no BSSID, which
 * is the query saying "every BSS you run". `source` has to be this controller's AL MAC: the agent answers the
 * Ethernet source of the query, and the master files whatever comes back under that source. Padded to 60 because a
 * raw socket does not pad an Ethernet frame. */
size_t ap_metrics_query_build(uint8_t *frame, size_t capacity, const uint8_t dest[6], const uint8_t source[6],
                              unsigned int message_id)
{
    size_t needed = CONTROLLER_1905_HEADER + 3 + 1 + 3, used = CONTROLLER_1905_HEADER;
    if (needed < 60)
        needed = 60;
    if (!frame || !dest || !source || capacity < needed)
        return 0;
    memcpy(frame, dest, 6);
    memcpy(frame + 6, source, 6);
    frame[12] = (uint8_t)(CONTROLLER_1905_ETHER >> 8);
    frame[13] = (uint8_t)CONTROLLER_1905_ETHER;
    frame[14] = 0;                                    /* revision, reserved */
    frame[15] = 0;
    frame[16] = (uint8_t)(CONTROLLER_MSG_AP_METRICS_QUERY >> 8);
    frame[17] = (uint8_t)CONTROLLER_MSG_AP_METRICS_QUERY;
    frame[18] = (uint8_t)(message_id >> 8);
    frame[19] = (uint8_t)message_id;
    frame[20] = 0;                                    /* fragment id */
    frame[21] = 0x80;                                 /* last fragment, not relayed: it goes to one node */
    frame[used++] = CONTROLLER_TLV_AP_METRICS_QUERY;
    frame[used++] = 0;
    frame[used++] = 1;                                /* the value is the count byte alone */
    frame[used++] = 0;                                /* and it counts no BSSID: ask about all of them */
    frame[used++] = 0;
    frame[used++] = 0;
    frame[used++] = 0;                                /* End of Message */
    memset(frame + used, 0, 60 - used);
    return 60;
}

/* One AP Metrics TLV: BSSID[6], channel utilisation, station count, presence bitmap - the fixed part the factory
 * sender writes before its optional estimated-service blocks. The reader in this repository takes a TLV of nine
 * bytes, so the count is where it expects it; the presence byte is kept because the vendor's own layout carries it
 * and a frame shorter than that is one its extractor may read past. */
#define AP_METRICS_TLV_BYTES 13

size_t ap_metrics_response_build(uint8_t *frame, size_t capacity, const uint8_t dest[6], const uint8_t source[6],
                                 unsigned int message_id, const struct ap_bss_metric *bsses, unsigned int count)
{
    return ap_metrics_response_build_clients(frame, capacity, dest, source, message_id, bsses, count,
                                             NULL, 0, NULL, 0);
}

/* The vendor TLV of the client report, and the size of the value the groups describe: the count byte, every group
 * header, and every MAC. `0` when the groups do not fit the wire's own fields (an SSID longer than 32, a count the
 * one-byte header cannot carry); the caller then sends the counts alone. */
static size_t clients_value_bytes(const struct ap_client_group *groups, unsigned int group_count,
                                  unsigned int mac_count)
{
    size_t bytes = 1;
    unsigned int k;
    if (group_count > 255 || mac_count > CONTROLLER_CLIENTS_MAX)
        return 0;
    for (k = 0; k < group_count; k++) {
        size_t ssid = strlen(groups[k].ssid);
        if (ssid > CONTROLLER_SSID_MAX || groups[k].count > 255 ||
            groups[k].first > mac_count || groups[k].count > mac_count - groups[k].first)
            return 0;
        bytes += 6 + 1 + ssid + 1;
    }
    bytes += (size_t)mac_count * 6;
    return bytes;
}

size_t ap_metrics_response_build_clients(uint8_t *frame, size_t capacity, const uint8_t dest[6],
                                         const uint8_t source[6], unsigned int message_id,
                                         const struct ap_bss_metric *bsses, unsigned int count,
                                         const struct ap_client_group *groups, unsigned int group_count,
                                         const uint8_t macs[][6], unsigned int mac_count)
{
    size_t value = 0, vendor = 0, needed, used = CONTROLLER_1905_HEADER, k;
    if (groups) {
        value = clients_value_bytes(groups, group_count, mac_count);
        if (!value)
            return 0;
        vendor = 3 + 6 + 3 + value;                   /* TLV header, OUI and three vendor bytes, sub-TLV header */
    }
    needed = CONTROLLER_1905_HEADER + (size_t)count * AP_METRICS_TLV_BYTES + vendor + 3;
    if (needed < 60)
        needed = 60;                                    /* the raw socket does not pad an Ethernet frame */
    if (!frame || !dest || !source || !bsses || !count || capacity < needed)
        return 0;
    memcpy(frame, dest, 6);
    memcpy(frame + 6, source, 6);
    frame[12] = (uint8_t)(CONTROLLER_1905_ETHER >> 8);
    frame[13] = (uint8_t)CONTROLLER_1905_ETHER;
    frame[14] = 0;                                    /* revision, reserved */
    frame[15] = 0;
    frame[16] = (uint8_t)(CONTROLLER_MSG_AP_METRICS_RESPONSE >> 8);
    frame[17] = (uint8_t)CONTROLLER_MSG_AP_METRICS_RESPONSE;
    frame[18] = (uint8_t)(message_id >> 8);
    frame[19] = (uint8_t)message_id;                  /* the id of the query this answers */
    frame[20] = 0;                                    /* fragment id */
    frame[21] = 0x80;                                 /* last fragment, and not relayed: it goes over the direct link */
    for (k = 0; k < count; k++) {
        frame[used++] = CONTROLLER_TLV_AP_METRICS;
        frame[used++] = 0;
        frame[used++] = AP_METRICS_TLV_BYTES - 3;
        memcpy(frame + used, bsses[k].bssid, 6);
        used += 6;
        frame[used++] = 0;                            /* channel utilisation: not measured here, so not reported */
        frame[used++] = (uint8_t)(bsses[k].stations >> 8);
        frame[used++] = (uint8_t)bsses[k].stations;
        frame[used++] = 0;                            /* presence bitmap: no estimated-service block follows */
    }
    if (groups) {
        frame[used++] = CONTROLLER_TLV_VENDOR;
        frame[used++] = (uint8_t)((vendor - 3) >> 8);
        frame[used++] = (uint8_t)(vendor - 3);
        frame[used++] = 0x00;                         /* OUI 00:19:c6, then the three vendor bytes the walker steps */
        frame[used++] = 0x19;
        frame[used++] = 0xc6;
        frame[used++] = 0;
        frame[used++] = 0;
        frame[used++] = 0;
        frame[used++] = CONTROLLER_VENDOR_TLV_CLIENTS;
        frame[used++] = (uint8_t)(value >> 8);
        frame[used++] = (uint8_t)value;
        frame[used++] = (uint8_t)group_count;
        for (k = 0; k < group_count; k++) {
            size_t ssid = strlen(groups[k].ssid);
            memcpy(frame + used, groups[k].bssid, 6);
            used += 6;
            frame[used++] = (uint8_t)ssid;
            memcpy(frame + used, groups[k].ssid, ssid);
            used += ssid;
            frame[used++] = (uint8_t)groups[k].count;
        }
        for (k = 0; k < mac_count; k++) {
            memcpy(frame + used, macs[k], 6);
            used += 6;
        }
    }
    frame[used++] = 0;
    frame[used++] = 0;
    frame[used++] = 0;                                /* End of Message */
    if (used < 60) {
        memset(frame + used, 0, 60 - used);
        used = 60;
    }
    return used;
}

/* The value of sub-TLV 0x70. Returns what ap_clients_decode() promises: the group count, or -1 when any count
 * disagrees with the bytes behind it. A repeated BSSID or a client MAC listed twice is damage too - the page keys
 * rows by MAC and would show the same device twice, or on two nodes. */
static int clients_parse(const uint8_t *value, size_t length, struct ap_client_group *groups, unsigned int max_groups,
                         uint8_t macs[][6], unsigned int max_macs)
{
    unsigned int groups_found, k, total = 0;
    size_t at = 1, mac_at;
    if (length < 1)
        return -1;
    groups_found = value[0];
    if (groups_found > max_groups)
        return -1;
    for (k = 0; k < groups_found; k++) {
        unsigned int clients, j;
        size_t ssid;
        if (length - at < 8)                          /* BSSID, SSID length, client count */
            return -1;
        memcpy(groups[k].bssid, value + at, 6);
        at += 6;
        ssid = value[at++];
        if (ssid > CONTROLLER_SSID_MAX || ssid > length - at)
            return -1;
        memcpy(groups[k].ssid, value + at, ssid);
        groups[k].ssid[ssid] = 0;
        at += ssid;
        clients = value[at++];
        groups[k].first = total;
        groups[k].count = clients;
        total += clients;
        if (total > max_macs)
            return -1;
        for (j = 0; j < k; j++)
            if (!memcmp(groups[j].bssid, groups[k].bssid, 6))
                return -1;
    }
    if (length - at != (size_t)total * 6)
        return -1;
    mac_at = at;
    for (k = 0; k < groups_found; k++) {
        unsigned int j;
        for (j = 0; j < groups[k].count; j++, mac_at += 6) {
            unsigned int m;
            for (m = 0; m < groups[k].first + j; m++)
                if (!memcmp(macs[m], value + mac_at, 6))
                    return -1;
            memcpy(macs[groups[k].first + j], value + mac_at, 6);
        }
    }
    return (int)groups_found;
}

int ap_clients_decode(const uint8_t *frame, size_t bytes, struct ap_client_group *groups, unsigned int max_groups,
                      uint8_t macs[][6], unsigned int max_macs)
{
    struct tlv_cursor cursor;
    const uint8_t *value;
    unsigned int type;
    size_t length;
    if (!frame || !groups || !macs)
        return -1;
    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &type, &value, &length)) {
        struct zte_cursor zte;
        unsigned int sub_type;
        const uint8_t *sub_value;
        size_t sub_length;
        if (type != CONTROLLER_TLV_VENDOR || !zte_vendor_open(&zte, value, length))
            continue;
        while (zte_vendor_next(&zte, &sub_type, &sub_value, &sub_length))
            if (sub_type == CONTROLLER_VENDOR_TLV_CLIENTS)
                return clients_parse(sub_value, sub_length, groups, max_groups, macs, max_macs);
    }
    return -1;
}
