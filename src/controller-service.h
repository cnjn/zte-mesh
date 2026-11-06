#ifndef ZTE_CONTROLLER_SERVICE_H
#define ZTE_CONTROLLER_SERVICE_H
#include <sys/types.h>
#include "mesh-bridge.h"
/* One row of the real daemon's P1905DeviceInfo node inventory (read only). */
struct controller_node {
    char view[64], mac[18], model[64], name[64], software[64];
    unsigned int zte, active;
};
unsigned int controller_read_nodes(struct controller_node *nodes, unsigned int max);
/* This router's own model (OpenWrt's board name) and host name, each falling back to "router" when the system
 * cannot answer. Display only: the identity sent to a ZTE child is the fixed SR1010 shape. */
void controller_self_identity(char *model, size_t model_size, char *name, size_t name_size);
/* Optional extra BSS slots (1..3 per radio) the controller answers SSID queries for.
 * set[r][k]!=0 exposes slot k of radio r; enabled[r][k] is the enable bit sent. */
void controller_set_extra_bss(const uint8_t set[2][4], const uint8_t enabled[2][4],
                              const char ssid[2][4][32], const char key[2][4][64]);
/* Their optional style: hidden SSID and BeaconType (3 / 6 / 12). */
void controller_set_extra_bss_style(const uint8_t hidden[2][4], const uint8_t beacon_type[2][4]);
/* Style of the primary slot (0) of each radio: hidden SSID and BeaconType (3 / 6 / 12). */
void controller_set_main_style(const uint8_t hidden[2], const uint8_t beacon_type[2]);
/* MAC filter rules and per-slot policies (-1 = keep the agent-mirroring default). */
struct wlan_acl_rule;
/* "Self-networking V2" = P1905ComCfg.Enable: the receive-side admission check (peer must carry the
 * ZTE identity and, with MarketConfEn, the same market sign). Default off at start-up; a live change
 * rewrites the field and respawns the daemon (it reads Enable once when its base module starts). */
void controller_set_v2(int enable);
/* P1905ComCfg.Enable as the database holds it (-1 = unreadable). */
int controller_read_v2(void);
/* Live add / remove of extra BSS slot K of a radio (0 = ok): creates or reuses its rows and makes it take part in (or leave)
 * the daemon's ACD module, which only learns about it when it starts, so the caller restarts the daemon. */
int controller_slot_present(unsigned int radio, unsigned int slot, int present, int enabled, const char *ssid,
                            const char *key, int hidden, unsigned int beacon);
/* MultiAPCfg scalar read / live write with read-back (0 = ok). */
int controller_read_cfg(const char *field, int *value);
int controller_write_cfg(const char *field, int value);
/* mesh.role as the MultiAPCfg triple the daemon reads once at start-up. MASTER: SetRoleEnable 0, Mode 2.
 * AUTO: SetRoleEnable 1 with Mode 2 (the elected result the supervisor settles). SLAVE is the agent side:
 * Mode 1, SetRoleEnable 0 and EnMasterOnly 0 - the last one is what lets multi_ap_slave_main run slave_init
 * (multiap-cfg.c leaves EnMasterOnly 1, the radio-less-controller compensation). Start-up value only; the
 * daemon reads all three once, so a live change needs a respawn. */
#define CONTROLLER_ROLE_UNSET (-1)
#define CONTROLLER_ROLE_MASTER 0
#define CONTROLLER_ROLE_AUTO 1
#define CONTROLLER_ROLE_SLAVE 2
void controller_set_role(int role);
int controller_role(void);
/* Lab observation of the agent side: boot with SLAVE, wait for the daemon's own 1905 frames, answer an AP
 * autoconfiguration Search, run the public WSC M1/M2 exchange against it, then report what the M2 did to the
 * WLANCfg/WLANPSK rows. Bounded, private netns only, no credentials logged. */
int controller_agent_startup(void);
/* MultiAPCfg.P1905DevNum: 1905 devices in the daemon's topology, itself included (-1 = unreadable). */
int controller_read_dev_num(void);
/* Live write + read-back of P1905ComCfg.Enable (0 = ok); the caller respawns the daemon. */
int controller_set_v2_live(int enable);
/* Stop *child (SIGKILL, as the factory multiapd_mgr does) and start a new daemon on the same database: 0 = running,
 * negative = nothing was started (-2/-3 pool lock held by the dead daemon, -1 kill/wait/fork failure). */
int controller_respawn_daemon(pid_t *child);
void controller_set_acl(const struct wlan_acl_rule *rules, unsigned int count, const int policy[2][4]);
/* WLANSetting.DualBandSync / MLOEnable (the Wi-Fi 7 MLO switch pushed to the agents); start-up only. */
void controller_set_wlan_setting(int dual_band_sync, int mlo_enable, const char *essid_suffix);
/* Live write + read-back of WLANSetting.DualBandSync / ESSIDSuffix (0 = ok). */
int controller_set_wlan_setting_live(int dual_band_sync, const char *essid_suffix);
/* Live change of WLANSetting.MLOEnable (0 = ok); the caller renews the bands. */
int controller_set_mlo_live(int enable);
int controller_boot(const struct mb_profile profiles[2], const char *version_mode, int lab,
                    pid_t *child, int *shm_id);
int controller_shutdown_private(int shm_id);
int controller_service(void);
/* Trigger the factory WLAN-renew notification for both bands (helper process). */
int controller_renew_agents(void);
/* Ask the daemon's acd task to start the role election (helper process; the lab stand-in for cspd's cooperator). */
int controller_election_trigger(void);
#endif
