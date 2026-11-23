#ifndef ZTE_CONTROLLER_STEER_H
#define ZTE_CONTROLLER_STEER_H
#include <stddef.h>
#include <stdint.h>

/* The three client-steering messages a controller sends, built byte by byte.
 *
 * Same rule as controller-1905.h: a pure function of its arguments. No socket, no vendor library, no
 * logging; the caller owns the frame buffer and the send path. That is what makes the wire format testable
 * on a host with nothing plugged in.
 *
 * Every layout below was checked field by field against the Wireshark ieee1905 dissector (tshark 4.4.2) by
 * feeding it the bytes this file produces and reading the decoded values back; see
 * docs/research/STEER_1905.md for the probe and the field order it settled (the flags byte sits before the
 * opportunity window, and each list count sits immediately in front of its own list - the spec text orders
 * these differently from the dissector, and the dissector is what we can check against). */

/* CMDU message types (the dissector's table). */
#define STEER_MSG_REQUEST 0x8014           /* Client Steering Request */
#define STEER_MSG_BTM_REPORT 0x8015        /* Client Steering BTM Report */
#define STEER_MSG_ASSOC_CONTROL 0x8016     /* Client Association Control Request */
#define STEER_MSG_COMPLETED 0x8017         /* Steering Completed */

/* TLV types. */
#define STEER_TLV_AL_MAC 0x01
#define STEER_TLV_CLIENT_INFO 0x90
#define STEER_TLV_REQUEST 0x9b
#define STEER_TLV_BTM_REPORT 0x9c
#define STEER_TLV_ASSOC_CONTROL 0x9d
#define STEER_TLV_BSSID 0xb8

/* Largest frame any builder here produces (a ban for 8 clients is 36 + 22 bytes of TLVs). The 1905 MTU is
 * 1500 and none of these messages gets near it, so nothing here fragments. */
#define STEER_FRAME_MAX 128
#define STEER_MAX_CLIENTS 8

/* 0x8014: ask one agent to move one client off a BSS.
 *
 * `client` and `bssid` describe where the client is now; a message naming a client the agent does not have is
 * ignored by every agent we know of, so the caller has to get these from its own association table.
 * `mandate` = 1 means "steer, this is not a suggestion"; 0 offers a steering opportunity. `disassoc_imminent`
 * makes the agent send a BTM request with the disassociation-imminent bit, which is the only variant that
 * reliably moves a client that ignores BTM otherwise - and the one that drops it off the network for
 * `disassoc_timer` if it does not move. */
struct steer_request {
    uint8_t al_mac[6];       /* our AL MAC, as in the AL MAC TLV: the agent answers to this */
    uint8_t bssid[6];        /* source BSS: where the client is associated right now */
    uint8_t client[6];
    uint16_t window;         /* steering opportunity window, as the spec's own unit; forwarded unchanged */
    uint16_t disassoc_timer; /* BTM disassociation timer, in TBTTs (100 ms each on 802.11) */
    int mandate;             /* flags bit 7 */
    int disassoc_imminent;   /* flags bit 6 */
    int abridged;            /* flags bit 5 */
    int target_count;        /* 0 = let the agent pick, 1 = the target BSS below. Never more than 1 here. */
    uint8_t target_bssid[6];
    uint8_t target_class;    /* operating class of the target channel, 0 when unknown */
    uint8_t target_channel;
};

/* Writes a complete Ethernet frame (destination, our source, 0x893a, CMDU header, TLVs, End of Message) and
 * returns its length, or 0 when the arguments do not fit `capacity` or are out of range. `dst` is the agent's
 * AL MAC for a unicast request, or the 1905 multicast group when every agent should see it. */
size_t steer_build_request(uint8_t *out, size_t capacity, unsigned int message_id, const uint8_t dst[6],
                           const struct steer_request *request);

/* 0x8016: refuse association of a client at one BSS, or lift a refusal.
 *
 * This is what "ban" has to mean: the agent keeps the client off that BSS for `validity_seconds`, no matter
 * how often it retries. 0 means "until further notice" in the dissector's reading, which is what a ban the
 * user has to undo wants. */
struct steer_ban {
    uint8_t al_mac[6];
    uint8_t bssid[6];
    int allow;                 /* 0 = block, 1 = unblock */
    uint16_t validity_seconds; /* 0 = until further notice */
    int count;                 /* 1..STEER_MAX_CLIENTS */
    uint8_t clients[STEER_MAX_CLIENTS][6];
};

size_t steer_build_assoc_control(uint8_t *out, size_t capacity, unsigned int message_id, const uint8_t dst[6],
                                 const struct steer_ban *ban);

/* 0x8015: the agent's answer to a BTM request. Status 0 = accept, 1 = reject, 2 = BTM request timeout,
 * 3 = unable to process (the dissector's value_string); anything else is kept as is. */
struct steer_btm_report {
    uint8_t bssid[6];   /* the source BSS the report is about */
    uint8_t client[6];
    uint8_t target[6];  /* target BSSID the client picked; all zero when the agent reported no target */
    unsigned int status;
    int has_target;     /* 0 = the TLV stopped before the target BSSID field */
};

int steer_decode_btm_report(const uint8_t *frame, size_t bytes, struct steer_btm_report *report);

/* 0x8017: the client left the source BSS, which is how a steer is confirmed. `has_bssid` is 0 when the agent
 * sent only the client info TLV, and `bssid` is then the one inside that TLV (where the client came from). */
struct steer_completed {
    uint8_t client[6];
    uint8_t bssid[6];
    int has_bssid;
};

int steer_decode_completed(const uint8_t *frame, size_t bytes, struct steer_completed *completed);

#endif
