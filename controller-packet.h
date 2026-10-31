#ifndef ZTE_CONTROLLER_PACKET_H
#define ZTE_CONTROLLER_PACKET_H
#include <stddef.h>
#include <stdint.h>

/* Validate only a public lab Topology Response; never expose TLV bodies. */
int controller_topology_response_matches(const uint8_t *, size_t, const uint8_t[6]);
int controller_autoconfig_response_matches(const uint8_t *, size_t, const uint8_t[6], unsigned int band);
/* 0 discovery, 1 topology query, 2/3 public MAP search on band 0/1. */
int controller_inject_packet(int query);
int controller_inject_wsc_m1(void);
/* Block until a frame of the given CMDU type appears on the private wire. */
int controller_cmdu_listener(void);
int controller_wait_cmdu(int listener, unsigned int type, unsigned int timeout_ms);
int controller_wsc_m2_matches(const uint8_t *, size_t, const uint8_t[6], const uint8_t[16]);
/* Standard Multi-AP Channel Selection Request (CMDU 0x8006) from the controller to one
 * agent, built the way the factory master builds it (Channel Preference TLV 0x8b with one
 * operating class holding the single target channel, Transmit Power Limit TLV 0x8d) and
 * sent from the daemon's private port. channel: -1 leave unchanged, 0 automatic channel,
 * else a fixed channel (the agent also turns its auto-channel switch off); power: -1 leave
 * unchanged, else EIRP dBm 0..30 (the agent maps it to its 5 transmit-power levels).
 * Returns 0 when the agent's Channel Selection Response (0x8007) arrived (its response code
 * in *code, 0 = accepted), -2 on timeout, -1 on any local error. */
int controller_radio_select(const uint8_t agent[6], unsigned int radio, int channel, int power, int *code);

/* One frame the daemon sent, with nothing of its body kept. */
struct controller_frame {
    unsigned int cmdu, mid, wsc_message, bytes, band;
    /* Supported Service TLV (0x80): how many entries it lists and the first entry's value, which is what
     * "this device is a Multi-AP Agent / Controller" is written as. */
    unsigned int service_count, service_first;
    uint8_t src[6];
    /* The WSC TLV payload (attribute list) when the frame carries one, and the Radio Unique Identifier that
     * leads the AP Radio Basic Capabilities TLV (0x85) - the first six bytes of its payload. Both are needed
     * to answer an M1 with an M2; the payload is wiped by the caller once it has been consumed. */
    uint8_t wsc[1200];
    size_t wsc_bytes;
    uint8_t radio[6];
    size_t radio_bytes; /* 6 once an identifier has been read, 0 otherwise */
};
/* Drain at most one frame from a listener opened with controller_cmdu_listener(): 1 = a frame was read,
 * 0 = none pending, -1 = error. `wsc_message` is the WSC Message Type attribute (0x1022) when the frame
 * carries a WSC payload, 0 otherwise. */
int controller_read_frame(int fd, struct controller_frame *frame);
/* Answer an AP Autoconfiguration Search (CMDU 0x0007) the way the factory controller does: an AP
 * Autoconfiguration Response (CMDU 0x0008) with Searched Role, Autoconfiguration Frequency Band and
 * Supported Service (Multi-AP Controller). `agent` is the MAC the Search came from, `band` its frequency
 * band, `mid` the Search's message id (the response must carry the same one). 0 = sent. */
int controller_autoconfig_response(const uint8_t agent[6], unsigned int band, unsigned int mid);
/* Answer the agent's Topology Discovery (CMDU 0x0000) with a Topology Query (CMDU 0x0002): AL MAC TLV
 * (0x01), MAC Address TLV (0x02) and Supported Service TLV (0x80) declaring `service`. This is the step
 * that makes the agent trust us: until it has seen a Supported Service = Controller from this MAC the
 * daemon drops our Autoconfiguration Response and never sends WSC M1 (FINDINGS section 4). A Multi-AP
 * Agent declares 1 there (observed from the daemon), so `service` is what the lab has to settle for
 * Controller. */
int controller_topology_query(const uint8_t agent[6], unsigned int mid, unsigned int service);
#endif
