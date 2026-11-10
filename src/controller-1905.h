#ifndef ZTE_CONTROLLER_1905_H
#define ZTE_CONTROLLER_1905_H
#include <stddef.h>
#include <stdint.h>

/* What one received 1905 frame says about the node that sent it.
 *
 * Pure decoding: no socket, no vendor library, no logging. The daemon's receive hook calls it on the bytes it
 * already has, and a host test calls it on constructed frames, so the layout below is the only thing that has
 * to be right. The TLV numbers and field offsets were checked against the Wireshark ieee1905 dissector
 * (tshark 3.6.2) on constructed frames, and the vendor sub-TLV container against every vendor TLV of a
 * 107-frame capture from the lab; see docs/research/CAPTURE_1905.md. */

/* IEEE 1905.1a: Ethernet type, and the CMDU header that follows the 14-byte Ethernet header. */
#define CONTROLLER_1905_ETHER 0x893a
#define CONTROLLER_1905_HEADER 22
#define CONTROLLER_1905_MAX_BYTES 2000

/* TLV types used here (the dissector's table, decimal -> value). The rest is skipped. 0xd4 is not in that table:
 * it is ZTE's own Device Inventory, and the number is the one the factory daemon puts into its AP Capability
 * Report and reads back out of it (I5TlvDeviceInventoryTypeInsert/Extract, see below). */
#define CONTROLLER_TLV_AL_MAC 0x01
#define CONTROLLER_TLV_VENDOR 0x0b
#define CONTROLLER_TLV_AP_OPERATIONAL_BSS 0x83
#define CONTROLLER_TLV_AP_METRICS_QUERY 0x93
#define CONTROLLER_TLV_AP_METRICS 0x94
#define CONTROLLER_TLV_DEVICE_INVENTORY 0xd4

/* CMDU types, for the rare field whose number only means something inside one message type. */
#define CONTROLLER_MSG_AP_CAPABILITY_REPORT 0x8002
#define CONTROLLER_MSG_AP_METRICS_QUERY 0x800b
#define CONTROLLER_MSG_AP_METRICS_RESPONSE 0x800c

struct cmdu_header {
    unsigned int type;      /* CMDU message type, e.g. 0x0003 topology response, 0x800c AP metrics response */
    unsigned int message_id;
    unsigned int fragment;
    int last_fragment;      /* header flags bit 7 */
    int relayed;            /* header flags bit 6: a neighbor forwarded the frame, so its sender is not a direct link */
};

/* 1 = a complete 1905 CMDU starting at the Ethernet header, 0 = anything else (wrong ethertype, version,
 * truncated, oversized). Never reads past `bytes`. */
int cmdu_decode(const uint8_t *frame, size_t bytes, struct cmdu_header *header);

struct tlv_cursor { const uint8_t *frame; size_t bytes, at; int done; };
/* Walks the TLV list of a CMDU from the end of its header. End of Message (type 0) stops the walk and is not
 * returned; a TLV whose length runs past the frame stops it too, so a truncated or forged frame yields the
 * TLVs before the damage and nothing after. */
void tlv_open(struct tlv_cursor *cursor, const uint8_t *frame, size_t bytes);
int tlv_next(struct tlv_cursor *cursor, unsigned int *type, const uint8_t **value, size_t *length);

struct zte_cursor { const uint8_t *value; size_t bytes, at; int valid; };
/* ZTE's Vendor Specific TLV (0x0b): OUI 00:19:c6, three more header bytes, then a list of
 * [type(1) length(2) value] sub-TLVs. Confirmed on the lab capture: the sub-TLV walk consumes the payload
 * exactly in all 61 vendor TLVs of 107 frames. 1 = recognized, 0 = another vendor's TLV. */
int zte_vendor_open(struct zte_cursor *cursor, const uint8_t *value, size_t length);
int zte_vendor_next(struct zte_cursor *cursor, unsigned int *type, const uint8_t **value, size_t *length);

/* Bit per field node_metrics_decode() actually found; the others stay at their "unknown" value. */
#define NODE_METRIC_STATIONS 1u
#define NODE_METRIC_BSS 2u
#define NODE_METRIC_HOPS 4u

struct node_metrics {
    unsigned int stations;  /* AP Metrics TLV: associated stations summed over every BSS entry in the frame */
    unsigned int bss;       /* AP Operational BSS TLV: BSS interfaces the node reports as up */
    int hops;               /* 1 = the frame arrived over the direct link, 2 = a neighbor relayed it */
    unsigned int fields;
};

/* 1 = the frame is a 1905 CMDU (metrics then hold at least hops), 0 = not a CMDU. Unknown fields keep the
 * value -1 for hops and 0 with their bit clear for the counts, so a caller can tell "no data" from "zero". */
int node_metrics_decode(const uint8_t *frame, size_t bytes, struct node_metrics *metrics);

/* One BSS of an AP Metrics report: which BSS the node counted on, and how many stations it counted there. */
struct ap_bss_metric {
    uint8_t bssid[6];
    unsigned int stations;
};

/* A child runs one BSS per (radio, slot): two radios with a guest each is four entries, and the limit below is
 * where the list stops being worth more than the frame's own total. */
#define CONTROLLER_AP_BSS_MAX 16

/* The AP Metrics TLVs of one frame, one entry per BSSID, in the order the frame lists them.
 *
 * This is the only place that keeps the per-BSS breakdown: node_metrics_decode() sums the same TLVs into one number,
 * which is what a single-cell page needs but loses which radio is busy. A frame the list cannot hold whole - a
 * BSSID that repeats, more distinct BSSes than `max` - returns 0 rather than a partial list, because a sum over a
 * partial list is a too-small number that looks measured. */
unsigned int ap_metrics_by_bss(const uint8_t *frame, size_t bytes, struct ap_bss_metric *out, unsigned int max);

/* AP Metric Query (CMDU 0x800b), AP Metric Query TLV (0x93) = {BSSID number, BSSID[n]} - the shape the factory
 * master's I5TlvApMetricsQueryInsert writes, which is what `enroller --metric` sends and what a ZTE master sends
 * when it wants station counts. The one field whose meaning depends on the message type is the number: a query
 * that lists no BSSID asks about every BSS the node runs.
 *
 * 1 = a complete, unfragmented query whose list this reads in whole, with `count` BSSIDs in `bssids` (0 = about
 * all of them). 0 = not a query, a fragment, no 0x93 TLV, or a list this decoder will not split (a length that
 * disagrees with its own count byte, more BSSIDs than CONTROLLER_AP_BSS_MAX): the cost of not answering a query is
 * that nothing shows on the page, and the cost of answering one we only half read is a number that looks measured. */
int ap_metrics_query_decode(const uint8_t *frame, size_t bytes, struct cmdu_header *header,
                            uint8_t bssids[CONTROLLER_AP_BSS_MAX][6], unsigned int *count);

/* The answer to one query: CMDU 0x800c carrying one AP Metrics TLV (0x94) per BSS, in the layout
 * node_metrics_decode() and ap_metrics_by_bss() read - BSSID[6], channel utilisation, station count, presence
 * byte. `dest` is the MAC that asked, `source` this node's AL MAC: the master keys the station files it writes by
 * the Ethernet source of the frame, so a reply from anywhere else is filed under the wrong node.
 *
 * The frame is padded to 60 bytes because a raw socket does not pad an Ethernet frame. Returns the byte count, or
 * 0 when there is nothing to say (no BSS given) or the frame will not fit in `capacity`. */
size_t ap_metrics_response_build(uint8_t *frame, size_t capacity, const uint8_t dest[6], const uint8_t source[6],
                                 unsigned int message_id, const struct ap_bss_metric *bsses, unsigned int count);

/* The client list a node reports beside its station counts, in the vendor container of the same AP Metrics
 * Response (0x800c): OUI 00:19:c6, sub-TLV 0x70. The number is this repository's own, not the vendor's - no
 * capture carries it, and the vendor's walker drops a sub-TLV it has no handler for. It exists because the vendor
 * agent inside the sandbox cannot report it either: its station table comes from cspd, which does not run here, and
 * the list that can be measured lives on the host. So the child answers the query it already answers, and the
 * master files the list beside the counts; a factory child simply sends the plain response and stays unattributed.

   value: group count(1), then per group { BSSID[6], SSID length(1), SSID[length], client count(1) }, then every
   client MAC of every group, in group order, six bytes each. A MAC appears once: a station is associated to one
   BSS at a time, and a repeat would make the page count it twice. */
#define CONTROLLER_VENDOR_TLV_CLIENTS 0x70
#define CONTROLLER_SSID_MAX 32

/* One BSS of a client report. `first` is where its MACs start in the decoder's one flat array, so a frame of
 * thirty-two clients does not cost the caller a per-group array of that size. */
struct ap_client_group {
    uint8_t bssid[6];
    char ssid[CONTROLLER_SSID_MAX + 1];
    unsigned int first;
    unsigned int count;
};

/* The largest client list a report carries. Beyond this the child drops clients rather than the whole answer:
 * the counts are still exact, and a home access point that reaches the cap has more devices than a page row list
 * is worth. */
#define CONTROLLER_CLIENTS_MAX 32

/* The client report of one frame: -1 = no client TLV (a factory child, a damaged one, or an older writer), 0 = the
 * TLV is there and the node runs no client on it (which is what clears the master's copy), otherwise the number of
 * groups copied. A group that repeats a BSSID, a count that disagrees with the bytes behind it, a list longer than
 * `max_macs` - all of it reads as -1: a partial attribution is worse than none, because the page would show a
 * device on a node it is not on. */
int ap_clients_decode(const uint8_t *frame, size_t bytes, struct ap_client_group *groups, unsigned int max_groups,
                      uint8_t macs[][6], unsigned int max_macs);

/* ap_metrics_response_build() with the client TLV appended before End of Message. `groups` NULL = the plain
 * answer; `groups` with `group_count` 0 = the node measured no client at all, which is still an answer. `macs`
 * holds the clients of every group in order, `mac_count` their total. Returns 0 when the whole frame - TLVs,
 * vendor header and MAC list - does not fit in `capacity`. */
size_t ap_metrics_response_build_clients(uint8_t *frame, size_t capacity, const uint8_t dest[6],
                                         const uint8_t source[6], unsigned int message_id,
                                         const struct ap_bss_metric *bsses, unsigned int count,
                                         const struct ap_client_group *groups, unsigned int group_count,
                                         const uint8_t macs[][6], unsigned int mac_count);

/* The query that starts the exchange, built the way a controller builds it: CMDU 0x800b with a 0x93 that lists no
 * BSSID, so the agent answers for every BSS it runs. `dest` is that agent's AL MAC and `source` this controller's,
 * because the answer comes back to the Ethernet source of the query and is filed under it. 60 bytes on the wire,
 * or 0 when `capacity` will not hold that. */
size_t ap_metrics_query_build(uint8_t *frame, size_t capacity, const uint8_t dest[6], const uint8_t source[6],
                              unsigned int message_id);

/* The longest version the inventory can carry. The factory receiver copies each of its strings into a 64-byte
 * member and refuses a length byte above 64, so nothing longer is ever a version. */
#define CONTROLLER_INVENTORY_TEXT 64

/* The Device Inventory TLV (0xd4): three strings in wire order - serial number, software version, hardware
 * version - each as [length(1), text], then a count byte and one record per radio. This takes only the software
 * version, and only as plain ASCII: the serial belongs to the device, and a page cell full of control bytes is
 * worse than an empty one. The layout is read out of the factory daemon, not seen on the wire: the sender
 * ApCapabilityReportSend @0x58470 (CMDU 0x8002) inserts this TLV through I5TlvDeviceInventoryTypeInsert @0x501e0,
 * and I5TlvDeviceInventoryTypeExtract @0x3d674 lays the three strings at device record +312, +247 and +377 with
 * I5TlvExtractChildlv @0x68764, which is the [length][text] walk and the 64-byte cap this copies. Whether a given
 * child sends it is a question for the device it runs on.
 *
 * 1 = `out` holds a version, 0 = the TLV is too short, damaged or empty (then `out` is ""). A length that runs
 * past the TLV stops the walk, so a truncated report yields nothing rather than the bytes after it. A buffer
 * shorter than 64 bytes gets the version truncated to fit. */
int device_inventory_software(const uint8_t *value, size_t length, char *out, size_t out_size);

#endif
