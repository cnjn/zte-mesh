#ifndef ZTE_AGENT_SYNC_H
#define ZTE_AGENT_SYNC_H
#include <stddef.h>
#include <stdint.h>

/* The library-to-hostapd synchroniser of the child (agent) role.

 * A Multi-AP agent applies the M2 it receives into the vendor configuration database (WLANCfg / WLANPSK) and
 * then announces the change with CmMultiAPWlanRenewNotify, whose subscribers are cspd and the driver layer.
 * Neither exists for this plugin, so the rows are what the Wi-Fi side has to be brought up from: this unit
 * polls them, and when they differ from the last published state it writes the host-side plan that
 * openwrt/zte-mesh/files/wlan-sync applies to UCI. Keys go into the plan (0600, the same trust domain as the
 * service configuration) and are never printed.
 */

#define AGENT_SYNC_ROWS 8
#define AGENT_SYNC_KEY_BYTES 64
/* Ten options per row plus the header; the service writes it in one buffer. */
#define AGENT_SYNC_PLAN_BYTES 4096

struct agent_sync_row {
    unsigned int radio, slot;      /* index and BSS slot the instance id was derived from */
    char instance[36];             /* WLANCfg instance, e.g. DEV.WIFI.AP1 */
    char psk_instance[36];         /* WLANPSK instance, e.g. DEV.WIFI.AP1.PSK1 */
    char ssid[37];
    unsigned int ssid_length;
    unsigned int enabled, hidden, access_attr, beacon_type;
    unsigned int key_length;
    uint32_t key_hash;             /* the key never leaves this struct as a value */
    unsigned int readable;
};

/* The rows as the database holds them now. `keys` receives the passphrases (empty when the row has none) so
 * that the caller can publish them; both outputs must be wiped by the caller. 0 on success. */
int agent_sync_read(struct agent_sync_row *rows, char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES],
                    unsigned int *count);

/* 1 when anything that reaches hostapd differs (including a key), 0 when the two snapshots agree. */
int agent_sync_changed(const struct agent_sync_row *before, unsigned int before_count,
                       const struct agent_sync_row *after, unsigned int after_count);

/* The plan text. Pure: no database, no file, so the layout can be tested on a host. */
int agent_sync_format(const struct agent_sync_row *rows, const char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES],
                      unsigned int count, char *out, size_t capacity);

/* Read, compare with the last published state and write the plan atomically (0600) when it changed.
 * Returns 1 when a plan was written, 0 when nothing changed, -1 when the rows could not be read. */
int agent_sync_poll(const char *path);

/* Forget the published state, so the next poll republishes even an unchanged database (used at start, where
 * the host side has to be brought in line with whatever the database already holds). */
void agent_sync_reset(void);

#endif
