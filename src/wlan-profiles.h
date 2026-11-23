#ifndef ZTE_WLAN_PROFILES_H
#define ZTE_WLAN_PROFILES_H
/* Private ARM32 DB experiment, NOT a production WLAN backend/RPC. */
int wlan_profiles_create_test_tables(void);
/* Same DB setup, but DevInfo carries the host's own public ModelName/Manufacturer
 * (never a serial) instead of the obvious PUBLIC/TEST placeholders, so the real
 * vendor daemon's device-identity-ext check on a real peer can pass. Only the
 * --controller-service path (a genuine on-LAN run) is allowed to ask for this. */
int wlan_profiles_create_identity_tables(const char *manufacturer, const char *model_name);
/* Extra (non-primary) BSS slot 1..3 of a logical radio, for the controller
 * service only: its own WLANCfg/WLANPSK rows so the factory daemon's per-slot
 * SSID query can be answered. The WSC M2 pushed to the agent carries EVERY slot's
 * SSID, key and enable bit (observed on a real child), so ssid/key are real
 * credentials the agent will apply - callers must supply them explicitly. */
int wlan_profiles_add_extra_bss(unsigned int radio, unsigned int slot, int enabled,
                                const char *ssid, const char *key);
int wlan_profiles_beacon_type_ok(unsigned int beacon_type);
/* Per-slot SSID hiding (WLANCfg.ESSIDHideEnable, sent to the agent in the 0100/0101 slot message) and
 * BeaconType (0 = open, 3 = WPA2, 6 = WPA + WPA2, 10 = WPA3, 12 = WPA3 + WPA2; the M2 offers the agent the types it also
 * advertised in its M1) of a slot added with wlan_profiles_add_extra_bss(). */
int wlan_profiles_set_extra_bss_style(unsigned int radio, unsigned int slot, int hidden, unsigned int beacon_type);
/* Primary slot (0) style: ESSIDHideEnable and BeaconType (3 / 6 / 12) of logical radio 0/1. The profile bridge
 * resets BeaconType to WPA2 each time it writes the primary profile, so this must follow every bridge write. Takes the
 * shared-pool lock itself. */
int wlan_profiles_set_main_style(unsigned int radio, int hidden, unsigned int beacon_type);
/* Remember which P1905AcdBssCfg row belongs to an extra slot (the row whose Enable bit the agent is told to apply). */
void wlan_profiles_note_bss_row(unsigned int radio, unsigned int slot, unsigned int row);
/* Extra-slot rows exist / make a slot take part in (or leave) the daemon's ACD module; the daemon must be restarted. */
int wlan_profiles_slot_known(unsigned int radio, unsigned int slot);
int wlan_profiles_set_slot_present(unsigned int radio, unsigned int slot, int present);
/* Live edit of an extra slot added at start-up: enable, SSID, key, hidden, BeaconType, all in one pool-lock section
 * (WLANCfg + WLANPSK + the slot's P1905AcdBssCfg Enable). The daemon is told by a band renew afterwards. */
int wlan_profiles_update_extra_bss(unsigned int radio, unsigned int slot, int enabled, const char *ssid, const char *key,
                                   int hidden, unsigned int beacon_type);
/* Radio BeaconInterval (ms, 20..1000, the same range the agent's own UI enforces) of logical
 * radio 0/1: updates the controller's WLANBase row, which the daemon reads when it answers an
 * agent's band-config query during (re-)onboarding. The agent applies only BeaconInterval and
 * MaxHop from that message (no Fragment/TxPower - verified in both binaries). */
int wlan_profiles_set_force_wifi5(unsigned int radio, unsigned int slot, int value);
int wlan_profiles_set_beacon_interval(unsigned int radio, unsigned int interval_ms);
/* MAC filter (ACL) rules for the controller service. A rule applies to one BSS slot of one
 * radio, or of both radios (radio = 2); every applied rule becomes one row of the vendor
 * `AclCfg` table (Interface = that slot's WLANCfg instance), which the daemon turns into
 * the vendor ACL message of the agent's (re-)onboarding. */
struct wlan_acl_rule { char mac[18]; char name[25]; int radio; int bss; };
#define WLAN_ACL_RULES 16U
#define WLAN_ACL_ROWS 32U
/* Fill the pre-created AclCfg rows from `rules` (unused rows are cleared). hot != 0 when the
 * daemon is already running (takes the shared-pool lock; at start-up the caller holds it). */
int wlan_profiles_acl_apply(const struct wlan_acl_rule *rules, unsigned int count, int hot);
/* Per-BSS MAC filter policy as the agent applies it: 0 disabled, 1 ban (block the listed
 * MACs), 2 allow (only the listed MACs). */
int wlan_profiles_set_acl_policy(unsigned int radio, unsigned int slot, int policy, int hot);
int wlan_profiles_client_test(void);
int wlan_profiles_parent_verify(void);
struct mb_profile;
struct mb_result;
int wlan_profiles_bridge_apply(const struct mb_profile *, int test_failure, struct mb_result *);
#endif
