/* Client steering messages: 1905 CMDU bytes in, 1905 CMDU bytes out.
 *
 * The encode side exists because a controller that only speaks topology and configuration cannot make
 * anything happen: "ban this phone" and "move that laptop to 5 GHz" are single messages, and building them is
 * the whole of the work on our side. The decode side covers the two answers the agents send back, which is
 * how a caller learns whether an action worked instead of assuming it did.
 *
 * No sockets and no logging here; see controller-steer.h for the field rules and docs/research/STEER_1905.md
 * for how the layouts were pinned to the Wireshark dissector.
 */
#include <string.h>
#include "controller-1905.h"
#include "controller-steer.h"

static void put_be16(uint8_t *at, unsigned int value)
{
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

/* Destination, source, ethertype, version 0, message id, fragment 0, last-fragment flag. Returns 0 when the
 * buffer cannot hold even the header, so every builder can start with this and stop on 0. */
static size_t put_cmdu_header(uint8_t *out, size_t capacity, unsigned int type, unsigned int message_id,
                              const uint8_t dst[6], const uint8_t src[6])
{
    if (!out || !dst || !src || capacity < CONTROLLER_1905_HEADER || message_id > 0xffffu)
        return 0;
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    put_be16(out + 12, CONTROLLER_1905_ETHER);
    out[14] = 0;                    /* message version */
    out[15] = 0;                    /* reserved */
    put_be16(out + 16, type);
    put_be16(out + 18, message_id);
    out[20] = 0;                    /* fragment id: these messages always fit one frame */
    out[21] = 0x80;                 /* last fragment */
    return CONTROLLER_1905_HEADER;
}

/* One TLV. `*at` is advanced on success and left alone when the TLV does not fit, so a caller that fails
 * halfway through has a wrong frame on its hands - every builder therefore checks capacity up front. */
static int put_tlv(uint8_t *out, size_t capacity, size_t *at, unsigned int type, const uint8_t *value,
                   size_t length)
{
    if (length > 0x3fffu || *at + 3 + length > capacity)
        return 0;
    out[*at] = (uint8_t)type;
    put_be16(out + *at + 1, (unsigned int)length);
    if (length)
        memcpy(out + *at + 3, value, length);
    *at += 3 + length;
    return 1;
}

/* End of Message. Its three bytes are part of the message: the dissector stops there and the Ethernet pad
 * after it is not read as a TLV. */
static int put_end(uint8_t *out, size_t capacity, size_t *at)
{
    static const uint8_t zero[3];
    return put_tlv(out, capacity, at, 0, zero, 0);
}

size_t steer_build_request(uint8_t *out, size_t capacity, unsigned int message_id, const uint8_t dst[6],
                           const struct steer_request *request)
{
    uint8_t body[32];
    size_t at = 0, bytes;
    unsigned int flags;

    if (!request || request->target_count < 0 || request->target_count > 1)
        return 0;
    bytes = put_cmdu_header(out, capacity, STEER_MSG_REQUEST, message_id, dst, request->al_mac);
    if (!bytes)
        return 0;
    at = bytes;

    if (!put_tlv(out, capacity, &at, STEER_TLV_AL_MAC, request->al_mac, 6))
        return 0;

    /* Client info: where the client is. The TLV is fixed at 12 bytes. */
    memcpy(body, request->bssid, 6);
    memcpy(body + 6, request->client, 6);
    if (!put_tlv(out, capacity, &at, STEER_TLV_CLIENT_INFO, body, 12))
        return 0;

    /* Steering request. Field order as the dissector reads it: BSSID, flags, window, timer, per-list counts. */
    flags = (request->mandate ? 0x80u : 0) | (request->disassoc_imminent ? 0x40u : 0) |
            (request->abridged ? 0x20u : 0);
    memcpy(body, request->bssid, 6);
    body[6] = (uint8_t)flags;
    put_be16(body + 7, request->window);
    put_be16(body + 9, request->disassoc_timer);
    body[11] = 1;                       /* STA list count: one client per message */
    memcpy(body + 12, request->client, 6);
    body[18] = (uint8_t)request->target_count;
    bytes = 19;
    if (request->target_count) {
        memcpy(body + 19, request->target_bssid, 6);
        body[25] = request->target_class;
        body[26] = request->target_channel;
        bytes = 27;
    }
    if (!put_tlv(out, capacity, &at, STEER_TLV_REQUEST, body, bytes))
        return 0;
    if (!put_end(out, capacity, &at))
        return 0;
    return at;
}

size_t steer_build_assoc_control(uint8_t *out, size_t capacity, unsigned int message_id, const uint8_t dst[6],
                                 const struct steer_ban *ban)
{
    uint8_t body[1 + 6 + 1 + 2 + 1 + 6 * STEER_MAX_CLIENTS];
    size_t at, bytes;
    int i;

    if (!ban || ban->count < 1 || ban->count > STEER_MAX_CLIENTS)
        return 0;
    bytes = put_cmdu_header(out, capacity, STEER_MSG_ASSOC_CONTROL, message_id, dst, ban->al_mac);
    if (!bytes)
        return 0;
    at = bytes;

    if (!put_tlv(out, capacity, &at, STEER_TLV_AL_MAC, ban->al_mac, 6))
        return 0;

    memcpy(body, ban->bssid, 6);
    body[6] = ban->allow ? 1 : 0;
    put_be16(body + 7, ban->validity_seconds);
    body[9] = (uint8_t)ban->count;
    for (i = 0; i < ban->count; i++)
        memcpy(body + 10 + 6 * i, ban->clients[i], 6);
    if (!put_tlv(out, capacity, &at, STEER_TLV_ASSOC_CONTROL, body, (size_t)(10 + 6 * ban->count)))
        return 0;
    if (!put_end(out, capacity, &at))
        return 0;
    return at;
}

/* Walks the TLVs of a decoded CMDU looking for one type. Returns 1 and the value when found. */
static int find_tlv(const uint8_t *frame, size_t bytes, unsigned int type, const uint8_t **value,
                    size_t *length)
{
    struct tlv_cursor cursor;
    unsigned int t;
    const uint8_t *v;
    size_t len;

    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &t, &v, &len)) {
        if (t == type) {
            *value = v;
            *length = len;
            return 1;
        }
    }
    return 0;
}

static int is_message(const uint8_t *frame, size_t bytes, unsigned int type)
{
    struct cmdu_header header;
    return cmdu_decode(frame, bytes, &header) && header.type == type;
}

int steer_decode_btm_report(const uint8_t *frame, size_t bytes, struct steer_btm_report *report)
{
    const uint8_t *value;
    size_t length;

    if (!report || !is_message(frame, bytes, STEER_MSG_BTM_REPORT))
        return 0;
    if (!find_tlv(frame, bytes, STEER_TLV_BTM_REPORT, &value, &length) || length < 13)
        return 0;
    memset(report, 0, sizeof(*report));
    memcpy(report->bssid, value, 6);
    memcpy(report->client, value + 6, 6);
    report->status = value[12];
    if (length >= 19) {
        memcpy(report->target, value + 13, 6);
        report->has_target = 1;
    }
    return 1;
}

int steer_decode_completed(const uint8_t *frame, size_t bytes, struct steer_completed *completed)
{
    const uint8_t *value, *bssid;
    size_t length, bssid_length;

    if (!completed || !is_message(frame, bytes, STEER_MSG_COMPLETED))
        return 0;
    if (!find_tlv(frame, bytes, STEER_TLV_CLIENT_INFO, &value, &length) || length < 12)
        return 0;
    memset(completed, 0, sizeof(*completed));
    memcpy(completed->bssid, value, 6);
    memcpy(completed->client, value + 6, 6);
    if (find_tlv(frame, bytes, STEER_TLV_BSSID, &bssid, &bssid_length) && bssid_length >= 6) {
        memcpy(completed->bssid, bssid, 6);
        completed->has_bssid = 1;
    }
    return 1;
}
