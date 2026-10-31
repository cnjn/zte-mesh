/* Bounded execution of the actual vendor daemon, using private fixture DB.
 * CAP_NET_RAW only in a private lab netns; actual Linux L2 identity adapter.
 * No blanket CM/driver success stubs, no host interfaces or IPC.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vendor-db.h"
#include "wlan-profiles.h"
#include "mesh-bridge.h"
#include "controller-packet.h"
#include "controller-wsc-crypto.h"
#include "controller-service.h"
#include "agent-sync.h"
#include "startup-views.inc"
/* Where the agent role publishes the host-side Wi-Fi plan (agent-sync.c, applied by wlan-sync). */
#define AGENT_WLAN_PLAN "/run/zte-mesh/wlan-plan"
extern int ShmPoolLock(unsigned int);
extern int ShmPoolUnlock(void);
extern int mesh_policies_startup_rows(void);
extern int mesh_policies_apply_profile_view(const char *table_name, void *table);

static int startup_table(const char *name, const struct zte_db_domain *fields,
                         unsigned int count, unsigned int bytes, unsigned int rows,
                         unsigned int initial, void **out)
{
    uint32_t descriptor[41] = { (uint32_t)(uintptr_t)name, 0, rows, 1 };
    void *table = dbCreateTbl(descriptor);
    if (!table || dbCreateDomainNew(table, fields, (count + 1) * sizeof(*fields)) ||
        dbMgrCommViewAddItem(name, name, "", bytes, rows == 1 ? 0 : 1024, NULL,
                            rows == 1 ? 12 : 63, name, "", "") || (initial && !dbAddRow(table)))
        return -1;
    *out = table;
    return 0;
}
static int startup_view(const char *name, const struct zte_db_domain *fields,
                        unsigned int count, unsigned int bytes, unsigned int rows, void **out)
{
    return startup_table(name, fields, count, bytes, rows, 1, out);
}
/* Views the daemon reads through dbAPIGetView / dbAPILstView (MapGetRoamCatonCfgDB, WlanSettingDbRead,
 * I5TlvVendorRoamCatonTypeInsert). cspd registers them with arguments that differ from startup_table()
 * above (read off the _dbInitTbl call sites):
 *   MultiAPRoamCatonCfg: dbMgrCommViewAddItem(name, name, "IGD", 232, 1, 0, 12, name, "", "")
 *   WLANSetting:         dbMgrViewAddItem(name, name, 520, 1, 63), whose libdb wrapper passes "" for the
 *                        parent and args 8..10, arg5 = 1, arg6 = 0
 *   MultiAPCacheMac:     dbMgrCommViewAddItem(name, "DEV", "IGD", 108, 200, 0, 63, name, "", "")
 * The cache-MAC list is empty on the factory router, but listing it must succeed: when the list fails
 * I5TlvVendorRoamCatonTypeInsert returns -8, which drags the vendor policy body length negative and the
 * first PolicyConfigReq collapses to one stray byte. */
struct exact_view {
    const char *name, *id, *parent, *arg8;
    unsigned int bytes, rows, arg5, flags, initial;
};
static int startup_exact_view(const struct exact_view *v, const struct zte_db_domain *fields,
                              unsigned int count, void **out)
{
    uint32_t descriptor[41] = { (uint32_t)(uintptr_t)v->name, 0, v->rows, 1 };
    void *table = dbCreateTbl(descriptor);
    if (!table || dbCreateDomainNew(table, fields, (count + 1) * sizeof(*fields)) ||
        dbMgrCommViewAddItem(v->name, v->id, v->parent, v->bytes, v->arg5, NULL, v->flags, v->arg8, "", "") ||
        (v->initial && !dbAddRow(table)))
        return -1;
    *out = table;
    return 0;
}
extern int dbGetTblRowCount(const char *, unsigned int *);
extern int dbGetValComm(const char *, unsigned int, const char *, void *, unsigned int);
/* Typed raw cells: strings need the full cell size (a short buffer is -13),
 * a MAC is 6 raw bytes and the flags are one raw byte. */
static int cell(unsigned int row, const char *name, void *value, size_t size)
{
    memset(value, 0, size);
    return dbGetValComm("P1905DeviceInfo", row, name, value, (unsigned int)size);
}
static void cell_string(unsigned int row, const char *name, char *out, size_t size)
{
    char value[300];
    if (cell(row, name, value, sizeof(value))) {
        out[0] = 0;
        return;
    }
    value[sizeof(value) - 1] = 0;
    snprintf(out, size, "%.*s", (int)size - 1, value);
}
/* Node inventory: rows the real daemon adds to P1905DeviceInfo. Read only;
 * serial numbers and product class are deliberately not exposed. */
unsigned int controller_read_nodes(struct controller_node *nodes, unsigned int max)
{
    unsigned int count = 0, row, n = 0;
    uint8_t raw[300];
    if (ShmPoolLock(1000))
        return 0;
    if (dbGetTblRowCount("P1905DeviceInfo", &count))
        count = 0;
    for (row = 0; row < count && n < max; row++) {
        struct controller_node *node = &nodes[n];
        memset(node, 0, sizeof(*node));
        cell_string(row, "ViewName", node->view, sizeof(node->view));
        if (cell(row, "MacAddr", raw, sizeof(raw)))
            continue;
        snprintf(node->mac, sizeof(node->mac), "%02x:%02x:%02x:%02x:%02x:%02x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
        cell_string(row, "ModelName", node->model, sizeof(node->model));
        cell_string(row, "Friendlyname", node->name, sizeof(node->name));
        cell_string(row, "SoftwareVer", node->software, sizeof(node->software));
        node->zte = !cell(row, "ZTEDevFlag", raw, sizeof(raw)) && raw[0];
        node->active = !cell(row, "Active", raw, sizeof(raw)) && raw[0];
        n++;
    }
    (void)ShmPoolUnlock();
    return n;
}
static void dump_nodes(unsigned long *last)
{
    struct controller_node nodes[16];
    char line[1024];
    unsigned int count = controller_read_nodes(nodes, 16), i;
    size_t used = 0;
    unsigned long hash = 5381;
    line[0] = 0;
    for (i = 0; i < count && used < sizeof(line) - 256; i++)
        used += (size_t)snprintf(line + used, sizeof(line) - used, "[%u view=%s mac=%s model=%.40s name=%.40s sw=%.40s zte=%u active=%u]",
                                 i, nodes[i].view, nodes[i].mac, nodes[i].model, nodes[i].name,
                                 nodes[i].software, nodes[i].zte, nodes[i].active);
    for (i = 0; line[i]; i++)
        hash = hash * 33 + (unsigned char)line[i];
    hash = hash * 33 + count;
    if (hash != *last) {
        *last = hash;
        printf("ZTE_MESH_NODES count=%u %s\n", count, line);
        fflush(NULL);
    }
}

static int p1905_v2_enable; /* P1905ComCfg.Enable, default off */
void controller_set_v2(int enable)
{
    p1905_v2_enable = enable ? 1 : 0;
}

/* WLANSetting.DualBandSync / MLOEnable / ESSIDSuffix at start-up: the vendor baseline has both switches on and the suffix "-5G". */
static int wlan_dual_band_sync, wlan_mlo_enable = 1;
static char wlan_essid_suffix[9] = "-5G";
void controller_set_wlan_setting(int dual_band_sync, int mlo_enable, const char *essid_suffix)
{
    wlan_dual_band_sync = dual_band_sync ? 1 : 0;
    wlan_mlo_enable = mlo_enable ? 1 : 0;
    if (essid_suffix && *essid_suffix && strlen(essid_suffix) < sizeof(wlan_essid_suffix))
        snprintf(wlan_essid_suffix, sizeof(wlan_essid_suffix), "%s", essid_suffix);
}

/* Live MLO switch: WLANSetting.MLOEnable of the running controller (row 0). The M2 builder reads it when the next
 * Renew re-runs onboarding (the Wi-Fi 7 MLD attributes drop out of the M2 when it is off). */
int controller_set_mlo_live(int enable)
{
    extern void *dbFindTbl(const char *);
    void *table = dbFindTbl("WLANSetting");
    int status;
    if (!table || ShmPoolLock(1000))
        return -1;
    status = dbSetValCommByRowNo(table, 0, "MLOEnable", enable ? "1" : "0");
    (void)ShmPoolUnlock();
    return status ? -1 : 0;
}

/* P1905ComCfg.Enable as the database holds it (-1 = unreadable); the CM getters read the same field. */
int controller_read_v2(void)
{
    uint8_t raw[16] = {0}; /* a flag is one raw byte (see cell() above) */
    if (dbGetValComm("P1905ComCfg", 0, "Enable", raw, sizeof(raw)))
        return -1;
    return raw[0] ? 1 : 0;
}

/* MultiAPCfg.P1905DevNum: the daemon's count of 1905 devices in its topology, itself included (-1 = unreadable). The
 * factory log shows it dropping to 1 and back to 2 across a daemon restart (FEATURE_MAP section 9). */
int controller_read_dev_num(void)
{
    uint8_t raw[16] = {0};
    int status;
    if (ShmPoolLock(1000))
        return -1;
    status = dbGetValComm("MultiAPCfg", 0, "P1905DevNum", raw, sizeof(raw));
    (void)ShmPoolUnlock();
    return status ? -1 : (int)(raw[0] | raw[1] << 8 | raw[2] << 16);
}

/* All rows of one extra BSS slot, written under the caller's pool lock: WLANCfg + WLANPSK (wlan-profiles), the slot's
 * P1905AcdBssCfg row at bss_row and four empty WEP rows from wep_row. CustomTag is the per-BSS directive the radio-less
 * controller uses to mark a BSS active instead of tearing it down; the ACD's teardown decision needs bits 2 and 6 while
 * VersionMode bit 1 is clear (0 and the vendor baseline 128 alike; bit 7 only adds the Wi-Fi 7 MLD TLVs, run 15):
 *   bit2 (0x4): _acdGetEnConfigBss treats the BSS as an "enabled config" BSS only when set, so acdcInitBssCfg creates
 *               the active g_AcdConfig entry that acdsIsTeardown looks for (no entry -> tear down); it is also what
 *               lets the slot take part in the M2 at all.
 *   bit6 (0x40): makes the emitted Multi-AP extension a fronthaul BSS rather than a tear-down marker.
 * 0x44 = 68. Bit0 (0x1) is left clear: with it set the ACD takes the enable bit from this row as it was when the
 * daemon started; without it the bit comes from WLANCfg.Enable at every Renew, as at the factory, which needs the
 * CM adapter to fill the Enable byte (controller-profile.c; with bit 0 clear and no adapter Enable every BSS is torn
 * down, lab run: map_teardown=1). */
static int create_extra_slot_rows(unsigned int radio, unsigned int slot, int enabled, const char *ssid, const char *key,
                                  int hidden, unsigned int beacon, unsigned int bss_row, unsigned int wep_row)
{
    extern void *dbFindTbl(const char *);
    extern int WlanIndex2InstId(unsigned int, unsigned int, char *);
    extern int WlanCardIndex2BaseInstId(unsigned int, char *);
    void *bss = dbFindTbl("P1905AcdBssCfg"), *wep = dbFindTbl("WLANWEP");
    char ap[32], base[32];
    unsigned int key_slot;
    if (!bss || !wep ||
        wlan_profiles_add_extra_bss(radio, slot, enabled, ssid, key) ||
        wlan_profiles_set_extra_bss_style(radio, slot, hidden, beacon) ||
        WlanIndex2InstId(radio, slot, ap) || WlanCardIndex2BaseInstId(radio, base) ||
        !dbAddRow(bss) ||
        dbSetValCommByRowNo(bss, bss_row, "ViewName", ap) ||
        dbSetValCommByRowNo(bss, bss_row, "WLANCfgInst", ap) ||
        dbSetValCommByRowNo(bss, bss_row, "WLANBaseName", base) ||
        dbSetValCommByRowNo(bss, bss_row, "CustomTag", "68") ||
        dbSetValCommByRowNo(bss, bss_row, "Enable", enabled ? "1" : "0"))
        return -1;
    wlan_profiles_note_bss_row(radio, slot, bss_row);
    for (key_slot = 0; key_slot < 4; key_slot++) {
        char name[32];
        if (!dbAddRow(wep) ||
            snprintf(name, sizeof(name), "%s.WEP%u", ap, key_slot + 1) >= (int)sizeof(name) ||
            dbSetValCommByRowNo(wep, wep_row + key_slot, "ViewName", name) ||
            dbSetValCommByRowNo(wep, wep_row + key_slot, "WLANViewName", ap) ||
            dbSetValCommByRowNo(wep, wep_row + key_slot, "Key", ""))
            return -1;
    }
    return 0;
}

/* Live add / remove of an extra slot (0 = ok; the caller restarts the daemon so its ACD module rebuilds its BSS list).
 * A slot whose rows exist (start-up, or an earlier add) is updated and made present again; otherwise the rows are
 * created after the last ones. */
int controller_slot_present(unsigned int radio, unsigned int slot, int present, int enabled, const char *ssid,
                            const char *key, int hidden, unsigned int beacon)
{
    unsigned int bss_rows = 0, wep_rows = 0;
    int status;
    if (radio > 1 || slot < 1 || slot > 3)
        return -1;
    if (!present)
        return wlan_profiles_set_slot_present(radio, slot, 0);
    if (wlan_profiles_slot_known(radio, slot))
        return wlan_profiles_update_extra_bss(radio, slot, enabled, ssid, key, hidden, beacon) ||
               wlan_profiles_set_slot_present(radio, slot, 1) ? -1 : 0;
    if (ShmPoolLock(1000))
        return -1;
    status = dbGetTblRowCount("P1905AcdBssCfg", &bss_rows) || dbGetTblRowCount("WLANWEP", &wep_rows) ||
             create_extra_slot_rows(radio, slot, enabled, ssid, key, hidden, beacon, bss_rows, wep_rows);
    (void)ShmPoolUnlock();
    return status ? -1 : 0;
}

/* MultiAPCfg scalars as the database holds them: type 12/14 fields are 4 raw bytes (signed), type 15 one byte; 0 = ok. */
int controller_read_cfg(const char *field, int *value)
{
    uint8_t raw[16] = {0};
    int status;
    if (ShmPoolLock(1000))
        return -1;
    status = dbGetValComm("MultiAPCfg", 0, field, raw, sizeof(raw));
    (void)ShmPoolUnlock();
    if (status)
        return -1;
    *value = (int32_t)((uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24);
    return 0;
}

/* Live write of one MultiAPCfg scalar under the pool lock, read back (0 = ok). */
int controller_write_cfg(const char *field, int value)
{
    extern void *dbFindTbl(const char *);
    void *table = dbFindTbl("MultiAPCfg");
    char text[16];
    int status, check = value + 1;
    if (!table || ShmPoolLock(1000))
        return -1;
    snprintf(text, sizeof(text), "%d", value);
    status = dbSetValCommByRowNo(table, 0, field, text);
    (void)ShmPoolUnlock();
    return status || controller_read_cfg(field, &check) || check != value ? -1 : 0;
}

static int role_mode = CONTROLLER_ROLE_UNSET; /* mesh.role: leave the database as built, master, auto or slave */
void controller_set_role(int role)
{
    role_mode = role;
}

int controller_role(void)
{
    return role_mode;
}

/* Live write of P1905ComCfg.Enable, read back (0 = ok). The factory CmMultiApTopoSetP1905ComCfg writes the view and
 * then sends 0xa93d to cspd's multiapd_mgr, which restarts multiapd; the caller does the restart. */
int controller_set_v2_live(int enable)
{
    extern void *dbFindTbl(const char *);
    void *table = dbFindTbl("P1905ComCfg");
    int status;
    if (!table || ShmPoolLock(1000))
        return -1;
    status = dbSetValCommByRowNo(table, 0, "Enable", enable ? "1" : "0");
    (void)ShmPoolUnlock();
    return status || controller_read_v2() != (enable ? 1 : 0) ? -1 : 0;
}

/* Live write of WLANSetting.DualBandSync and ESSIDSuffix (the fields the factory OBJ_WLANSETTING set writes when the
 * dual-band switch changes, FEATURE_MAP section 4), read back; 0 = ok. The names themselves follow through the profile
 * bridge. */
int controller_set_wlan_setting_live(int dual_band_sync, const char *essid_suffix)
{
    extern void *dbFindTbl(const char *);
    void *table = dbFindTbl("WLANSetting");
    uint8_t flag[16] = {0};
    char suffix[300] = "";
    int status;
    if (!table || !essid_suffix || strlen(essid_suffix) > 8 || ShmPoolLock(1000))
        return -1;
    status = dbSetValCommByRowNo(table, 0, "DualBandSync", dual_band_sync ? "1" : "0") ||
             dbSetValCommByRowNo(table, 0, "ESSIDSuffix", essid_suffix);
    (void)ShmPoolUnlock();
    if (status || dbGetValComm("WLANSetting", 0, "DualBandSync", flag, sizeof(flag)) ||
        dbGetValComm("WLANSetting", 0, "ESSIDSuffix", suffix, sizeof(suffix)))
        return -1;
    return (flag[0] ? 1 : 0) != (dual_band_sync ? 1 : 0) || strcmp(suffix, essid_suffix) ? -1 : 0;
}

static struct wlan_acl_rule acl_rules[WLAN_ACL_RULES];
static unsigned int acl_count;
static int acl_policy[2][4] = { {-1, -1, -1, -1}, {-1, -1, -1, -1} };
void controller_set_acl(const struct wlan_acl_rule *rules, unsigned int count, const int policy[2][4])
{
    if (count > WLAN_ACL_RULES)
        count = WLAN_ACL_RULES;
    memset(acl_rules, 0, sizeof(acl_rules));
    if (count)
        memcpy(acl_rules, rules, count * sizeof(*rules));
    acl_count = count;
    memcpy(acl_policy, policy, sizeof(acl_policy));
}

static uint8_t main_hidden[2], main_beacon[2] = { 3, 3 };
void controller_set_main_style(const uint8_t hidden[2], const uint8_t beacon_type[2])
{
    memcpy(main_hidden, hidden, sizeof(main_hidden));
    memcpy(main_beacon, beacon_type, sizeof(main_beacon));
}

static uint8_t extra_bss_set[2][4], extra_bss_enabled[2][4], extra_bss_hidden[2][4], extra_bss_beacon[2][4];
static char extra_bss_ssid[2][4][32], extra_bss_key[2][4][64];
void controller_set_extra_bss_style(const uint8_t hidden[2][4], const uint8_t beacon_type[2][4])
{
    memcpy(extra_bss_hidden, hidden, sizeof(extra_bss_hidden));
    memcpy(extra_bss_beacon, beacon_type, sizeof(extra_bss_beacon));
}
void controller_set_extra_bss(const uint8_t set[2][4], const uint8_t enabled[2][4],
                              const char ssid[2][4][32], const char key[2][4][64])
{
    memcpy(extra_bss_set, set, sizeof(extra_bss_set));
    memcpy(extra_bss_enabled, enabled, sizeof(extra_bss_enabled));
    memcpy(extra_bss_ssid, ssid, sizeof(extra_bss_ssid));
    memcpy(extra_bss_key, key, sizeof(extra_bss_key));
}

static int startup_identity(const struct mb_profile profiles[2], const char *version_mode)
{
    void *communication, *interface, *cfg, *acd, *bss, *base_table, *wep, *devices, *acl;
    void *roam_caton, *wlan_setting, *cache_mac;
    static const struct exact_view roam_caton_view = { "MultiAPRoamCatonCfg", "MultiAPRoamCatonCfg", "IGD",
        "MultiAPRoamCatonCfg", 232, 1, 1, 12, 1 };
    static const struct exact_view wlan_setting_view = { "WLANSetting", "WLANSetting", "", "", 520, 1, 1, 63, 1 };
    static const struct exact_view cache_mac_view = { "MultiAPCacheMac", "DEV", "IGD", "MultiAPCacheMac", 108, 200, 200, 63, 0 };
    int result = -1;
    unsigned int slot;
    extern void *dbFindTbl(const char *);
    extern int WlanIndex2InstId(unsigned int, unsigned int, char *);
    extern int WlanCardIndex2BaseInstId(unsigned int, char *);
    if (ShmPoolLock(1000))
        return -1;
    if (mesh_policies_startup_rows() || startup_view("P1905ComCfg", startup_P1905ComCfg, 8, 328, 1, &communication) ||
        startup_view("IPInterface", startup_IPInterface, 48, 2404, 25, &interface) ||
        startup_view("P1905_ACD", startup_P1905_ACD, 9, 456, 1, &acd) ||
        startup_view("P1905AcdBssCfg", startup_P1905AcdBssCfg, 5, 264, 8, &bss) ||
        startup_view("AclCfg", startup_AclCfg, 5, 268, 256, &acl) ||
        startup_view("WLANWEP", startup_WLANWEP, 3, 232, 32, &wep) ||
        startup_table("P1905DeviceInfo", startup_P1905DeviceInfo, 11, 788, 1024, 0, &devices) ||
        startup_exact_view(&roam_caton_view, startup_MultiAPRoamCatonCfg, 7, &roam_caton) ||
        startup_exact_view(&wlan_setting_view, startup_WLANSetting, 14, &wlan_setting) ||
        startup_exact_view(&cache_mac_view, startup_MultiAPCacheMac, 2, &cache_mac))
        goto done;
    /* Factory baseline of the unmodified router. Without these the first PolicyConfigReq loses its
     * Roam Caton TLV (get RoamCatonCfg error, view status -2) and the M2 loses its MLO TLV body.
     * MultiAPCacheMac stays empty (RowCount 0 on the factory router). */
    if (dbSetValCommByRowNo(roam_caton, 0, "TxBaDelayMs", "10") ||
        dbSetValCommByRowNo(roam_caton, 0, "TxConDelayMs", "100") ||
        /* WlanSettingDbRead asks for the instance DEV.WIFI.SETTING: the key must match exactly. */
        dbSetValCommByRowNo(wlan_setting, 0, "ViewName", "DEV.WIFI.SETTING") ||
        dbSetValCommByRowNo(wlan_setting, 0, "DualBandSync", wlan_dual_band_sync ? "1" : "0") ||
        dbSetValCommByRowNo(wlan_setting, 0, "SSIDSync", "1") ||
        dbSetValCommByRowNo(wlan_setting, 0, "ESSIDSuffix", wlan_essid_suffix) ||
        dbSetValCommByRowNo(wlan_setting, 0, "MLOEnable", wlan_mlo_enable ? "1" : "0") ||
        dbSetValCommByRowNo(wlan_setting, 0, "MLOSSID", "1") ||
        dbSetValCommByRowNo(wlan_setting, 0, "TimerStatus", "1") ||
        dbSetValCommByRowNo(wlan_setting, 0, "MinScanInterval", "30") ||
        dbSetValCommByRowNo(wlan_setting, 0, "BootScanDuration", "21600") ||
        dbSetValCommByRowNo(wlan_setting, 0, "ScanInterval", "86400"))
        goto done;
    mesh_policies_apply_profile_view("MultiAPRoamCatonCfg", roam_caton);
    mesh_policies_apply_profile_view("WLANSetting", wlan_setting);
    (void)cache_mac;
    printf("ZTE_CONTROLLER_STARTUP roam_caton_view=1 wlan_setting_view=1 cache_mac_view=1 dual_band_sync=%d mlo_enable=%d\n",
           wlan_dual_band_sync, wlan_mlo_enable);
    cfg = dbFindTbl("MultiAPCfg");
    if (!cfg || dbSetValCommByRowNo(cfg, 0, "IFLink", "br0") ||
        /* Explicit policy supplied by the caller, never the unknown product default. */
        dbSetValCommByRowNo(cfg, 0, "VersionMode", version_mode) ||
        /* SR1010 is a wired gateway with no Wi-Fi radio, so bRadioStatusOn() would
         * report every radio down and acdGetWLANCFG would mark all BSSes inactive,
         * making the controller push a tear-down M2 to every agent. SSIDSyncRadio
         * is exactly the vendor's wired-controller switch: acdGetMapCfgSSIDSyncRadio
         * short-circuits bRadioStatusOn to "on" so config is synced to agents
         * instead of torn down. (Confirmed by disassembly of the SR1010 multiapd.) */
        dbSetValCommByRowNo(cfg, 0, "SSIDSyncRadio", "1") ||
        dbSetValCommByRowNo(communication, 0, "Enable", p1905_v2_enable ? "1" : "0") ||
        dbSetValCommByRowNo(interface, 0, "ViewName", "DEV.IP.IF1") ||
        dbSetValCommByRowNo(interface, 0, "Enable", "1") ||
        dbSetValCommByRowNo(interface, 0, "Name", "br0") ||
        dbSetValCommByRowNo(interface, 0, "IsBridge", "1") ||
        dbSetValCommByRowNo(interface, 0, "MaxMTUSize", "1500"))
        goto done;
    printf("ZTE_CONTROLLER_STARTUP p1905_v2=%d\n", p1905_v2_enable);
    {
        /* _baseCheck1905Packet gates every peer on a "market sign" (P1905ComCfg
         * MarketSign, guarded by MarketConfEn): the same multiapd rejects a peer
         * whose sign differs from ours. The real child enforces it (MarketConfEn=1,
         * MarketSign=<region code>), so to interoperate we must advertise and accept
         * the same public region code. Opt-in via env; the lab/offline paths leave
         * MarketConfEn=0 and the sign empty, so nothing changes there. */
        const char *market_sign = getenv("ZTE_MESH_REAL_MARKET_SIGN");
        if (market_sign && *market_sign) {
            if (dbSetValCommByRowNo(communication, 0, "MarketConfEn", "1") ||
                dbSetValCommByRowNo(communication, 0, "MarketSign", market_sign))
                goto done;
            printf("ZTE_CONTROLLER_STARTUP market_conf_en=1 market_sign=%s\n", market_sign);
        }
    }
    mesh_policies_apply_profile_view("P1905ComCfg", communication);
    mesh_policies_apply_profile_view("MultiAPCfg", cfg);
    /* mesh.role: the settled state of the factory device-role page. "Master" is SetRoleEnable 0, "adaptive" is
     * SetRoleEnable 1 with the role the election ended on (Mode 2 for a router); Mode 0 only exists during an election.
     * "Slave" is the agent side of the same switch (CM API Mode 1 = Slave, 0 = adaptive, 2 = Master): it also has to
     * clear EnMasterOnly, which the fixture sets because a radio-less controller must skip slave_init. */
    if (role_mode == CONTROLLER_ROLE_SLAVE) {
        if (dbSetValCommByRowNo(cfg, 0, "Mode", "1") ||
            dbSetValCommByRowNo(cfg, 0, "SetRoleEnable", "0") ||
            dbSetValCommByRowNo(cfg, 0, "EnMasterOnly", "0") ||
            /* Registry default "not attached": slave_init sets CurHop itself (CfMultiapSetCurHop). The fixture value 2
             * is the stand-in for that call on the controller side and must not leak into the agent path. */
            dbSetValCommByRowNo(cfg, 0, "CurHop", "-2"))
            goto done;
        printf("ZTE_CONTROLLER_STARTUP role=slave mode=1 set_role_enable=0 en_master_only=0 cur_hop=-2\n");
    } else if (role_mode >= 0 &&
               (dbSetValCommByRowNo(cfg, 0, "Mode", "2") ||
                dbSetValCommByRowNo(cfg, 0, "SetRoleEnable", role_mode == CONTROLLER_ROLE_AUTO ? "1" : "0")))
        goto done;
    base_table = dbFindTbl("WLANBase");
    if (!base_table || dbSetValCommByRowNo(acd, 0, "ACDSyncEnable", "1")) goto done;
    for (slot = 0; slot < 2; slot++) {
        char ap[32], base[32];
        if (slot && !dbAddRow(bss)) goto done;
        /* acdInitBssCfgList uses the pinned "DEV" prefix, not an arbitrary key. */
        if (WlanIndex2InstId(slot, 0, ap) || WlanCardIndex2BaseInstId(slot, base) ||
            dbSetValCommByRowNo(bss, slot, "ViewName", ap) ||
            dbSetValCommByRowNo(bss, slot, "WLANCfgInst", ap) ||
            dbSetValCommByRowNo(bss, slot, "WLANBaseName", base) ||
            /* CustomTag 68 = bits 2 and 6, see create_extra_slot_rows(); the enable bit comes live from WLANCfg. */
            dbSetValCommByRowNo(bss, slot, "CustomTag", "68") ||
            dbSetValCommByRowNo(bss, slot, "Enable", "1") ||
            dbSetValCommByRowNo(base_table, slot, "Band", slot ? "1" : "0") ||
            dbSetValCommByRowNo(base_table, slot, "RadioStatus", "1")) goto done;
        /* acdGetWLANCFG reads all four WEP slots before the WPA2 PSK.
         * They are genuinely empty/unused rows, not a successful getter stub. */
        {
            unsigned int key_slot;
            for (key_slot = 0; key_slot < 4; key_slot++) {
                char name[32];
                unsigned int row = slot * 4 + key_slot;
                if (row && !dbAddRow(wep)) goto done;
                if (snprintf(name, sizeof(name), "%s.WEP%u", ap, key_slot + 1) >= (int)sizeof(name) ||
                    dbSetValCommByRowNo(wep, row, "ViewName", name) ||
                    dbSetValCommByRowNo(wep, row, "WLANViewName", ap) ||
                    dbSetValCommByRowNo(wep, row, "Key", "")) goto done;
            }
        }
    }
    {
        unsigned int radio, bss_row = 2, wep_row = 8, extras = 0;
        for (radio = 0; radio < 2; radio++) {
            for (slot = 1; slot < 4; slot++) {
                if (!extra_bss_set[radio][slot])
                    continue;
                if (create_extra_slot_rows(radio, slot, extra_bss_enabled[radio][slot], extra_bss_ssid[radio][slot],
                                           extra_bss_key[radio][slot], extra_bss_hidden[radio][slot],
                                           extra_bss_beacon[radio][slot], bss_row, wep_row))
                    goto done;
                bss_row++;
                wep_row += 4;
                extras++;
            }
        }
        printf("ZTE_CONTROLLER_STARTUP extra_bss=%u\n", extras);
    }
    {
        /* MAC filter: WLAN_ACL_ROWS pre-created AclCfg rows (edited in place later, never
         * added/removed while the daemon runs), plus the configured per-slot policies. */
        unsigned int row, radio;
        for (row = 1; row < WLAN_ACL_ROWS; row++)
            if (!dbAddRow(acl))
                goto done;
        if (wlan_profiles_acl_apply(acl_rules, acl_count, 0))
            goto done;
        for (radio = 0; radio < 2; radio++)
            for (slot = 0; slot < 4; slot++)
                if (acl_policy[radio][slot] >= 0 &&
                    wlan_profiles_set_acl_policy(radio, slot, acl_policy[radio][slot], 0))
                    goto done;
        printf("ZTE_CONTROLLER_STARTUP acl_rules=%u\n", acl_count);
    }
    printf("ZTE_CONTROLLER_STARTUP acd_bss_rows=2 logical_radio=1 hardware_claimed=0\n");
    result = 0;
done:
    (void)ShmPoolUnlock();
    if (!result) {
        struct mb_profile profile;
        struct mb_result response = {0};
        unsigned int radio;
        for (radio = 0; radio < 2; radio++) {
            profile = profiles[radio];
            profile.radio = (uint8_t)radio;
            if (wlan_profiles_bridge_apply(&profile, 0, &response)) {
                result = -1;
                break;
            }
        }
        /* the bridge writes BeaconType = WPA2: the configured style of the primary slots goes on top */
        for (radio = 0; radio < 2 && !result; radio++)
            if (wlan_profiles_set_main_style(radio, main_hidden[radio], main_beacon[radio]))
                result = -1;
        mb_wipe(&profile, sizeof(profile)); mb_wipe(&response, sizeof(response));
    }
    return result;
}

static int verify_route_adapter(void)
{
    struct { uint32_t pre[4]; uint8_t data[532]; uint32_t post[4]; } guarded;
    uint8_t snapshot[532];
    unsigned int i;
    int result = -1, fd = -1;
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a) }, actual, original;
    socklen_t address_bytes = sizeof(actual);
    void *library = dlopen("/lib/libzte_controller_route.so", RTLD_NOW | RTLD_LOCAL);
    int (*getter)(const char *, void *) = library ? dlsym(library, "GetRouteIFInfo") : NULL;
    int (*binder)(int, const struct sockaddr *, socklen_t) = library ? dlsym(library, "bind") : NULL;
    if (!getter || !binder)
        goto done;
    memset(&guarded, 0xa5, sizeof(guarded));
    if (getter("DEV.IP.IF1", guarded.data) || strcmp((char *)guarded.data, "DEV.IP.IF1") ||
        strcmp((char *)guarded.data + 32, "br0") ||
        memcmp(guarded.data + 172, "\x02\x00\x00\x00\x19", 5)) /* AL MAC prefix only: the last byte is ZTE_MESH_AL_LAST */
        goto done;
    for (i = 0; i < 4; i++)
        if (guarded.pre[i] != 0xa5a5a5a5 || guarded.post[i] != 0xa5a5a5a5)
            goto done;
    memcpy(snapshot, guarded.data, sizeof(snapshot));
    if (getter("DEV.IP.IF99", guarded.data) != -1 ||
        memcmp(snapshot, guarded.data, sizeof(snapshot)))
        goto done;
    target.sll_ifindex = (int)if_nametoindex("br0"); original = target;
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x893a));
    if (!target.sll_ifindex || fd < 0 || binder(fd, (struct sockaddr *)&target, sizeof(target)) ||
        getsockname(fd, (struct sockaddr *)&actual, &address_bytes) ||
        actual.sll_ifindex != (int)if_nametoindex("veth-lab") ||
        actual.sll_protocol != htons(0x893a) || memcmp(&target, &original, sizeof(target)))
        goto done;
    if (binder(-1, (struct sockaddr *)&target, sizeof(target)) != -1 || errno != EBADF ||
        memcmp(&target, &original, sizeof(target)))
        goto done;
    /* Other protocols must keep their original binding, not use the mapper. */
    target.sll_protocol = htons(0x0800); address_bytes = sizeof(actual);
    if (binder(fd, (struct sockaddr *)&target, sizeof(target)) ||
        getsockname(fd, (struct sockaddr *)&actual, &address_bytes) ||
        actual.sll_ifindex != target.sll_ifindex || actual.sll_protocol != htons(0x0800))
        goto done;
    result = 0;
    printf("ZTE_CONTROLLER_STARTUP real_route_adapter=1 buffer_guard=1 unknown_interface_rejected=1 actual_lab_bind_verified=1 bind_errors_preserved=1 other_protocol_unchanged=1\n");
done:
    if (fd >= 0) close(fd);
    if (library)
        dlclose(library);
    return result;
}

static int verify_profile_adapter(void)
{
    struct { uint32_t pre[4]; uint8_t data[2496]; uint32_t post[4]; } ap;
    struct { uint32_t pre[4]; uint8_t data[324]; uint32_t post[4]; } psk;
    uint8_t saved[2496], native_psk[324], before_cfg[2312], after_cfg[2312];
    uint32_t size = 2312, magic = 0x01020304, mode = 3, band;
    uint16_t selected = 1;
    char key[32], psk_key[32];
    unsigned int radio, i;
    int result = -1;
    void *library = dlopen("/lib/libzte_controller_profile.so", RTLD_NOW | RTLD_LOCAL);
    int (*getap)(const char *, void *) = library ? dlsym(library, "CmWLANGetAPConfig") : NULL;
    int (*getpsk)(const char *, void *) = library ? dlsym(library, "CmWLANGetPsk") : NULL;
    int (*factory_psk)(const char *, void *) = dlsym(RTLD_DEFAULT, "CmWLANGetPsk");
    extern int WlanIndex2InstId(unsigned int, unsigned int, char *);
    extern int WlanIndex2PSKInstId(unsigned int, unsigned int, char *);
    extern int dbAPIGetView(const char *, const char *, void *);
    if (!getap || !getpsk || !factory_psk) goto done;
    for (radio = 0; radio < 2; radio++) {
        if (WlanIndex2InstId(radio, 0, key) || WlanIndex2PSKInstId(radio, 0, psk_key)) goto done;
        memset(before_cfg, 0, sizeof(before_cfg));
        memcpy(before_cfg, &size, 4); memcpy(before_cfg + 4, &magic, 4);
        if (dbAPIGetView("WLANCfg", key, before_cfg)) goto done;
        memset(&ap, 0xa5, sizeof(ap)); memset(ap.data, 0, sizeof(ap.data));
        memcpy(ap.data + 2306, &selected, 2); memcpy(ap.data + 2332, &mode, 4);
        if (getap(key, ap.data) || strcmp((char *)ap.data + 444, "zte-controller-test")) goto done;
        memcpy(&band, ap.data + 60, 4);
        if (band != radio || memcmp(ap.data + 2306, &selected, 2) || memcmp(ap.data + 2332, &mode, 4)) goto done;
        memset(&psk, 0xa5, sizeof(psk)); memset(psk.data, 0, sizeof(psk.data));
        memset(native_psk, 0, sizeof(native_psk));
        {
            int adapter = getpsk(psk_key, psk.data), native = factory_psk(psk_key, native_psk);
            unsigned int differences = 0, first = sizeof(native_psk);
            for (i = 0; i < sizeof(native_psk); i++)
                if (native_psk[i] != psk.data[i]) { differences++; if (first == sizeof(native_psk)) first = i; }
            if (adapter || native || strcmp((char *)psk.data + 188, "public-test-only-key") || differences) {
                fprintf(stderr, "ZTE_CONTROLLER_STARTUP fail=psk-equivalence adapter_status=%d native_status=%d differing_bytes=%u first_offset=%u payload_logged=0\n",
                        adapter, native, differences, first);
                goto done;
            }
        }
        for (i = 0; i < 4; i++)
            if (ap.pre[i] != 0xa5a5a5a5 || ap.post[i] != 0xa5a5a5a5 ||
                psk.pre[i] != 0xa5a5a5a5 || psk.post[i] != 0xa5a5a5a5) goto done;
        memset(after_cfg, 0, sizeof(after_cfg));
        memcpy(after_cfg, &size, 4); memcpy(after_cfg + 4, &magic, 4);
        if (dbAPIGetView("WLANCfg", key, after_cfg) || memcmp(before_cfg, after_cfg, sizeof(before_cfg))) goto done;
        memcpy(saved, ap.data, sizeof(saved));
        if (!getap("UNAPPROVED", ap.data) || memcmp(saved, ap.data, sizeof(saved)) ||
            !getap(NULL, ap.data) || memcmp(saved, ap.data, sizeof(saved)) || !getap(key, NULL)) goto done;
        memcpy(saved, psk.data, sizeof(psk.data));
        if (!getpsk("UNAPPROVED", psk.data) || memcmp(saved, psk.data, sizeof(psk.data)) ||
            !getpsk(NULL, psk.data) || !getpsk(key, NULL)) goto done;
    }
    result = 0;
    printf("ZTE_CONTROLLER_STARTUP real_cm_profiles=1 readonly=1 buffer_guard=1 rejected_unchanged=1 native_psk_equivalent=1\n");
done:
    mb_wipe(&ap, sizeof(ap)); mb_wipe(&psk, sizeof(psk)); mb_wipe(saved, sizeof(saved));
    mb_wipe(native_psk, sizeof(native_psk)); mb_wipe(before_cfg, sizeof(before_cfg)); mb_wipe(after_cfg, sizeof(after_cfg));
    if (library) dlclose(library);
    return result;
}

extern int SubShmSrvInit(void);
extern int SubShmLock(unsigned int);
extern int SubShmUnlock(void);
extern void *gptSrvList;

/* Start the real daemon (first boot and respawn alike): same preloads, same environment as the supervisor's. */
static unsigned int daemon_starts;
static int spawn_daemon(pid_t *child)
{
    char count[16];
    /* cspd's multiapd_mgr counts every start, see controller-route.c. The factory sends the first Renew pair only after a
     * RE-start (count above 1), because at its first start no agent can know the controller yet. Here a controller instance
     * often starts while an agent that onboarded against an earlier instance is up (the service restarted, or the router
     * rebooted alone): without the Renew pair that agent keeps its old session, is never flagged as a ZTE device in the
     * database (no 0200fe, no channel/power push, no node actions) and does not learn the new profile. So every start counts
     * as a restart; a joining agent just sees one extra Renew 3 s after the daemon's first received frame. */
    snprintf(count, sizeof(count), "%u", daemon_starts + 2);
    fflush(NULL);
    *child = fork();
    if (*child < 0)
        return -1;
    if (!*child) {
        if (setenv("ZTE_MESH_START_COUNT", count, 1))
            _exit(82);
        if (setenv("LD_PRELOAD", "/lib/libzte_controller_profile.so:/lib/libzte_controller_route.so:/lib/libzte_controller_trace.so:/lib/libzte_mesh_shim.so:/lib/libzte_agent_mm.so", 1))
            _exit(82);
        execl("/lib/ld-linux.so.3", "ld-linux.so.3", "--library-path", "/lib",
              "/bin/multiapd", (char *)NULL);
        _exit(82);
    }
    daemon_starts++;
    return 0;
}

/* The factory multiapd_mgr restart (cspd MultiapRestart: stop the process, start it again as soon as it is gone; the
 * database and the subscriber pool live in cspd and stay). The daemon dies by SIGKILL in the factory log
 * (exit_code = 9). Here the pool belongs to this process, so only the daemon is replaced: 0 = a new daemon runs,
 * -2/-3 = the pool lock stayed held by the killed daemon (nothing was started), -1 = kill/wait/fork failed. */
int controller_respawn_daemon(pid_t *child)
{
    int status;
    pid_t old = *child;
    if (old > 0) {
        (void)kill(old, SIGKILL);
        while (waitpid(old, &status, 0) < 0) {
            unsigned int tries;
            if (errno == EINTR)
                continue;
            if (errno != ECHILD)
                return -1;
            /* not our child (already reaped, or SIGCHLD ignored): wait until the process is really gone */
            for (tries = 0; tries < 30 && !kill(old, 0); tries++) {
                struct timespec pause_time = {0, 100000000L};
                nanosleep(&pause_time, NULL);
            }
            if (!kill(old, 0))
                return -1;
            break;
        }
        *child = 0;
    }
    if (ShmPoolLock(1000))
        return -2;
    (void)ShmPoolUnlock();
    if (SubShmLock(1000))
        return -3;
    (void)SubShmUnlock();
    return spawn_daemon(child);
}

/* Shared by the lab observation and the long-running service: private DB
 * identity, real entropy, real subscriber server, then the real daemon. */
int controller_boot(const struct mb_profile profiles[2], const char *version_mode, int lab,
                    pid_t *child, int *shm_id)
{
    uint8_t randomness[32];
    extern int RAND_bytes(unsigned char *, int);
    struct shmid_ds info_shm;
    int status, id;
    if (startup_identity(profiles, version_mode)) {
        printf("ZTE_CONTROLLER_STARTUP fail=startup-identity errno=%d\n", errno);
        fflush(NULL);
        return -1;
    }
    if (verify_route_adapter()) {
        printf("ZTE_CONTROLLER_STARTUP fail=route-adapter errno=%d\n", errno);
        fflush(NULL);
        return -1;
    }
    if (lab && verify_profile_adapter()) {
        printf("ZTE_CONTROLLER_STARTUP fail=profile-adapter errno=%d\n", errno);
        fflush(NULL);
        return -1;
    }
    /* Validate real legacy OpenSSL RNG, never stub deterministic randomness. */
    status = RAND_bytes(randomness, sizeof(randomness));
    mb_wipe(randomness, sizeof(randomness));
    if (status != 1)
        return -1;
    printf("ZTE_CONTROLLER_STARTUP kernel_entropy=1 native_crypto_rng=1\n");
    errno = 0;
    if (shmget(43947, 0, 0) != -1 || errno != ENOENT)
        return -1;
    (void)SubShmSrvInit(); /* Real factory server initializes pool + semaphore. */
    id = shmget(43947, 0, 0);
    if (id < 0 || shmctl(id, IPC_STAT, &info_shm) || info_shm.shm_segsz != 0x20000 ||
        gptSrvList != (void *)0x68000000 ||
        ((void **)gptSrvList)[0] != gptSrvList || ((void **)gptSrvList)[1] != gptSrvList ||
        SubShmLock(1000))
        return -1;
    (void)SubShmUnlock();
    printf("ZTE_CONTROLLER_STARTUP real_subscriber_server=1 shared_bytes=131072 lock_verified=1\n");
    if (spawn_daemon(child))
        return -1;
    *shm_id = id;
    printf("ZTE_CONTROLLER_STARTUP real_multiapd=1 private_db=1 private_net_raw=1 host_network=0\n");
    fflush(NULL);
    return 0;
}

int controller_shutdown_private(int shm_id)
{
    return shmctl(shm_id, IPC_RMID, NULL) || shmdt((void *)0x68000000) ? -1 : 0;
}

int controller_startup(void)
{
    static const struct mb_profile fixture = { .request_id = 1, .security = MB_WPA2_PSK_AES, .enabled = 1,
        .ssid_length = 19, .key_length = 20, .ssid = "zte-controller-test", .key = "public-test-only-key" };
    struct mb_profile profiles[2] = { fixture, fixture };
    struct timespec start, now, pause_time = {0, 20000000};
    siginfo_t info;
    int status, id = -1, timed = 0, discovery_attempted = 0, query_attempted = 0;
    unsigned int autoconfig_attempted = 0;
    int wsc_attempted = 0, renew_attempted = 0;
    unsigned long nodes_hash = 0;
    pid_t child;
    /* Explicit standard-MAP lab policy (bit1: no ZTE vendor TLVs). */
    if (controller_boot(profiles, "2", 1, &child, &id) || clock_gettime(CLOCK_MONOTONIC, &start))
        return -1;
    for (;;) {
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT)) {
            if (errno == EINTR)
                continue;
            return -1; /* Outer namespace watchdog cleans the entire group. */
        }
        if (info.si_pid == child)
            break;
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            return -1;
        if (!discovery_attempted && now.tv_sec - start.tv_sec >= 8) {
            discovery_attempted = 1;
            status = controller_inject_packet(0);
            printf("ZTE_CONTROLLER_PACKET fixture=topology-discovery private_only=1 send_status=%d\n", status);
            fflush(NULL);
        }
        if (!query_attempted && now.tv_sec - start.tv_sec >= 12) {
            query_attempted = 1;
            status = controller_inject_packet(1);
            printf("ZTE_CONTROLLER_PACKET fixture=topology-query private_only=1 send_status=%d\n", status);
            fflush(NULL);
        }
        if (!wsc_attempted && autoconfig_attempted && now.tv_sec - start.tv_sec >= 18) {
            wsc_attempted = 1;
            status = controller_inject_wsc_m1();
            printf("ZTE_CONTROLLER_PACKET fixture=wsc-exchange private_only=1 response_status=%d\n", status);
            fflush(NULL);
        }
        if (!renew_attempted && wsc_attempted && now.tv_sec - start.tv_sec >= 26) {
            struct mb_profile changed[2] = { fixture, fixture };
            struct mb_result applied = {0};
            unsigned int radio;
            int failed = 0, renew, listener;
            renew_attempted = 1;
            listener = controller_cmdu_listener();
            for (radio = 0; radio < 2 && !failed; radio++) {
                memcpy(changed[radio].ssid, "zte-controller-new", 19);
                changed[radio].ssid_length = 18;
                changed[radio].radio = (uint8_t)radio;
                failed = wlan_profiles_bridge_apply(&changed[radio], 0, &applied);
            }
            renew = failed ? -1 : controller_renew_agents();
            status = controller_wait_cmdu(listener, 10, 8000); /* even a non-zero CM status may have reached the daemon */
            printf("ZTE_CONTROLLER_PACKET fixture=autoconfig-renew private_only=1 profile_update=%d renew_status=%d peer_received=%d cmdu=10\n",
                   !failed, renew, !status);
            fflush(NULL);
            mb_wipe(changed, sizeof(changed));
        }
        if (autoconfig_attempted < 2 && now.tv_sec - start.tv_sec >= 16 + 6 * (long)autoconfig_attempted) {
            unsigned int band = autoconfig_attempted++;
            status = controller_inject_packet(2 + (int)band);
            printf("ZTE_CONTROLLER_PACKET fixture=autoconfig-search private_only=1 band=%u response_status=%d\n", band, status);
            fflush(NULL);
        }
        dump_nodes(&nodes_hash);
        if (now.tv_sec - start.tv_sec >= 40) {
            timed = 1;
            (void)kill(child, SIGKILL);
            break;
        }
        nanosleep(&pause_time, NULL);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    printf("ZTE_CONTROLLER_STARTUP observed=1 timeout_stop=%d exited=%d exit=%d signal=%d controller_ready=0\n",
           timed, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(NULL);
    /* Observation is not an operational Controller success indication. */
    return controller_shutdown_private(id);
}

/* WLANCfg / WLANPSK as the database holds them, for the agent observation: the SSID, the enable bit and (for the key row)
 * the key LENGTH only. Nothing that could be a credential is printed. */
static void dump_wlan_rows(const char *label)
{
    unsigned int row;
    char line[512];
    size_t used = 0;
    if (ShmPoolLock(1000))
        return;
    for (row = 0; row < 8 && used < sizeof(line) - 96; row++) {
        char ssid[64] = "", key[128] = "";
        uint8_t enable = 0, access = 0;
        if (dbGetValComm("WLANCfg", row, "ESSID", ssid, sizeof(ssid)))
            ssid[0] = 0;
        if (dbGetValComm("WLANCfg", row, "Enable", &enable, sizeof(enable)))
            enable = 0;
        if (dbGetValComm("WLANCfg", row, "MapAccessAttr", &access, sizeof(access)))
            access = 0;
        if (dbGetValComm("WLANPSK", row, "PreSharedKey", key, sizeof(key)))
            key[0] = 0;
        used += (size_t)snprintf(line + used, sizeof(line) - used, "[%u ssid=%s enable=%u access=%u key_len=%u]",
                                 row, ssid, enable, access, (unsigned int)strlen(key));
        mb_wipe(key, sizeof(key));
    }
    (void)ShmPoolUnlock();
    printf("ZTE_AGENT_WLAN %s rows=%s\n", label, line);
    fflush(NULL);
}

/* WLANPSK read back through the view API, which is how the daemon itself addresses the row (by instance id,
 * not by row number - dbGetValComm() with a row index reports nothing for this table even after it is
 * written). The key lands at offset 260 of the 328-byte row (wlan_psk.def: ViewName, WLANViewName,
 * PreSharedKey, KeyPassphrase); only its length is printed, never the key, plus whether it equals the one the
 * M2 carried - the length alone would not rule out a coincidence. */
static void dump_psk_rows(const char *label, const char *expected_24g, const char *expected_5g)
{
    char line[384], key[32], buffer[328];
    const char *expected[2] = { expected_24g, expected_5g };
    size_t used = 0;
    unsigned int radio;
    line[0] = '\0';
    for (radio = 0; radio < 2 && used < sizeof(line) - 96; radio++) {
        uint32_t size = sizeof(buffer), magic = 0x01020304;
        int status, matches = 0;
        size_t key_length = 0;
        char view[36] = "";
        extern int WlanIndex2PSKInstId(unsigned int, unsigned int, char *);
        extern int dbAPIGetView(const char *, const char *, void *);
        if (WlanIndex2PSKInstId(radio, 0, key))
            continue;
        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer, &size, 4);
        memcpy(buffer + 4, &magic, 4);
        status = dbAPIGetView("WLANPSK", key, buffer);
        if (!status) {
            key_length = strnlen(buffer + 260, 64);
            memcpy(view, buffer + 100, sizeof(view) - 1);
            matches = expected[radio] && !strcmp(buffer + 260, expected[radio]);
        }
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 "[%s status=%d view=%s key_len=%u m2_match=%d]",
                                 key, status, status ? "-" : view, (unsigned int)key_length, matches);
        mb_wipe(buffer, sizeof(buffer));
    }
    printf("ZTE_AGENT_PSK %s rows=%s\n", label, line);
    fflush(NULL);
}

/* MAC of a local interface (the daemon's AL MAC is br0's, it binds its 1905 socket to that device). */
static int local_mac(const char *name, uint8_t out[6])
{
    struct ifreq interface;
    int fd, result, saved;
    memset(&interface, 0, sizeof(interface));
    snprintf(interface.ifr_name, sizeof(interface.ifr_name), "%s", name);
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    result = ioctl(fd, SIOCGIFHWADDR, &interface);
    saved = errno;
    close(fd);
    errno = saved;
    if (result || interface.ifr_hwaddr.sa_family != 1)
        return -1;
    memcpy(out, interface.ifr_hwaddr.sa_data, 6);
    return 0;
}

/* Which band an M1 is about, read from the radio identifier the daemon carries in its 0x85 TLV. This lab has
 * no radio hardware, so get_wlan_radio_info() answers from a hard-coded per-band table whose entries are
 * ASCII: "MAP24G" for 2.4 GHz and "MAP5GL" for 5 GHz. Returns -1 when the identifier is neither. */
static int radio_band(const uint8_t *radio, size_t bytes)
{
    if (!radio || bytes < 6)
        return -1;
    if (!memcmp(radio + 3, "24G", 3))
        return 0;
    if (!memcmp(radio + 3, "5GL", 3))
        return 1;
    return -1;
}

/* The interfaces this side can see, so "the daemon emitted nothing" and "we looked at the wrong interface" are
 * told apart in the log. Names, indexes and MACs only. */
static void dump_interfaces(void)
{
    struct if_nameindex *list = if_nameindex(), *entry;
    char line[512];
    size_t used = 0;
    if (!list)
        return;
    line[0] = 0;
    for (entry = list; entry->if_index; entry++) {
        struct ifreq interface;
        int fd;
        memset(&interface, 0, sizeof(interface));
        snprintf(interface.ifr_name, sizeof(interface.ifr_name), "%s", entry->if_name);
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0 && !ioctl(fd, SIOCGIFHWADDR, &interface) && interface.ifr_hwaddr.sa_family == 1) {
            const unsigned char *mac = (const unsigned char *)interface.ifr_hwaddr.sa_data;
            used += (size_t)snprintf(line + used, sizeof(line) - used, "[%u %s %02x:%02x:%02x:%02x:%02x:%02x]",
                                     entry->if_index, entry->if_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            used += (size_t)snprintf(line + used, sizeof(line) - used, "[%u %s]", entry->if_index, entry->if_name);
        }
        if (fd >= 0)
            close(fd);
        if (used > sizeof(line) - 64)
            break;
    }
    if_freenameindex(list);
    printf("ZTE_AGENT_IF interfaces=%s\n", line);
    fflush(NULL);
}

/* Lab observation of the agent side: the daemon boots as a Slave (Mode 1, SetRoleEnable 0, EnMasterOnly 0) and this side
 * is the synthetic controller - it answers the daemon's AP-autoconfiguration Search and runs the public WSC M1/M2
 * exchange, then reports what the M2 did to the WLANCfg/WLANPSK rows. Bounded; private netns only. */
int controller_agent_startup(void)
{
    /* The identity profile startup_identity() writes. verify_profile_adapter() asserts exactly these values,
     * so this one stays fixed across modes. */
    static const struct mb_profile identity = { .request_id = 1, .security = MB_WPA2_PSK_AES, .enabled = 1,
        .ssid_length = 19, .key_length = 20, .ssid = "zte-controller-test", .key = "public-test-only-key" };
    /* What the M2 carries: deliberately different from the identity, so a change in the WLANCfg/WLANPSK rows
     * can only have come from applying the controller's configuration (the AGENT_PROBE 6.5 criterion).
     * One profile per band, because the daemon enrols each radio with its own M1 - a single answer profile
     * cannot tell "the second M2 was applied to the right row" from "it was applied to the same row twice". */
    static const struct mb_profile answer_24g = { .request_id = 2, .security = MB_WPA2_PSK_AES, .enabled = 1,
        .ssid_length = 12, .key_length = 16, .ssid = "zte-agent-2g", .key = "m2-public-2g-key" };
    static const struct mb_profile answer_5g = { .request_id = 3, .security = MB_WPA2_PSK_AES, .enabled = 1,
        .ssid_length = 12, .key_length = 16, .ssid = "zte-agent-5g", .key = "m2-public-5g-key" };
    siginfo_t info;
    struct mb_profile profiles[2] = { identity, identity };
    struct timespec start, now, pause_time = {0, 100000000};
    struct controller_frame frame;
    unsigned char daemon[6] = {0}, al_mac[6] = {0}, have_daemon = 0;
    /* Supported Service TLV values to try as "Multi-AP Controller": the daemon declares 1 for itself as an
     * Agent, so the lab cycles the three plausible encodings until it stops re-searching. */
    static const unsigned int service_cycle[3] = { 0, 2, 1 };
    unsigned int seen = 0, i, search = 0, discovery = 0, topo_sent = 0, m1_bytes = 0;
    int status, id = -1, timed = 0, listener = -1, election_fired = 0, m1_seen = 0, debug_raised = 0;
    /* One M2 per radio: the daemon sends an M1 for every band it wants configured, and answering only the
     * first one is what left band 1 unconfigured in the earlier runs. */
    uint8_t answered[4][6];
    unsigned int answered_count = 0, band_applied[2] = { 0, 0 };
    long last_m1 = -1;
    /* Restart phase: the shared-memory engine belongs to this process, so a daemon restart is the only way
     * this lab can ask whether the agent keeps the profile it was just given. 1 = running, 2 = failed. */
    int reboot = 0;
    long reboot_at = -1;
    unsigned int reboot_m1 = 0;
    long last_topo = -1, next_sync = 0;
    int last_cmdu[8], last_mid[8];
    pid_t child;
    for (i = 0; i < 8; i++)
        last_cmdu[i] = last_mid[i] = -1;
    controller_set_role(CONTROLLER_ROLE_SLAVE);
    /* Open the listener first: the daemon may send its Search as soon as its acds task runs. */
    dump_interfaces();
    if (local_mac("br0", al_mac))
        memset(al_mac, 0, sizeof(al_mac));
    listener = controller_cmdu_listener();
    printf("ZTE_AGENT_WIRE listener=%d epoch=%ld\n", listener, (long)time(NULL));
    fflush(NULL);
    if (controller_boot(profiles, "2", 1, &child, &id) || clock_gettime(CLOCK_MONOTONIC, &start)) {
        printf("ZTE_AGENT_WIRE boot=failed errno=%d\n", errno);
        fflush(NULL);
        if (listener >= 0)
            close(listener);
        return -1;
    }
    dump_wlan_rows("before");
    dump_psk_rows("before", NULL, NULL);
    for (;;) {
        long elapsed;
        int read;
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT)) {
            if (errno == EINTR)
                continue;
            if (listener >= 0)
                close(listener);
            return -1;
        }
        if (info.si_pid == child) {
            printf("ZTE_AGENT_WIRE daemon_exited=1\n");
            fflush(NULL);
            break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            (void)kill(child, SIGKILL);
            continue;
        }
        elapsed = now.tv_sec - start.tv_sec;
        while ((read = controller_read_frame(listener, &frame)) == 1) {
            /* One line per new (CMDU, message id) pair: the observation is "which frames does a Slave emit". */
            unsigned int slot = 0;
            while (slot < 8 && last_cmdu[slot] >= 0 &&
                   (last_cmdu[slot] != (int)frame.cmdu || last_mid[slot] != (int)frame.mid))
                slot++;
            if (slot < 8 && last_cmdu[slot] < 0) {
                last_cmdu[slot] = (int)frame.cmdu;
                last_mid[slot] = (int)frame.mid;
                printf("ZTE_AGENT_WIRE rx cmdu=0x%04x mid=0x%04x bytes=%u wsc_msg_type=%u service=%u,%u epoch=%ld\n",
                       frame.cmdu, frame.mid, frame.bytes, frame.wsc_message, frame.service_count,
                       frame.service_first, (long)time(NULL));
                fflush(NULL);
                seen++;
            }
            if (!have_daemon) {
                memcpy(daemon, frame.src, 6);
                have_daemon = 1;
            }
            /* The synchroniser, observed from the same side as the rows: once an M2 has been answered the
             * daemon writes WLANCfg/WLANPSK, and this turns those rows into the host-side plan. */
            if (m1_seen && elapsed >= next_sync) {
                next_sync = elapsed + 2;
                status = agent_sync_poll(AGENT_WLAN_PLAN);
                if (status > 0)
                    printf("ZTE_AGENT_WIRE wlan_plan=1 path=%s epoch=%ld\n", AGENT_WLAN_PLAN, (long)time(NULL));
                fflush(NULL);
            }
            if (frame.cmdu == 0x0007) {
                /* AP Autoconfiguration Search: the enrolment request a controller answers on the band it names. */
                unsigned int band = frame.band > 1 ? 0 : frame.band;
                search++;
                status = controller_autoconfig_response(frame.src, band, frame.mid);
                printf("ZTE_AGENT_WIRE search_reply band=%u mid=0x%04x status=%d\n", band, frame.mid, status);
                fflush(NULL);
            } else if (frame.cmdu == 0x0000) {
                /* Topology Discovery: the answer that makes the agent record us as its controller. */
                unsigned int service = service_cycle[discovery % 3];
                discovery++;
                status = controller_topology_query(frame.src, 0x1200 + discovery, service);
                topo_sent++;
                last_topo = elapsed;
                printf("ZTE_AGENT_WIRE topology_query mid=0x%04x discovery=%u service=%u status=%d\n",
                       0x1200 + discovery, discovery, service, status);
                fflush(NULL);
            } else if (frame.cmdu == 0x0009 && frame.wsc_message == 4) {
                /* WSC M1: the agent (enrollee) is asking for the configuration of one radio. The lab peer is
                 * the registrar here, so it answers with an M2 carrying that band's profile. Every radio gets
                 * its own answer - the daemon enrols 2.4G and 5G separately, and with only the first M1
                 * answered the second band stayed at the identity profile. */
                struct controller_wsc_check check;
                static const char *const stage_name[] = { "none", "parse", "keys", "settings", "settings-leaf",
                    "keywrap", "unused", "encrypt", "m2-head", "m2-body", "authenticator" };
                int band = radio_band(frame.radio, frame.radio_bytes);
                int duplicate = 0;
                for (i = 0; i < answered_count; i++)
                    if (!memcmp(answered[i], frame.radio, 6))
                        duplicate = 1;
                m1_seen++;
                m1_bytes = frame.bytes;
                last_m1 = elapsed;
                printf("ZTE_AGENT_WIRE radio_id=%02x:%02x:%02x:%02x:%02x:%02x text=%.6s band=%d "
                       "mid=0x%04x duplicate=%d\n",
                       frame.radio[0], frame.radio[1], frame.radio[2], frame.radio[3], frame.radio[4],
                       frame.radio[5], frame.radio_bytes >= 6 ? (const char *)frame.radio : "?", band, frame.mid,
                       duplicate);
                if (reboot) {
                    /* After the restart only the question "does it re-enrol" is open, so the M1 is counted
                     * and left unanswered: answering it would rewrite the rows with the same profile and
                     * hide whether the earlier apply survived the restart. */
                    reboot_m1++;
                    printf("ZTE_AGENT_WIRE wsc_m1_after_reboot bytes=%u mid=0x%04x band=%d answered=0\n",
                           frame.bytes, frame.mid, band);
                    fflush(NULL);
                    continue;
                }
                if (duplicate || answered_count >= sizeof(answered) / sizeof(answered[0])) {
                    printf("ZTE_AGENT_WIRE wsc_m1_skipped bytes=%u reason=%s answered=%u\n", frame.bytes,
                           duplicate ? "already-answered" : "table-full", answered_count);
                    fflush(NULL);
                    continue;
                }
                status = controller_wsc_answer_m1(&frame, band == 1 ? &answer_5g : &answer_24g, &check);
                if (check.m1_tags[0])
                    printf("ZTE_AGENT_WIRE m1_tags=%s\n", check.m1_tags);
                if (check.m2_tags[0])
                    printf("ZTE_AGENT_WIRE m2_tags=%s\n", check.m2_tags);
                printf("ZTE_AGENT_WIRE wsc_m1 bytes=%u mid=0x%04x wsc=%u radio=%u m2_sent=%d stage=%u "
                       "stage_name=%s keywrap=%u authenticator=%u profile=%u band=%d epoch=%ld\n",
                       frame.bytes, frame.mid, (unsigned int)frame.wsc_bytes, (unsigned int)frame.radio_bytes,
                       status, check.stage, stage_name[check.stage < 11 ? check.stage : 0], check.keywrap_verified,
                       check.authenticator_verified, check.profile_matched, band, (long)time(NULL));
                fflush(NULL);
                if (!status) {
                    memcpy(answered[answered_count], frame.radio, 6);
                    answered_count++;
                    if (band >= 0 && band <= 1)
                        band_applied[band] = 1;
                }
            }
        }
        if (read < 0 && errno != EAGAIN)
            break;
        /* The daemon's own acd/topo decisions are only logged at debug level; raise it once the daemon is up so
         * "the Slave did not start the M1 exchange" can be read from its side too. Factory channel, no sendcmd -
         * and no shell: the lab root has no /bin/sh, so this is a direct exec. */
        if (debug_raised < 2 && elapsed >= 3 + 6 * debug_raised) {
            static const char *const tasks[2] = { "multiapd.acd.acd", "multiapd.base.base" };
            pid_t helper = fork();
            if (helper == 0) {
                int quiet = open("/dev/null", O_WRONLY);
                if (quiet >= 0) {
                    (void)dup2(quiet, 1);
                    (void)dup2(quiet, 2);
                }
                execl("/bin/zte-debugcmd", "zte-debugcmd", tasks[debug_raised], "-l", "8", (char *)NULL);
                _exit(127);
            }
            status = -1;
            if (helper > 0)
                while (waitpid(helper, &status, 0) < 0 && errno == EINTR)
                    ;
            printf("ZTE_AGENT_DEBUG task=%s level=8 exit=%d\n", tasks[debug_raised],
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            fflush(NULL);
            debug_raised++;
        }
        /* No Search in the first 18 s: the factory agent is nudged by cspd's cooperator (AGENT_PROBE section 5.3). */
        if (!election_fired && !search && elapsed >= 18) {
            election_fired = 1;
            status = controller_election_trigger();
            printf("ZTE_AGENT_WIRE election_trigger status=%d\n", status);
            fflush(NULL);
        }
        /* The daemon may have sent its Discovery before the listener was up, and one query the daemon drops must
         * not be the only one: repeat it every 8 s until the M1 shows up. Before the daemon has been heard from at
         * all its AL MAC is still known - it is br0's. */
        if ((reboot == 1 || !m1_seen) && topo_sent < 8 &&
            (last_topo < 0 ? elapsed >= 4 : elapsed >= last_topo + 8)) {
            unsigned int service = service_cycle[topo_sent % 3];
            status = controller_topology_query(have_daemon ? daemon : al_mac, 0x1200 + (unsigned int)topo_sent,
                                               service);
            topo_sent++;
            last_topo = elapsed;
            printf("ZTE_AGENT_WIRE topology_query retry=%u at=%lds to=%s service=%u status=%d\n", topo_sent,
                   elapsed, have_daemon ? "daemon-src" : "br0-mac", service, status);
            fflush(NULL);
        }
        /* Leave 8 s after the last M1 for the M2 to be applied - or 20 s while a band is still unenrolled,
         * because the daemon's second M1 can follow the first by a while. */
        if (!reboot && answered_count && elapsed >= last_m1 + (band_applied[0] && band_applied[1] ? 8 : 20)) {
            dump_wlan_rows("after");
            dump_psk_rows("after", answer_24g.key, answer_5g.key);
            printf("ZTE_AGENT_SYNC final=1 publish=%d plan=%s\r\n", agent_sync_poll(AGENT_WLAN_PLAN), AGENT_WLAN_PLAN);
            /* Restart the daemon with the database intact. The factory multiapd_mgr does exactly this
             * (SIGKILL, then start again; the engine lives in cspd), so it is the closest this lab gets to
             * "the agent router rebooted": the rows and the daemon's own state are kept, the process is not. */
            status = controller_respawn_daemon(&child);
            printf("ZTE_AGENT_WIRE respawn status=%d epoch=%ld\n", status, (long)time(NULL));
            fflush(NULL);
            if (status) {
                reboot = 2;
                break;
            }
            reboot = 1;
            reboot_at = elapsed;
            /* Announce ourselves to the new instance: the acd task does not leave state 0 on its own.
             * Observed before this was added - the restarted daemon built its M1 phrase and never put a
             * single frame on the wire, which reads as "it kept its configuration" when it only meant
             * "nobody told it a controller exists". */
            last_topo = -1;
            topo_sent = 0;
            dump_wlan_rows("reboot-before");
            dump_psk_rows("reboot-before", answer_24g.key, answer_5g.key);
        } else if (reboot == 1 && elapsed >= reboot_at + 30) {
            dump_wlan_rows("reboot-after");
            dump_psk_rows("reboot-after", answer_24g.key, answer_5g.key);
            (void)kill(child, SIGKILL);
            break;
        }
        if (elapsed >= 100) {
            timed = 1;
            dump_wlan_rows("after");
            dump_psk_rows("after", answer_24g.key, answer_5g.key);
            printf("ZTE_AGENT_SYNC final=1 publish=%d plan=%s\r\n", agent_sync_poll(AGENT_WLAN_PLAN), AGENT_WLAN_PLAN);
            (void)kill(child, SIGKILL);
            break;
        }
        nanosleep(&pause_time, NULL);
    }
    if (listener >= 0)
        close(listener);
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    printf("ZTE_AGENT_WIRE observed=1 frames=%u search_seen=%u discovery_seen=%u topology_sent=%u "
           "election_triggered=%d m1_seen=%d answered=%u band_24g=%u band_5g=%u m1_bytes=%u reboot=%d "
           "reboot_m1=%u timed=%d exit=%d\n",
           seen, search, discovery, topo_sent, election_fired, m1_seen, answered_count, band_applied[0],
           band_applied[1], m1_bytes, reboot, reboot_m1, timed, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fflush(NULL);
    return controller_shutdown_private(id);
}
