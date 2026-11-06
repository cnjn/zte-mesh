#ifndef ZTE_MESH_POLICIES_H
#define ZTE_MESH_POLICIES_H
#include <stddef.h>
#include <stdint.h>

#define MESH_MASTER_SIZE 2312U
#define MESH_DOMAIN_SIZE 3464U
#define MESH_DOMAIN_A "ZTE_SHIM_DOMAIN_A"
#define MESH_DOMAIN_B "ZTE_SHIM_DOMAIN_B"

enum mesh_policy { MESH_MASTER, MESH_DOMAIN };

int mesh_policies_create_test_tables(void);
int mesh_policies_client_test(void);
int mesh_policies_parent_verify(void);

/* Semantic, bounded field operations internal to the private ARM32 probe.
 * No raw structure/pointer is exposed as a helper/RPC interface.
 */
int mesh_policy_validate(enum mesh_policy, const void *, size_t);
int mesh_policy_set_scalar(enum mesh_policy, void *, size_t, const char *, uint32_t);
/* Real-network policy for the service: recorded before start, applied by
 * mesh_policies_startup_rows(). Row MULTIAPDOMAIN1 (matched to the BSSes by ESSID) gets
 * the roam RSSI limits the master hands to agents (RCPI in the vendor policy TLV) and the
 * band-steering RSSI limits (master-side only: not in any TLV sent to the agent);
 * MultiAPMaster.EnBandSteer is the master's band-steering switch. All values dBm.
 * Changing them needs a controller restart. */
struct mesh_startup_policy {
    const char *essid;
    int roam_set, roam24, roam5;
    int steer_set, steer_enabled;      /* MultiAPMaster.EnBandSteer */
    int steer_rssi_set, steer24, steer5; /* MultiAPDomain.BsRssiLmt24G/5G */
    int force_set, force;              /* weak-signal forced roam: Domain EnLegacyStaRoam + Master EnBtmKickoff/EnRoamKick */
};
void mesh_policies_set_startup_policy(const struct mesh_startup_policy *policy);
/* Apply the optional /run/zte-mesh/db-profile lines for one startup view (P1905ComCfg, MultiAPCfg). */
/* Live edits (service hot reconfiguration). Each writes the field into every MULTIAPDOMAIN row (rows 2..9) or the
 * Master row, exactly the rows the factory CM setters (CmMultiAPSetRoamCfg / SetMasterCfg) change; the daemon is told
 * afterwards by the CM notification (zte-meshctl notify-master: 0x10206 after Domain changes, 0x10205 after Master
 * changes). Return 0, or -1 if the write failed (a partial write is possible, the caller must not notify). */
int mesh_policies_live_roam(int rssi24, int rssi5);       /* Domain RoamRssiLmt24G/5G, dBm */
int mesh_policies_live_steer_rssi(int rssi24, int rssi5); /* Domain BsRssiLmt24G/5G, dBm */
int mesh_policies_live_steer_enabled(int enabled);        /* Master EnBandSteer */
int mesh_policies_live_force_domain(int on);              /* Domain EnLegacyStaRoam */
int mesh_policies_live_force_master(int on);              /* Master EnBtmKickoff + EnRoamKick */
int mesh_policies_apply_profile_view(const char *table_name, void *table);
int mesh_policy_set_string(enum mesh_policy, void *, size_t, const char *, const char *);
#endif
