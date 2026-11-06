/* Long-running Controller service: the real vendor daemon in the private
 * IPC/network sandbox, configured from a host-written file and reporting its
 * real node inventory as JSON. No fixtures, no timer stop, no success stubs.
 * Only the explicit WPA2-PSK/AES profile of each logical radio is applied
 * (through the typed, rolled-back bridge); everything else is the vendor's.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "controller-service.h"
#include "controller-packet.h"
#include "agent-sync.h"
#include "wlan-profiles.h"
#include "mesh-policies.h"

#define SERVICE_DIR "/run/zte-mesh"
#define CONFIG_PATH SERVICE_DIR "/config"
#define STATE_PATH SERVICE_DIR "/state.json"
#define STATE_TEMP SERVICE_DIR "/.state.json.tmp"
/* The plan the host-side wlan-sync applies to UCI (openwrt/zte-mesh/files/wlan-sync). */
#define WLAN_PLAN_PATH SERVICE_DIR "/wlan-plan"
#define MAX_NODES 32U
#define MAX_CONFIG 8192U /* 16 ACL rules, 6 extra slots, 8 timer rules and the policy keys fit with room to spare */

/* glibc 2.26 exports only the versioned stat entry points (_STAT_VER_LINUX = 3). */
extern int __fxstat(int, int, struct stat *);
extern int __lxstat(int, const char *, struct stat *);
#define fstat(fd, info) __fxstat(3, (fd), (info))
#define lstat(path, info) __lxstat(3, (path), (info))

/* A Wi-Fi off window of the schedule (FEATURE_MAP section 15 / REPLAY_RESULTS section 16): minutes since local midnight,
 * the weekdays on which the window STARTS (bit 0 = Monday ... bit 6 = Sunday), scope 0 = all Wi-Fi except the hidden
 * backhaul slot, 1 = guest slot 1 only. end <= start crosses midnight. */
#define TIMER_RULES 8U
struct timer_rule { uint16_t start, end; uint8_t days, scope; };

struct service_config {
    unsigned int version_mode;
    struct mb_profile profiles[2];
    /* optional extra BSS slots 1..3: enabled/ssid/key are all required together */
    uint8_t bss_set[2][4], bss_enabled[2][4], bss_hidden[2][4], bss_beacon[2][4];
    char bss_ssid[2][4][32], bss_key[2][4][64];
    uint8_t main_hidden[2], main_beacon[2]; /* primary slot style: hidden SSID, BeaconType 3 / 6 / 12 (default 0 / 3) */
    int roam_set, roam_rssi24, roam_rssi5; /* optional MULTIAPDOMAIN1 roam limits, dBm */
    int dual_band_sync;                    /* radio1 ssid/key follow radio0 (wifi.dual_band_sync=1) */
    char essid_suffix[9];                  /* wifi.essid_suffix: 5G name = 2.4G name + suffix while radio1.ssid is not given (default "-5G") */
    int v2_enable;                         /* mesh.v2: P1905ComCfg.Enable, strict peer admission (default off) */
    int mlo_enable;                        /* mlo.enable: WLANSetting.MLOEnable, sent to the agents (default on = vendor baseline) */
    unsigned int beacon_interval[2];       /* ms, 0 = not set (the 100 ms registry mirror stays) */
    struct wlan_acl_rule acl[WLAN_ACL_RULES]; /* MAC filter rules, packed */
    unsigned int acl_count;
    int acl_policy[2][4];                  /* per BSS slot: -1 default, 0 disabled, 1 ban, 2 allow */
    int channel[2];                        /* -1 not set (agent keeps its own), 0 auto, else fixed */
    int tx_power[2];                       /* -1 not set, else EIRP dBm 0..30 */
    int steer_set, steer_enabled;          /* optional master band steering switch */
    int steer_rssi_set, steer_rssi24, steer_rssi5; /* optional band-steering RSSI limits, dBm */
    int role_set, role;                     /* optional mesh.role=master|auto|slave: the MultiAPCfg role triple (CONTROLLER_ROLE_*) */
    int force_set, force_on;               /* optional roam.force: weak-signal forced roam (Domain EnLegacyStaRoam, Master EnBtmKickoff/EnRoamKick) */
    /* What the user configured (profiles[].enabled and bss_enabled above are the EFFECTIVE values: the schedule and the
     * guest access time can switch a configured-on network off). */
    uint8_t profile_enabled_cfg[2], bss_enabled_cfg[2][4];
    unsigned int bss_access[2][4];         /* radioN.bssK.access_minutes: switch the slot off this long after it came on (0 = never) */
    int8_t bss_wifi5[2][4];                /* radioN.bssK.force_wifi5: WLANCfg.ForceWiFi5 of the slot (-1 = not set) */
    struct timer_rule timer[TIMER_RULES];
    unsigned int timer_count;
    int timer_offset;                      /* timer.utc_offset: minutes east of UTC (the sandbox has no time zone) */
};

struct config_stamp { dev_t device; ino_t inode; off_t size; struct timespec modified; };

static int printable(const char *text, size_t length)
{
    size_t i;
    for (i = 0; i < length; i++)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e)
            return 0;
    return 1;
}

static int set_text(char *out, size_t capacity, uint8_t *length, const char *value, size_t bytes)
{
    if (bytes == 0 || bytes >= capacity || !printable(value, bytes))
        return -1;
    memset(out, 0, capacity);
    memcpy(out, value, bytes);
    *length = (uint8_t)bytes;
    return 0;
}

static int parse_acl_policy(const char *text)
{
    if (!strcmp(text, "disabled")) return 0;
    if (!strcmp(text, "ban")) return 1;
    if (!strcmp(text, "allow")) return 2;
    return -1;
}

/* Unicast, non-zero, canonical "aa:bb:cc:dd:ee:ff" in either case; stored lower-case. */
static int normalize_mac(char out[18], const char *text)
{
    unsigned int i, octets[6], any = 0;
    char probe[3];
    if (strlen(text) != 17)
        return -1;
    for (i = 0; i < 17; i++)
        if ((i % 3 == 2) ? text[i] != ':' : !((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                                              (text[i] >= 'A' && text[i] <= 'F')))
            return -1;
    for (i = 0; i < 6; i++) {
        probe[0] = text[i * 3]; probe[1] = text[i * 3 + 1]; probe[2] = 0;
        octets[i] = (unsigned int)strtoul(probe, NULL, 16);
        any |= octets[i];
    }
    if (!any || (octets[0] & 1))
        return -1;
    for (i = 0; i < 6; i++) {
        out[i * 3] = "0123456789abcdef"[(octets[i] >> 4) & 15];
        out[i * 3 + 1] = "0123456789abcdef"[octets[i] & 15];
        out[i * 3 + 2] = i == 5 ? 0 : ':';
    }
    return 0;
}

/* 0 = automatic channel; otherwise the legal 20 MHz channels of the band (no 2.4G ch14). */
static int channel_allowed(unsigned int radio, int value)
{
    static const int five[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                132, 136, 140, 144, 149, 153, 157, 161, 165 };
    unsigned int i;
    if (value == 0)
        return 1;
    if (radio == 0)
        return value >= 1 && value <= 13;
    for (i = 0; i < sizeof(five) / sizeof(five[0]); i++)
        if (five[i] == value)
            return 1;
    return 0;
}

/* Every rejection inside the parser funnels through done:. Recording the source line makes a rejected
 * /run/zte-mesh/config name the rule that refused it, instead of only reporting "missing-or-invalid" -- which
 * cannot be told apart from a key the running binary is too old to know. */
#define reject(at) do { if (!reject_line) reject_line = (at); goto done; } while (0)

/* Strict key=value parser: fixed keys only, no duplicates, no continuation. */
static int parse_config(struct service_config *config, struct config_stamp *stamp)
{
    char buffer[MAX_CONFIG + 1], *line, *next;
    unsigned int seen = 0;
    uint8_t bss_seen[2][4], style_seen[2] = {0, 0}, timer_seen[TIMER_RULES];
    struct timer_rule timer_tmp[TIMER_RULES];
    unsigned int roam_seen = 0, steer_seen = 0, dual_seen = 0, v2_seen = 0, mlo_seen = 0, force_seen = 0, role_seen = 0, suffix_seen = 0, offset_seen = 0;
    struct wlan_acl_rule acl_slots[WLAN_ACL_RULES];
    unsigned char acl_seen[WLAN_ACL_RULES];
    struct stat info;
    ssize_t bytes;
    int fd, result = -1, reject_line = 0;
    memset(config, 0, sizeof(*config));
    config->mlo_enable = 1;
    snprintf(config->essid_suffix, sizeof(config->essid_suffix), "-5G");
    config->main_beacon[0] = config->main_beacon[1] = 3;
    config->channel[0] = config->channel[1] = config->tx_power[0] = config->tx_power[1] = -1;
    memset(config->acl_policy, 0xff, sizeof(config->acl_policy));
    memset(bss_seen, 0, sizeof(bss_seen));
    memset(timer_seen, 0, sizeof(timer_seen));
    memset(timer_tmp, 0, sizeof(timer_tmp));
    memset(config->bss_wifi5, 0xff, sizeof(config->bss_wifi5));
    memset(acl_slots, 0, sizeof(acl_slots));
    memset(acl_seen, 0, sizeof(acl_seen));
    fd = open(CONFIG_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 077) ||
        info.st_size < 1 || info.st_size > (off_t)MAX_CONFIG)
        reject(__LINE__);
    bytes = read(fd, buffer, MAX_CONFIG + 1);
    if (bytes != info.st_size)
        reject(__LINE__);
    buffer[bytes] = 0;
    for (line = buffer; *line; line = next) {
        char *newline = strchr(line, '\n'), *equals;
        unsigned int radio, bit;
        if (!newline)
            reject(__LINE__);
        *newline = 0;
        next = newline + 1;
        equals = strchr(line, '=');
        if (!equals)
            reject(__LINE__);
        *equals++ = 0;
        if (!strcmp(line, "version_mode")) {
            char *end;
            unsigned long value = strtoul(equals, &end, 10);
            if (seen & 1U || !*equals || *end || value > 255)
                reject(__LINE__);
            config->version_mode = (unsigned int)value;
            seen |= 1U;
            continue;
        }
        if (!strncmp(line, "acl.", 4)) {
            char *dot;
            unsigned long n = strtoul(line + 4, &dot, 10);
            struct wlan_acl_rule *rule;
            unsigned int bit;
            if (dot == line + 4 || *dot != '.' || n < 1 || n > WLAN_ACL_RULES)
                reject(__LINE__);
            rule = &acl_slots[n - 1];
            if (!strcmp(dot + 1, "mac")) {
                bit = 1U;
                if (acl_seen[n - 1] & bit || normalize_mac(rule->mac, equals))
                    reject(__LINE__);
            } else if (!strcmp(dot + 1, "name")) {
                size_t i, bytes = strlen(equals);
                bit = 2U;
                if (acl_seen[n - 1] & bit || !bytes || bytes > 20)
                    reject(__LINE__);
                for (i = 0; i < bytes; i++)
                    if (!((equals[i] >= '0' && equals[i] <= '9') || (equals[i] >= 'a' && equals[i] <= 'z') ||
                          (equals[i] >= 'A' && equals[i] <= 'Z') || equals[i] == '_' || equals[i] == '-' || equals[i] == '.'))
                        reject(__LINE__);
                memcpy(rule->name, equals, bytes + 1);
            } else if (!strcmp(dot + 1, "radio")) {
                bit = 4U;
                if (acl_seen[n - 1] & bit ||
                    (strcmp(equals, "0") && strcmp(equals, "1") && strcmp(equals, "both")))
                    reject(__LINE__);
                rule->radio = !strcmp(equals, "both") ? 2 : equals[0] - '0';
            } else if (!strcmp(dot + 1, "bss")) {
                bit = 8U;
                if (acl_seen[n - 1] & bit || strlen(equals) != 1 || equals[0] < '0' || equals[0] > '3')
                    reject(__LINE__);
                rule->bss = equals[0] - '0';
            } else {
                reject(__LINE__);
            }
            acl_seen[n - 1] |= bit;
            continue;
        }
        if (!strcmp(line, "mesh.v2")) {
            if (v2_seen || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->v2_enable = equals[0] - '0';
            v2_seen = 1;
            continue;
        }
        if (!strcmp(line, "mesh.role")) {
            /* "slave" is the agent side: the daemon then runs multi_ap_slave_main instead of the controller, which is
             * what the agent observation (controller_agent_startup) exercises. The router-facing UCI/UI does not offer
             * it yet, see docs/research/AGENT_PROBE.md. */
            if (role_seen || (strcmp(equals, "master") && strcmp(equals, "auto") && strcmp(equals, "slave")))
                reject(__LINE__);
            config->role_set = 1;
            config->role = !strcmp(equals, "auto") ? CONTROLLER_ROLE_AUTO
                          : !strcmp(equals, "slave") ? CONTROLLER_ROLE_SLAVE : CONTROLLER_ROLE_MASTER;
            role_seen = 1;
            continue;
        }
        if (!strcmp(line, "roam.force")) {
            if (force_seen || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->force_on = equals[0] - '0';
            config->force_set = 1;
            force_seen = 1;
            continue;
        }
        if (!strcmp(line, "mlo.enable")) {
            if (mlo_seen || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->mlo_enable = equals[0] - '0';
            mlo_seen = 1;
            continue;
        }
        if (!strcmp(line, "wifi.dual_band_sync")) {
            if (dual_seen || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->dual_band_sync = equals[0] - '0';
            dual_seen = 1;
            continue;
        }
        if (!strcmp(line, "wifi.essid_suffix")) {
            size_t bytes = strlen(equals);
            if (suffix_seen || bytes < 1 || bytes > 8 || !printable(equals, bytes))
                reject(__LINE__);
            snprintf(config->essid_suffix, sizeof(config->essid_suffix), "%s", equals);
            suffix_seen = 1;
            continue;
        }
        if (!strncmp(line, "timer.", 6)) {
            if (!strcmp(line + 6, "utc_offset")) {
                char *end;
                long offset = strtol(equals, &end, 10);
                if (offset_seen || !*equals || *end || offset < -720 || offset > 840)
                    reject(__LINE__);
                config->timer_offset = (int)offset;
                offset_seen = 1;
            } else {
                char *dot;
                unsigned long n = strtoul(line + 6, &dot, 10);
                struct timer_rule *rule;
                unsigned int field;
                if (dot == line + 6 || *dot != '.' || n < 1 || n > TIMER_RULES)
                    reject(__LINE__);
                rule = &timer_tmp[n - 1];
                if (!strcmp(dot + 1, "start") || !strcmp(dot + 1, "end")) {
                    unsigned int hour, minute;
                    int is_start = !strcmp(dot + 1, "start");
                    if (strlen(equals) != 5 || equals[2] != ':' || equals[0] < '0' || equals[0] > '2' || equals[1] < '0' ||
                        equals[1] > '9' || equals[3] < '0' || equals[3] > '5' || equals[4] < '0' || equals[4] > '9')
                        reject(__LINE__);
                    hour = (unsigned int)((equals[0] - '0') * 10 + (equals[1] - '0'));
                    minute = (unsigned int)((equals[3] - '0') * 10 + (equals[4] - '0'));
                    if (hour > 23)
                        reject(__LINE__);
                    field = is_start ? 1U : 2U;
                    if (timer_seen[n - 1] & field)
                        reject(__LINE__);
                    if (is_start)
                        rule->start = (uint16_t)(hour * 60 + minute);
                    else
                        rule->end = (uint16_t)(hour * 60 + minute);
                } else if (!strcmp(dot + 1, "days")) {
                    unsigned int d;
                    field = 4U;
                    if ((timer_seen[n - 1] & field) || strlen(equals) != 7)
                        reject(__LINE__);
                    rule->days = 0;
                    for (d = 0; d < 7; d++) {
                        if (equals[d] != '0' && equals[d] != '1')
                            reject(__LINE__);
                        if (equals[d] == '1')
                            rule->days |= (uint8_t)(1U << d);
                    }
                    if (!rule->days)
                        reject(__LINE__);
                } else if (!strcmp(dot + 1, "scope")) {
                    field = 8U;
                    if ((timer_seen[n - 1] & field) || (strcmp(equals, "all") && strcmp(equals, "guest")))
                        reject(__LINE__);
                    rule->scope = !strcmp(equals, "guest");
                } else {
                    reject(__LINE__);
                }
                timer_seen[n - 1] |= (uint8_t)field;
            }
            continue;
        }
        if (!strcmp(line, "band_steer.enabled")) {
            if ((steer_seen & 1U) || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->steer_enabled = equals[0] - '0';
            config->steer_set = 1;
            steer_seen |= 1U;
            continue;
        }
        if (!strcmp(line, "band_steer.rssi24") || !strcmp(line, "band_steer.rssi5")) {
            char *end;
            long value = strtol(equals, &end, 10);
            int is24 = !strcmp(line, "band_steer.rssi24");
            if ((steer_seen & (is24 ? 2U : 4U)) || !*equals || *end || value < -110 || value > -30)
                reject(__LINE__);
            if (is24)
                config->steer_rssi24 = (int)value;
            else
                config->steer_rssi5 = (int)value;
            steer_seen |= is24 ? 2U : 4U;
            continue;
        }
        if (!strcmp(line, "roam.rssi24") || !strcmp(line, "roam.rssi5")) {
            char *end;
            long value = strtol(equals, &end, 10);
            int is24 = !strcmp(line, "roam.rssi24");
            if ((roam_seen & (is24 ? 1U : 2U)) || !*equals || *end || value < -110 || value > -30)
                reject(__LINE__);
            if (is24)
                config->roam_rssi24 = (int)value;
            else
                config->roam_rssi5 = (int)value;
            roam_seen |= is24 ? 1U : 2U;
            continue;
        }
        if (strncmp(line, "radio", 5) || (line[5] != '0' && line[5] != '1') || line[6] != '.')
            reject(__LINE__);
        radio = (unsigned int)(line[5] - '0');
        if (!strcmp(line + 7, "acl_policy")) {
            if (config->acl_policy[radio][0] >= 0 || (config->acl_policy[radio][0] = parse_acl_policy(equals)) < 0)
                reject(__LINE__);
            continue;
        }
        if (!strcmp(line + 7, "channel") || !strcmp(line + 7, "tx_power")) {
            char *end;
            long value = strtol(equals, &end, 10);
            int is_channel = !strcmp(line + 7, "channel");
            int *slot = is_channel ? &config->channel[radio] : &config->tx_power[radio];
            if (*slot >= 0 || !*equals || *end || value < 0 ||
                (is_channel ? !channel_allowed(radio, (int)value) : value > 30))
                reject(__LINE__);
            *slot = (int)value;
            continue;
        }
        if (!strcmp(line + 7, "hidden") || !strcmp(line + 7, "beacon_type")) {
            int is_hidden = !strcmp(line + 7, "hidden");
            uint8_t flag = is_hidden ? 1U : 2U;
            if (style_seen[radio] & flag)
                reject(__LINE__);
            if (is_hidden) {
                if (strcmp(equals, "0") && strcmp(equals, "1"))
                    reject(__LINE__);
                config->main_hidden[radio] = (uint8_t)(equals[0] - '0');
            } else {
                if (strcmp(equals, "0") && strcmp(equals, "3") && strcmp(equals, "6") && strcmp(equals, "10") && strcmp(equals, "12"))
                    reject(__LINE__);
                config->main_beacon[radio] = (uint8_t)strtoul(equals, NULL, 10);
            }
            style_seen[radio] |= flag;
            continue;
        }
        if (!strcmp(line + 7, "beacon_interval")) {
            char *end;
            unsigned long value = strtoul(equals, &end, 10);
            if (config->beacon_interval[radio] || !*equals || *end || value < 20 || value > 1000)
                reject(__LINE__);
            config->beacon_interval[radio] = (unsigned int)value;
            continue;
        }
        if (!strcmp(line + 7, "ssid")) {
            bit = 2U << (radio * 3);
            if (seen & bit ||
                set_text(config->profiles[radio].ssid, sizeof(config->profiles[radio].ssid),
                         &config->profiles[radio].ssid_length, equals, strlen(equals)))
                reject(__LINE__);
        } else if (!strcmp(line + 7, "key")) {
            bit = 4U << (radio * 3);
            if (seen & bit ||
                set_text(config->profiles[radio].key, sizeof(config->profiles[radio].key),
                         &config->profiles[radio].key_length, equals, strlen(equals)))
                reject(__LINE__);
        } else if (!strcmp(line + 7, "enabled")) {
            bit = 8U << (radio * 3);
            if (seen & bit || (strcmp(equals, "0") && strcmp(equals, "1")))
                reject(__LINE__);
            config->profiles[radio].enabled = config->profile_enabled_cfg[radio] = (uint8_t)(equals[0] - '0');
        } else if (!strncmp(line + 7, "bss", 3) && line[10] >= '1' && line[10] <= '3' && line[11] == '.') {
            unsigned int slot = (unsigned int)(line[10] - '0'), what;
            size_t bytes = strlen(equals);
            if (!strcmp(line + 12, "enabled"))
                what = 1U;
            else if (!strcmp(line + 12, "ssid"))
                what = 2U;
            else if (!strcmp(line + 12, "key"))
                what = 4U;
            else if (!strcmp(line + 12, "acl_policy"))
                what = 8U;
            else if (!strcmp(line + 12, "hidden"))
                what = 16U;
            else if (!strcmp(line + 12, "beacon_type"))
                what = 32U;
            else if (!strcmp(line + 12, "access_minutes"))
                what = 64U;
            else if (!strcmp(line + 12, "force_wifi5"))
                what = 128U;
            else
                reject(__LINE__);
            if (bss_seen[radio][slot] & what)
                reject(__LINE__);
            if (what == 1U) {
                if (strcmp(equals, "0") && strcmp(equals, "1"))
                    reject(__LINE__);
                config->bss_enabled[radio][slot] = config->bss_enabled_cfg[radio][slot] = (uint8_t)(equals[0] - '0');
            } else if (what == 2U) {
                uint8_t length;
                if (set_text(config->bss_ssid[radio][slot], sizeof(config->bss_ssid[radio][slot]), &length, equals, bytes))
                    reject(__LINE__);
            } else if (what == 8U) {
                if (config->acl_policy[radio][slot] >= 0 || (config->acl_policy[radio][slot] = parse_acl_policy(equals)) < 0)
                    reject(__LINE__);
            } else if (what == 16U) {
                if (strcmp(equals, "0") && strcmp(equals, "1"))
                    reject(__LINE__);
                config->bss_hidden[radio][slot] = (uint8_t)(equals[0] - '0');
            } else if (what == 32U) {
                if (strcmp(equals, "0") && strcmp(equals, "3") && strcmp(equals, "6") && strcmp(equals, "10") && strcmp(equals, "12"))
                    reject(__LINE__);
                config->bss_beacon[radio][slot] = (uint8_t)strtoul(equals, NULL, 10);
            } else if (what == 64U) {
                char *end;
                unsigned long minutes = strtoul(equals, &end, 10);
                if (!*equals || *end || minutes < 1 || minutes > 10080)
                    reject(__LINE__);
                config->bss_access[radio][slot] = (unsigned int)minutes;
            } else if (what == 128U) {
                if (strcmp(equals, "0") && strcmp(equals, "1"))
                    reject(__LINE__);
                config->bss_wifi5[radio][slot] = (int8_t)(equals[0] - '0');
            } else {
                uint8_t length;
                if (bytes < 8 || set_text(config->bss_key[radio][slot], sizeof(config->bss_key[radio][slot]), &length, equals, bytes))
                    reject(__LINE__);
            }
            bss_seen[radio][slot] |= what;
            continue;
        } else {
            reject(__LINE__);
        }
        seen |= bit;
    }
    if (dual_seen && config->dual_band_sync) {
        /* radio1 name/key are not given separately: a contradictory value is rejected,
         * otherwise they mirror radio0 (the vendor "dual-band sync" meaning). */
        if (seen & ((2U | 4U) << 3))
            reject(__LINE__);
        if (seen != (1U | (14U << 0) | (8U << 3)))
            reject(__LINE__);
        memcpy(config->profiles[1].ssid, config->profiles[0].ssid, sizeof(config->profiles[1].ssid));
        memcpy(config->profiles[1].key, config->profiles[0].key, sizeof(config->profiles[1].key));
        config->profiles[1].ssid_length = config->profiles[0].ssid_length;
        config->profiles[1].key_length = config->profiles[0].key_length;
    } else {
        /* Bands not synchronised: radio1.enabled is required, its name and key are optional. An omitted radio1.ssid is the
         * 2.4G name + WLANSetting.ESSIDSuffix, the factory rule (FEATURE_MAP section 4.1). The factory can also change the 5G
         * password on its own (section 11.1); an omitted radio1.key is the 2.4G key, which is what the baseline has. */
        const unsigned int need = 1U | (14U << 0) | (8U << 3);
        if ((seen & need) != need)
            reject(__LINE__);
        if (!(seen & (2U << 3)) && mb_derive_name(&config->profiles[0], config->essid_suffix, &config->profiles[1]))
            reject(__LINE__);
        if (!(seen & (4U << 3))) {
            memcpy(config->profiles[1].key, config->profiles[0].key, sizeof(config->profiles[1].key));
            config->profiles[1].key_length = config->profiles[0].key_length;
        }
    }
    {
        unsigned int radio, slot;
        for (radio = 0; radio < 2; radio++)
            for (slot = 1; slot < 4; slot++) {
                if (!bss_seen[radio][slot])
                    continue;
                if ((bss_seen[radio][slot] & 7U) != 7U) /* enabled + ssid + key, never half a slot */
                    reject(__LINE__);
                /* The BSS entries of the M2 are positional: the agent applies them to its slots in order, so a gap
                 * (slot 3 without slot 2) would put slot 3's settings on the agent's slot 2 and switch its slot 3 off
                 * (replay run 44). The configured slots of a radio are therefore always 1..k. */
                if (slot >= 2 && !config->bss_set[radio][slot - 1])
                    reject(__LINE__);
                config->bss_set[radio][slot] = 1;
                if (!(bss_seen[radio][slot] & 16U))
                    config->bss_hidden[radio][slot] = slot == 3; /* slot 3 is the hidden backhaul BSS */
                if (!(bss_seen[radio][slot] & 32U))
                    config->bss_beacon[radio][slot] = 3;
            }
    }
    /* the slot table above is complete: rules and policies may now refer to slots (this check used to run before the table
     * was filled, which rejected every rule with bss>0 and every radioN.bssK.acl_policy) */
    {
        unsigned int i, radio, slot;
        for (i = 0; i < WLAN_ACL_RULES; i++) {
            struct wlan_acl_rule *rule = &acl_slots[i];
            if (!acl_seen[i])
                continue;
            if (!(acl_seen[i] & 1U)) /* a rule needs its MAC */
                reject(__LINE__);
            if (!(acl_seen[i] & 2U))
                snprintf(rule->name, sizeof(rule->name), "acl%u", i + 1);
            if (!(acl_seen[i] & 4U))
                rule->radio = 2;
            for (radio = 0; radio < 2; radio++)
                if (rule->bss && (rule->radio == 2 || rule->radio == (int)radio) && !config->bss_set[radio][rule->bss])
                    reject(__LINE__); /* a slot without its own BSS has no interface to filter */
            config->acl[config->acl_count++] = *rule;
        }
        for (radio = 0; radio < 2; radio++)
            for (slot = 1; slot < 4; slot++)
                if (config->acl_policy[radio][slot] >= 0 && !config->bss_set[radio][slot])
                    reject(__LINE__);
    }
    {
        unsigned int i, n = 0;
        for (i = 0; i < TIMER_RULES; i++) {
            if (!timer_seen[i])
                continue;
            if ((timer_seen[i] & 3U) != 3U || timer_tmp[i].start == timer_tmp[i].end) /* start and end are both needed and differ */
                reject(__LINE__);
            if (!(timer_seen[i] & 4U))
                timer_tmp[i].days = 0x7f;
            config->timer[n++] = timer_tmp[i];
        }
        config->timer_count = n;
        if (n && !offset_seen) /* the sandbox has no time zone: the host passes its offset */
            reject(__LINE__);
    }
    if (steer_seen & 6U) {
        if ((steer_seen & 6U) != 6U) /* both bands or neither */
            reject(__LINE__);
        config->steer_rssi_set = 1;
    }
    if (roam_seen) {
        if (roam_seen != 3U) /* both bands or neither */
            reject(__LINE__);
        config->roam_set = 1;
    }
    {
        unsigned int radio;
        for (radio = 0; radio < 2; radio++) {
            config->profiles[radio].request_id = 1;
            config->profiles[radio].security = MB_WPA2_PSK_AES;
            config->profiles[radio].radio = (uint8_t)radio;
            if (mb_validate(&config->profiles[radio]))
                reject(__LINE__);
        }
    }
    stamp->device = info.st_dev; stamp->inode = info.st_ino;
    stamp->size = info.st_size; stamp->modified = info.st_mtim;
    result = 0;
done:
    mb_wipe(buffer, sizeof(buffer));
    close(fd);
    if (result) {
        fprintf(stderr, "ZTE_MESH_SERVICE config-reject line=%d\n", reject_line);
        mb_wipe(config, sizeof(*config));
    }
    return result;
}

#undef reject

static int same_stamp(const struct config_stamp *a, const struct config_stamp *b)
{
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
           a->modified.tv_sec == b->modified.tv_sec && a->modified.tv_nsec == b->modified.tv_nsec;
}

static int stat_stamp(struct config_stamp *stamp)
{
    struct stat info;
    if (lstat(CONFIG_PATH, &info))
        return -1;
    stamp->device = info.st_dev; stamp->inode = info.st_ino;
    stamp->size = info.st_size; stamp->modified = info.st_mtim;
    return 0;
}

static int profile_differs(const struct service_config *a, const struct service_config *b, unsigned int radio)
{
    return a->profiles[radio].enabled != b->profiles[radio].enabled ||
           a->profiles[radio].ssid_length != b->profiles[radio].ssid_length ||
           a->profiles[radio].key_length != b->profiles[radio].key_length ||
           memcmp(a->profiles[radio].ssid, b->profiles[radio].ssid, sizeof(a->profiles[radio].ssid)) ||
           memcmp(a->profiles[radio].key, b->profiles[radio].key, sizeof(a->profiles[radio].key));
}

static int main_style_differs(const struct service_config *a, const struct service_config *b, unsigned int radio)
{
    return a->main_hidden[radio] != b->main_hidden[radio] || a->main_beacon[radio] != b->main_beacon[radio];
}

static int slot_differs(const struct service_config *a, const struct service_config *b, unsigned int radio, unsigned int slot)
{
    return a->bss_enabled[radio][slot] != b->bss_enabled[radio][slot] ||
           a->bss_wifi5[radio][slot] != b->bss_wifi5[radio][slot] ||
           a->bss_hidden[radio][slot] != b->bss_hidden[radio][slot] ||
           a->bss_beacon[radio][slot] != b->bss_beacon[radio][slot] ||
           memcmp(a->bss_ssid[radio][slot], b->bss_ssid[radio][slot], sizeof(a->bss_ssid[radio][slot])) ||
           memcmp(a->bss_key[radio][slot], b->bss_key[radio][slot], sizeof(a->bss_key[radio][slot]));
}

/* JSON string: printable ASCII and well-formed UTF-8 pass; anything else is '?'. */
static size_t json_string(char *out, size_t capacity, const char *text)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (capacity < 3)
        return 0;
    out[used++] = '"';
    while (*p && used + 8 < capacity) {
        unsigned int extra = 0;
        if (*p == '"' || *p == '\\') {
            out[used++] = '\\'; out[used++] = (char)*p++;
        } else if (*p >= 0x20 && *p < 0x7f) {
            out[used++] = (char)*p++;
        } else if (*p >= 0xc2 && *p <= 0xdf) {
            extra = 1;
        } else if (*p >= 0xe0 && *p <= 0xef) {
            extra = 2;
        } else if (*p >= 0xf0 && *p <= 0xf4) {
            extra = 3;
        } else {
            out[used++] = '?'; p++;
        }
        if (extra) {
            unsigned int i, good = 1;
            for (i = 1; i <= extra; i++)
                if ((p[i] & 0xc0) != 0x80) { good = 0; break; }
            if (good) {
                for (i = 0; i <= extra; i++)
                    out[used++] = (char)p[i];
                p += extra + 1;
            } else {
                out[used++] = '?'; p++;
            }
        }
    }
    out[used++] = '"';
    out[used] = 0;
    return used;
}

struct service_state {
    int running;
    unsigned long sequence, applied;
    unsigned int version_mode;
    int restart_required;
    char config_error[64];
    pid_t daemon;
    time_t started;
    unsigned int radio_dirty;  /* bit per radio: channel/power changed, push again */
    /* Master-policy changes already written to the database, not yet told to the daemon:
     * bit0 = Domain rows (CM notification 0x10206), bit1 = Master row (0x10205). */
    unsigned int notify_pending, notify_attempts;
    time_t notify_next;
    /* Daemon respawns (the factory multiapd_mgr restart, FEATURE_MAP sections 9/10): requested after the database
     * write, done by the main loop that owns the child; the count is the number of restarts since the service began. */
    int respawn_pending;
    unsigned int respawns;
    /* "adaptive" role page (REPLAY_RESULTS section 13): CLOCK_MONOTONIC second at which the election is settled
     * (Mode 0 -> 2, second restart); 0 = none pending */
    time_t role_settle_at;
    /* Schedule and guest access time (REPLAY_RESULTS section 16): the Wi-Fi off windows currently open, and the countdown
     * of every slot with access_minutes. mask_dirty = the effective enable bits must be recomputed (the config is read again). */
    int sched_off_all, sched_off_guest, mask_dirty;
    time_t guest_since[2][4];
    uint8_t guest_expired[2][4];
    unsigned int sched_rules, guest_left[2][4]; /* for state.json: rule count, seconds left of each access countdown */
};

/* Node actions requested from the host (reboot / LED of a child router, FEATURE_MAP section 17 / REPLAY_RESULTS section 16):
 * the rpcd plugin writes a one-line request file into ACTION_DIR, the main loop executes it with the factory-style
 * helper and keeps the last results for state.json. Requests left over from before the service started are discarded. */
#define ACTION_DIR SERVICE_DIR "/actions"
#define ACTION_KEEP 8U
struct action_result { char id[40], action[8], mac[18]; int value, status; long epoch; };
static struct action_result action_results[ACTION_KEEP];
static unsigned int action_total;

struct radio_push { int done; unsigned int attempts; time_t next; };
struct radio_agent { char mac[18]; time_t since; };
/* Per ZTE agent session: has the MLO switch been tunnelled to its wlan_mgr yet? */
#define MLO_NODES 8U
struct mlo_push { char mac[18]; time_t since, next; int sent; unsigned int attempts; };

static void al_mac(char out[18])
{
    struct ifreq request;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "br0");
    snprintf(out, 18, "00:00:00:00:00:00");
    if (fd >= 0 && !ioctl(fd, SIOCGIFHWADDR, &request)) {
        const unsigned char *m = (const unsigned char *)request.ifr_hwaddr.sa_data;
        snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    if (fd >= 0)
        close(fd);
}

/* What the trace library digested from the child's frames (controller-trace.c note_peer): the value of "key" in
 * peers/<mac>.<kind>, -1 if missing or older than max_age seconds; *age gets the file's age in seconds. */
static int read_peer(const char *mac, const char *kind, const char *key, time_t max_age, long *age)
{
    char path[96], text[32], wanted[16];
    struct stat info;
    ssize_t bytes;
    int fd, value = -1;
    snprintf(path, sizeof(path), SERVICE_DIR "/peers/%s.%s", mac, kind);
    snprintf(wanted, sizeof(wanted), "%s=", key);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size < (off_t)sizeof(text)) {
        bytes = read(fd, text, sizeof(text) - 1);
        if (bytes > 0) {
            text[bytes] = 0;
            if (!strncmp(text, wanted, strlen(wanted)) && text[strlen(wanted)] >= '0' && text[strlen(wanted)] <= '9')
                value = atoi(text + strlen(wanted));
        }
        if (age)
            *age = (long)(time(NULL) - info.st_mtime);
        if (value >= 0 && max_age && time(NULL) - info.st_mtime > max_age)
            value = -1;
    }
    (void)close(fd);
    return value;
}

static int write_state(const struct service_state *state)
{
    static char json[16384];
    struct controller_node nodes[MAX_NODES];
    unsigned int count = controller_read_nodes(nodes, MAX_NODES), i;
    struct timespec now;
    char mac[18], quoted[320];
    size_t used = 0;
    int fd, written;
    clock_gettime(CLOCK_MONOTONIC, &now);
    al_mac(mac);
    used += (size_t)snprintf(json + used, sizeof(json) - used,
        "{\"schema\":1,\"running\":%s,\"sequence\":%lu,\"uptime\":%ld,\"daemon_pid\":%d,\"daemon_restarts\":%u,"
        "\"al_mac\":\"%s\",\"version_mode\":%u,\"config_applied\":%lu,\"restart_required\":%s,\"config_error\":",
        state->running ? "true" : "false", state->sequence, (long)(now.tv_sec - state->started), (int)state->daemon,
        state->respawns, mac, state->version_mode, state->applied, state->restart_required ? "true" : "false");
    json_string(quoted, sizeof(quoted), state->config_error);
    used += (size_t)snprintf(json + used, sizeof(json) - used, "%s,\"nodes\":[", quoted);
    for (i = 0; i < count && used + 1200 < sizeof(json); i++) {
        used += (size_t)snprintf(json + used, sizeof(json) - used, "%s{\"key\":", i ? "," : "");
        json_string(quoted, sizeof(quoted), nodes[i].view);
        used += (size_t)snprintf(json + used, sizeof(json) - used, "%s,\"mac\":\"%s\",\"model\":", quoted, nodes[i].mac);
        json_string(quoted, sizeof(quoted), nodes[i].model);
        used += (size_t)snprintf(json + used, sizeof(json) - used, "%s,\"name\":", quoted);
        json_string(quoted, sizeof(quoted), nodes[i].name);
        used += (size_t)snprintf(json + used, sizeof(json) - used, "%s,\"software\":", quoted);
        json_string(quoted, sizeof(quoted), nodes[i].software);
        {
            long light_age = -1;
            int light = read_peer(nodes[i].mac, "light", "light", 0, &light_age);
            int stations = read_peer(nodes[i].mac, "stations", "stations", 30, NULL);
            used += (size_t)snprintf(json + used, sizeof(json) - used, "%s,\"zte\":%s,\"online\":%s,\"light\":%d,\"light_age\":%ld,\"stations\":%d}",
                                     quoted, nodes[i].zte ? "true" : "false", nodes[i].active ? "true" : "false", light, light_age, stations);
        }
    }
    used += (size_t)snprintf(json + used, sizeof(json) - used, "],\"actions\":[");
    for (i = 0; i < ACTION_KEEP && i < action_total && used + 400 < sizeof(json); i++) {
        const struct action_result *r = &action_results[(action_total - 1 - i) % ACTION_KEEP]; /* newest first */
        json_string(quoted, sizeof(quoted), r->id);
        used += (size_t)snprintf(json + used, sizeof(json) - used,
                                 "%s{\"id\":%s,\"action\":\"%s\",\"mac\":\"%s\",\"value\":%d,\"status\":%d,\"epoch\":%ld}",
                                 i ? "," : "", quoted, r->action, r->mac, r->value, r->status, r->epoch);
    }
    used += (size_t)snprintf(json + used, sizeof(json) - used, "],\"sched\":{\"rules\":%u,\"off_all\":%s,\"off_guest\":%s,\"access\":[",
                             state->sched_rules, state->sched_off_all ? "true" : "false", state->sched_off_guest ? "true" : "false");
    {
        unsigned int r, k, first = 1;
        for (r = 0; r < 2; r++)
            for (k = 1; k < 4; k++)
                if ((state->guest_left[r][k] || state->guest_expired[r][k]) && used + 200 < sizeof(json)) {
                    used += (size_t)snprintf(json + used, sizeof(json) - used, "%s{\"radio\":%u,\"slot\":%u,\"left\":%u,\"expired\":%s}",
                                             first ? "" : ",", r, k, state->guest_left[r][k], state->guest_expired[r][k] ? "true" : "false");
                    first = 0;
                }
    }
    used += (size_t)snprintf(json + used, sizeof(json) - used, "]}}\n");
    if (used >= sizeof(json))
        return -1;
    fd = open(STATE_TEMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    written = (int)write(fd, json, used);
    if (close(fd) || written != (int)used || rename(STATE_TEMP, STATE_PATH))
        return -1;
    return 0;
}

/* Ask the daemon to push the WLAN profile to its agents, exactly as the factory
 * CM layer does. The CM call sends an OSS message, which needs a client OSS
 * identity (liboss_pub + InitAppComm, as the factory tools do); the controller
 * itself runs the full liboss and must not do that. So it runs as the separate
 * factory-style helper inside this sandbox. Returns the CM status, -1000 when
 * the helper could not run, -1001 on timeout, -2000-N if it died on signal N. */
static int run_helper3(const char *a, const char *b, const char *c)
{
    struct timespec pause_time = {0, 100000000L};
    int status = 0, waited = 0, tries;
    pid_t child = fork();
    if (child < 0)
        return -1000;
    if (!child) {
        /* Lazy binding: libcmapi has references that only the daemon's full OSS
         * library defines. They are never called on this path. */
        /* Format-only trace of the factory client's own diagnostics (no arguments). */
        setenv("LD_PRELOAD", "/lib/libzte_controller_trace.so", 1);
        unsetenv("LD_BIND_NOW");
        execl("/lib/ld-linux.so.3", "ld-linux.so.3", "--library-path", "/lib",
              "/bin/zte-meshctl", a, b, c, (char *)NULL);
        _exit(127);
    }
    for (tries = 0; tries < 80; tries++) {
        waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        nanosleep(&pause_time, NULL);
    }
    if (waited != child) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        return -1001;
    }
    if (WIFSIGNALED(status))
        return -2000 - WTERMSIG(status);
    return WEXITSTATUS(status);
}

static int run_helper(const char *a, const char *b)
{
    return run_helper3(a, b, NULL);
}

/* Log every change of the role/hop fields of MultiAPCfg (replay evidence for the role election and CurHop, GAP 7/8). */
static void cfg_watch(void)
{
    static int last[5] = { -99, -99, -99, -99, -99 };
    static const char *const names[5] = { "Mode", "SetRoleEnable", "EnMasterOnly", "CurHop", "MaxHop" };
    int now[5], i, changed = 0;
    for (i = 0; i < 5; i++) {
        if (controller_read_cfg(names[i], &now[i]))
            now[i] = -98;
        if (now[i] != last[i])
            changed = 1;
    }
    if (!changed)
        return;
    printf("ZTE_MESH_SERVICE cfg mode=%d set_role_enable=%d en_master_only=%d cur_hop=%d max_hop=%d epoch=%ld\n", now[0], now[1],
           now[2], now[3], now[4], (long)time(NULL));
    fflush(NULL);
    for (i = 0; i < 5; i++)
        last[i] = now[i];
}

/* The daemon's topology holds the agent (P1905DevNum >= 2: itself plus at least one device). After a daemon respawn the
 * agent rows of the database are stale (Active stays 1 from the old daemon) and the agent needs 40 s or so to appear in
 * the new topology (REPLAY_RESULTS section 12); a push sent in that gap is dropped by the daemon and, marked as done, never
 * repeated. Unreadable = assume ready (the previous behaviour). Logs every change. */
static int topology_ready(void)
{
    static int last = -2;
    int devnum = controller_read_dev_num();
    if (devnum != last) {
        printf("ZTE_MESH_SERVICE topology dev_num=%d epoch=%ld\n", devnum, (long)time(NULL));
        fflush(NULL);
        last = devnum;
    }
    return devnum < 0 || devnum >= 2;
}

/* Channel / transmit-power reconciliation: a configured value is pushed to the first ZTE
 * agent once it has been online long enough to know this controller (15 s; earlier
 * Multi-AP messages are dropped by it), again whenever the agent comes back or the value
 * changes, retried a few times. The agent only restarts its radio if the value differs. */
static void radio_reconcile(const struct service_config *cfg, struct service_state *state,
                            struct radio_push push[2], struct radio_agent *agent, time_t now)
{
    struct controller_node nodes[MAX_NODES];
    unsigned int count, i, radio, octets[6];
    int found = -1;
    if (cfg->channel[0] < 0 && cfg->channel[1] < 0 && cfg->tx_power[0] < 0 && cfg->tx_power[1] < 0 &&
        !state->radio_dirty)
        return;
    count = controller_read_nodes(nodes, MAX_NODES);
    for (i = 0; i < count && topology_ready(); i++)
        /* The radio identifiers used are ZTE's; skip only nodes known to be something else
         * (a node whose identity TLV was not parsed yet has an empty model). */
        if (nodes[i].active && (nodes[i].zte || !nodes[i].model[0])) { found = (int)i; break; }
    if (found < 0) {
        agent->mac[0] = 0;
        return;
    }
    if (strcmp(agent->mac, nodes[found].mac)) {
        snprintf(agent->mac, sizeof(agent->mac), "%s", nodes[found].mac);
        agent->since = now;
        for (radio = 0; radio < 2; radio++) { push[radio].done = 0; push[radio].attempts = 0; push[radio].next = 0; }
    }
    if (now - agent->since < 15)
        return;
    if (sscanf(agent->mac, "%x:%x:%x:%x:%x:%x", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) != 6)
        return;
    for (radio = 0; radio < 2; radio++) {
        uint8_t mac[6];
        int status, code = -1;
        if (state->radio_dirty & (1U << radio)) {
            state->radio_dirty &= ~(1U << radio);
            push[radio].done = 0; push[radio].attempts = 0; push[radio].next = 0;
        }
        if (cfg->channel[radio] < 0 && cfg->tx_power[radio] < 0)
            continue;
        if (push[radio].done || push[radio].attempts >= 4 || now < push[radio].next)
            continue;
        for (i = 0; i < 6; i++)
            mac[i] = (uint8_t)octets[i];
        status = controller_radio_select(mac, radio, cfg->channel[radio], cfg->tx_power[radio], &code);
        push[radio].attempts++;
        push[radio].next = now + 10;
        printf("ZTE_MESH_SERVICE radio_select radio=%u channel=%d tx_power=%d status=%d response_code=%d attempt=%u\n",
               radio, cfg->channel[radio], cfg->tx_power[radio], status, code, push[radio].attempts);
        fflush(NULL);
        if (!status && !code)
            push[radio].done = 1;
    }
}

/* The factory cspd wlan_mgr tells every device that comes online the master's MLOEnable (cross-device
 * module message 0200fe, REPLAY_RESULTS section 8/9). cspd is not running here, so do what it does through
 * the same daemon event: once a ZTE agent has been online long enough to know this controller (15 s, as
 * for the radio push), send it the start-up value; again when that agent comes back. Tracked per agent
 * (up to MLO_NODES at once); at most one helper run per call so the service loop stays responsive. */
static void mlo_reconcile(const struct service_config *cfg, struct mlo_push push[MLO_NODES], time_t now)
{
    struct controller_node nodes[MAX_NODES];
    unsigned int count = controller_read_nodes(nodes, MAX_NODES), i, k;
    if (!topology_ready())
        count = 0; /* nobody counts as online until the daemon's topology holds the agent again */
    for (k = 0; k < MLO_NODES; k++) {
        int online = 0;
        for (i = 0; i < count && push[k].mac[0]; i++)
            if (nodes[i].active && nodes[i].zte && !strcmp(nodes[i].mac, push[k].mac))
                online = 1;
        if (!online && push[k].mac[0]) {
            printf("ZTE_MESH_SERVICE mlo_node mac=%s online=0 epoch=%ld\n", push[k].mac, (long)time(NULL));
            push[k].mac[0] = 0;
        }
    }
    for (i = 0; i < count; i++) {
        int slot = -1;
        if (!nodes[i].active || !nodes[i].zte)
            continue;
        for (k = 0; k < MLO_NODES; k++) {
            if (!strcmp(push[k].mac, nodes[i].mac)) { slot = (int)k; break; }
            if (!push[k].mac[0] && slot < 0)
                slot = (int)k;
        }
        if (slot >= 0 && strcmp(push[slot].mac, nodes[i].mac)) {
            snprintf(push[slot].mac, sizeof(push[slot].mac), "%s", nodes[i].mac);
            printf("ZTE_MESH_SERVICE mlo_node mac=%s online=1 epoch=%ld\n", nodes[i].mac, (long)time(NULL));
            push[slot].since = now;
            push[slot].sent = -1;
            push[slot].attempts = 0;
            push[slot].next = 0;
        }
    }
    for (k = 0; k < MLO_NODES; k++) {
        struct mlo_push *p = &push[k];
        int status;
        if (p->mac[0] && p->sent >= 0 && p->sent != cfg->mlo_enable && p->attempts) {
            p->attempts = 0; /* a new value after a successful push gets a fresh set of retries */
            p->next = now;
        }
        if (!p->mac[0] || now - p->since < 15 || p->sent == cfg->mlo_enable || p->attempts >= 4 || now < p->next)
            continue;
        status = run_helper3("wlan-mgr", p->mac, cfg->mlo_enable ? "1" : "0");
        p->attempts++;
        p->next = now + 10;
        printf("ZTE_MESH_SERVICE wlan_mgr_push mac=%s mlo_enable=%d status=%d attempt=%u epoch=%ld\n", p->mac, cfg->mlo_enable,
               status, p->attempts, (long)time(NULL));
        fflush(NULL);
        if (!status)
            p->sent = cfg->mlo_enable;
        return;
    }
}

static void action_record(const char *id, const char *action, const char *mac, int value, int status)
{
    struct action_result *r = &action_results[action_total % ACTION_KEEP];
    memset(r, 0, sizeof(*r));
    snprintf(r->id, sizeof(r->id), "%s", id);
    snprintf(r->action, sizeof(r->action), "%s", action);
    snprintf(r->mac, sizeof(r->mac), "%s", mac);
    r->value = value;
    r->status = status;
    r->epoch = (long)time(NULL);
    action_total++;
    printf("ZTE_MESH_SERVICE node_action id=%s action=%s mac=%s value=%d status=%d epoch=%ld\n", id, action, mac, value, status, r->epoch);
    fflush(NULL);
}

/* Status codes of a request: 0 = sent to the daemon; -2 = malformed request; -3 = no such online ZTE node; -4 = the daemon is
 * not ready (restarting); other = the helper's status (-1000 helper did not run, -1001 timeout, 3 = the daemon refused). */
static void action_reconcile(int execute)
{
    DIR *dir = opendir(ACTION_DIR);
    struct dirent *entry;
    unsigned int handled = 0;
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL && handled < 2) {
        char path[96], text[96], id[40], verb[16], mac_text[32], extra[8], mac[18];
        size_t length = strlen(entry->d_name), i;
        struct controller_node nodes[MAX_NODES];
        unsigned int count, k, tokens;
        struct stat info;
        int fd, status = -2, value = 0;
        ssize_t bytes;
        if (length < 5 || length > 43 || strcmp(entry->d_name + length - 4, ".act"))
            continue;
        snprintf(path, sizeof(path), ACTION_DIR "/%s", entry->d_name);
        memcpy(id, entry->d_name, length - 4);
        id[length - 4] = 0;
        for (i = 0; id[i]; i++)
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= 'A' && id[i] <= 'Z') || id[i] == '-' || id[i] == '_'))
                id[i] = '_';
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            (void)unlink(path);
            continue;
        }
        bytes = -1;
        if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == 0 && info.st_size > 0 && info.st_size < (off_t)sizeof(text))
            bytes = read(fd, text, sizeof(text) - 1);
        (void)close(fd);
        (void)unlink(path);
        if (!execute)
            continue;
        handled++;
        verb[0] = mac_text[0] = extra[0] = 0;
        if (bytes > 0) {
            text[bytes] = 0;
            tokens = (unsigned int)sscanf(text, "%15s %31s %7s", verb, mac_text, extra);
            if (normalize_mac(mac, mac_text)) {
                action_record(id, "bad", "-", 0, -2);
                continue;
            }
            if (!strcmp(verb, "reboot") && tokens == 2)
                value = 1;
            else if (!strcmp(verb, "light") && tokens == 3 && (!strcmp(extra, "0") || !strcmp(extra, "1")))
                value = extra[0] - '0';
            else {
                action_record(id, "bad", mac, 0, -2);
                continue;
            }
            count = topology_ready() ? controller_read_nodes(nodes, MAX_NODES) : 0;
            status = -3;
            for (k = 0; k < count; k++)
                if (nodes[k].active && nodes[k].zte && !strcmp(nodes[k].mac, mac))
                    status = 0;
            if (status == 0)
                status = run_helper3(!strcmp(verb, "reboot") ? "reboot-node" : "light-node", mac, !strcmp(verb, "reboot") ? NULL : extra);
            else if (!topology_ready())
                status = -4;
            action_record(id, verb, mac, value, status);
        } else {
            action_record(id, "bad", "-", 0, -2);
        }
    }
    closedir(dir);
}

/* Tell the master task that a policy table changed, as the factory CM setters do after their database write
 * (CmSendMsg2MM("multiapd.map.master", 0x2401, id)). 0x10206 makes it re-read the Domain rows and push the
 * roam-threshold PolicyConfigReq to every agent, 0x10205 re-reads MultiAPMaster and pushes the vendor-only one. */
static int notify_master(unsigned int first, unsigned int second)
{
    char a[12], b[12];
    snprintf(a, sizeof(a), "0x%x", first);
    if (!second)
        return run_helper("notify-master", a);
    snprintf(b, sizeof(b), "0x%x", second);
    return run_helper3("notify-master", a, b);
}

/* Domain before Master (the factory web path writes the roam setter first, FEATURE_MAP section 3); when both are
 * pending they go out from one helper process back to back, like the 12 ms gap of the original capture. A few retries
 * when the helper or the daemon did not answer. */
static void policy_notify_reconcile(struct service_state *state, time_t now)
{
    unsigned int first, second = 0;
    int status;
    if (!state->notify_pending || now < state->notify_next)
        return;
    if (state->notify_pending & 1U) {
        first = 0x10206U;
        if (state->notify_pending & 2U)
            second = 0x10205U;
    } else {
        first = 0x10205U;
    }
    status = notify_master(first, second);
    state->notify_attempts++;
    state->notify_next = now + 2;
    printf("ZTE_MESH_SERVICE policy_notify id=0x%x%s status=%d attempt=%u\n", first, second ? "+0x10205" : "", status,
           state->notify_attempts);
    fflush(NULL);
    if (!status || state->notify_attempts >= 4) {
        state->notify_pending &= second ? ~3U : ~((first == 0x10206U) ? 1U : 2U);
        state->notify_attempts = 0;
    }
}

int controller_renew_agents(void)
{
    int band, status = 0;
    for (band = 0; band < 2 && !status; band++)
        status = run_helper("renew-acd", band ? "1" : "0");
    return status;
}

/* cspd's cooperator (_MapCooperatorACDUpProc / _V4AddrProc) sets g_TriggerSelectFlag and then calls
 * CfIeee1905RoleElect, which is how a factory agent starts its Search. multiapd only reaches that entry through its
 * acd task, so the lab path pokes it with the same kind of probe event zte-meshctl already sends for the renew
 * (0x909b, destination multiapd.acd.acd). The payload is not observable from the binary, so it is a zeroed four-byte
 * probe and the status is reported as-is - a non-zero status simply means this trigger does not exist. */
int controller_election_trigger(void)
{
    return run_helper("acd-election", NULL);
}

/* Which off windows are open at this wall-clock second. Bit 0 of the result: all Wi-Fi off; bit 1: guest slot 1 off. */
static unsigned int sched_windows(const struct service_config *c, time_t wall)
{
    struct tm tm;
    time_t local = wall + (time_t)c->timer_offset * 60;
    unsigned int i, result = 0, minute, today, yesterday;
    if (!c->timer_count || !gmtime_r(&local, &tm))
        return 0;
    minute = (unsigned int)(tm.tm_hour * 60 + tm.tm_min);
    today = (unsigned int)((tm.tm_wday + 6) % 7); /* tm_wday: 0 = Sunday; ours: 0 = Monday */
    yesterday = (today + 6) % 7;
    for (i = 0; i < c->timer_count; i++) {
        const struct timer_rule *r = &c->timer[i];
        int open;
        if (r->start < r->end)
            open = (r->days >> today & 1U) && minute >= r->start && minute < r->end;
        else /* crosses midnight: the part before midnight belongs to today's day bit, the part after to yesterday's */
            open = ((r->days >> today & 1U) && minute >= r->start) || ((r->days >> yesterday & 1U) && minute < r->end);
        if (open)
            result |= r->scope ? 2U : 3U;
    }
    return result;
}

/* Effective enable bits = configured, minus what the schedule or an expired access time has switched off. The hidden
 * backhaul slot (3) is never switched off by the schedule: the mesh needs it. */
static void sched_mask(struct service_config *c, const struct service_state *s)
{
    unsigned int r, k;
    for (r = 0; r < 2; r++) {
        c->profiles[r].enabled = (uint8_t)(c->profile_enabled_cfg[r] && !s->sched_off_all);
        for (k = 1; k < 4; k++)
            c->bss_enabled[r][k] = (uint8_t)(c->bss_enabled_cfg[r][k] && !s->guest_expired[r][k] &&
                                             !(k < 3 && s->sched_off_all) && !(k == 1 && s->sched_off_guest));
    }
}

/* Is the slot "running" as far as its access-time countdown goes: configured on and not held off by the schedule. */
static int sched_armed(const struct service_config *c, const struct service_state *s, unsigned int r, unsigned int k)
{
    return c->bss_set[r][k] && c->bss_enabled_cfg[r][k] && !(k < 3 && s->sched_off_all) && !(k == 1 && s->sched_off_guest);
}

/* A new configuration: countdowns restart for slots whose switch or access time changed. */
static void sched_reset(const struct service_config *next, const struct service_config *old, struct service_state *s)
{
    unsigned int r, k;
    for (r = 0; r < 2; r++)
        for (k = 1; k < 4; k++)
            if (!next->bss_set[r][k] || !next->bss_enabled_cfg[r][k] || old->bss_enabled_cfg[r][k] != next->bss_enabled_cfg[r][k] ||
                old->bss_access[r][k] != next->bss_access[r][k]) {
                s->guest_since[r][k] = 0;
                s->guest_expired[r][k] = 0;
            }
}

/* Once per second: open/close the schedule windows and run the access-time countdowns. A change only marks the effective
 * values dirty; the main loop's config pass applies them like any other configuration change. */
static void sched_tick(const struct service_config *c, struct service_state *s, time_t wall, time_t mono)
{
    unsigned int windows = sched_windows(c, wall), r, k;
    int all = (int)(windows & 1U), guest = (int)(windows >> 1 & 1U);
    if (all != s->sched_off_all || guest != s->sched_off_guest) {
        s->sched_off_all = all;
        s->sched_off_guest = guest;
        s->mask_dirty = 1;
        printf("ZTE_MESH_SERVICE sched off_all=%d off_guest=%d epoch=%ld\n", all, guest, (long)wall);
        fflush(NULL);
    }
    s->sched_rules = c->timer_count;
    for (r = 0; r < 2; r++)
        for (k = 1; k < 4; k++) {
            s->guest_left[r][k] = 0;
            if (!c->bss_access[r][k])
                continue;
            if (!sched_armed(c, s, r, k)) {
                s->guest_since[r][k] = 0; /* held off or switched off: the countdown starts over when it comes back */
                if (!c->bss_enabled_cfg[r][k])
                    s->guest_expired[r][k] = 0;
                continue;
            }
            if (!s->guest_since[r][k])
                s->guest_since[r][k] = mono;
            if (!s->guest_expired[r][k]) {
                time_t used = mono - s->guest_since[r][k], limit = (time_t)c->bss_access[r][k] * 60;
                s->guest_left[r][k] = used < limit ? (unsigned int)(limit - used) : 0;
            }
            if (!s->guest_expired[r][k] && mono - s->guest_since[r][k] >= (time_t)c->bss_access[r][k] * 60) {
                s->guest_expired[r][k] = 1;
                s->mask_dirty = 1;
                printf("ZTE_MESH_SERVICE access_expired radio=%u slot=%u minutes=%u epoch=%ld\n", r, k, c->bss_access[r][k], (long)wall);
                fflush(NULL);
            }
        }
}

static const char *role_name(int role)
{
    return role == CONTROLLER_ROLE_SLAVE ? "slave" : role == CONTROLLER_ROLE_AUTO ? "auto" : "master";
}

static void apply_new_config(struct service_config *current, struct config_stamp *stamp, struct service_state *state)
{
    struct service_config next;
    struct config_stamp fresh;
    struct mb_result response = {0};
    unsigned int radio, setting_renew = 0;
    int failed = 0;
    if (stat_stamp(&fresh) || (same_stamp(&fresh, stamp) && !state->mask_dirty))
        return;
    state->mask_dirty = 0;
    if (parse_config(&next, &fresh)) {
        snprintf(state->config_error, sizeof(state->config_error), "invalid configuration ignored");
        *stamp = fresh;
        printf("ZTE_MESH_SERVICE config=rejected\n");
        fflush(NULL);
        return;
    }
    *stamp = fresh;
    state->config_error[0] = 0;
    sched_reset(&next, current, state);
    sched_mask(&next, state);
    if (next.version_mode != current->version_mode) {
        state->restart_required = 1;
        printf("ZTE_MESH_SERVICE version_mode_changed restart_required=1\n");
    }
    {
        /* Extra BSS slots. Their enable bit, SSID, key, hiding and security are edited live below (the enable bit is read
         * from WLANCfg at every Renew, REPLAY_RESULTS section 14). Adding or removing a slot changes what the daemon's
         * ACD module knows: it builds its per-BSS list from P1905AcdBssCfg only when it starts, so the rows are written
         * now and the daemon is restarted, the same way the factory restarts it for a mesh-level change (the agent drops
         * out of the topology for a while). A removed slot keeps its rows and reads as "not instantiated". */
        unsigned int r, k;
        int slots_changed = 0, slots_failed = 0;
        for (r = 0; r < 2; r++)
            for (k = 1; k < 4; k++) {
                int status;
                if (current->bss_set[r][k] == next.bss_set[r][k])
                    continue;
                status = controller_slot_present(r, k, next.bss_set[r][k], next.bss_enabled[r][k], next.bss_ssid[r][k],
                                                 next.bss_key[r][k], next.bss_hidden[r][k], next.bss_beacon[r][k]);
                printf("ZTE_MESH_SERVICE slot_%s radio=%u slot=%u status=%d epoch=%ld\n", next.bss_set[r][k] ? "add" : "remove", r, k,
                       status, (long)time(NULL));
                if (status) {
                    slots_failed = 1;
                    continue;
                }
                if (next.bss_set[r][k] && next.bss_wifi5[r][k] >= 0 && wlan_profiles_set_force_wifi5(r, k, next.bss_wifi5[r][k])) {
                    slots_failed = 1;
                    continue;
                }
                current->bss_set[r][k] = next.bss_set[r][k];
                if (next.bss_set[r][k]) {
                    current->bss_enabled[r][k] = next.bss_enabled[r][k];
                    current->bss_hidden[r][k] = next.bss_hidden[r][k];
                    current->bss_beacon[r][k] = next.bss_beacon[r][k];
                    memcpy(current->bss_ssid[r][k], next.bss_ssid[r][k], sizeof(current->bss_ssid[r][k]));
                    memcpy(current->bss_key[r][k], next.bss_key[r][k], sizeof(current->bss_key[r][k]));
                }
                slots_changed = 1;
            }
        if (slots_failed) {
            state->restart_required = 1;
            snprintf(state->config_error, sizeof(state->config_error), "slot update failed (partial), restart required");
        }
        if (slots_changed)
            state->respawn_pending = 1;
    }
    if (next.v2_enable != current->v2_enable) {
        /* The factory path (FEATURE_MAP section 9): CmMultiApTopoSetP1905ComCfg writes the P1905ComCfg view and then
         * sends 0xa93d to cspd's multiapd_mgr, which stops and starts multiapd (the daemon reads Enable once when its
         * base module starts). Every Renew and the 6 to 40 s topology flap seen there are the new daemon coming up,
         * so the same is done here: write the field, then the main loop respawns the daemon on the same database. */
        int before = controller_read_v2();
        int status = controller_set_v2_live(next.v2_enable);
        printf("ZTE_MESH_SERVICE v2_update enabled=%d db_before=%d db_after=%d status=%d epoch=%ld\n", next.v2_enable, before,
               controller_read_v2(), status, (long)time(NULL));
        if (status) {
            state->restart_required = 1;
            snprintf(state->config_error, sizeof(state->config_error), "mesh.v2 update failed, restart required");
        } else {
            current->v2_enable = next.v2_enable;
            state->respawn_pending = 1;
        }
    }
    if (next.role_set != current->role_set) {
        /* the key appearing or disappearing has no value to go back to */
        state->restart_required = 1;
        printf("ZTE_MESH_SERVICE role_key_changed restart_required=1\n");
    } else if (next.role_set && next.role != current->role) {
        /* The factory device-role page (FEATURE_MAP section 10): "adaptive" writes SetRoleEnable 1 and Mode 0 (the
         * election state; the Mode change makes cspd restart the daemon). The new daemon then lets cspd's cooperator
         * decide: router mode -> master, `_autoSetRole` writes Mode 2 and asks cspd for a second restart about 3 s
         * later. Neither cspd's cooperator nor that request exist here and the daemon does not finish the election by
         * itself (replay lab run: Mode stays 0), so the supervisor stands in for them: see role_settle_at. Back to
         * "master" only clears SetRoleEnable, nothing restarts (10.1).
         *
         * The agent side ("slave") is a different kind of change: multi_ap_slave_main reads EnMasterOnly/Mode once, so
         * the whole triple is written and the daemon is replaced. No election stand-in is involved in either direction. */
        int status;
        if (next.role == CONTROLLER_ROLE_SLAVE || current->role == CONTROLLER_ROLE_SLAVE) {
            status = next.role == CONTROLLER_ROLE_SLAVE
                ? (controller_write_cfg("Mode", 1) || controller_write_cfg("SetRoleEnable", 0) ||
                   controller_write_cfg("EnMasterOnly", 0) || controller_write_cfg("CurHop", -2))
                : (controller_write_cfg("Mode", 2) ||
                   controller_write_cfg("SetRoleEnable", next.role == CONTROLLER_ROLE_AUTO ? 1 : 0) ||
                   controller_write_cfg("EnMasterOnly", 1) || controller_write_cfg("CurHop", 2));
            state->role_settle_at = 0;
            if (!status)
                state->respawn_pending = 1;
        } else if (next.role == CONTROLLER_ROLE_AUTO) {
            status = controller_write_cfg("SetRoleEnable", 1) || controller_write_cfg("Mode", 0);
            if (!status) {
                struct timespec at;
                clock_gettime(CLOCK_MONOTONIC, &at);
                state->respawn_pending = 1;
                state->role_settle_at = at.tv_sec + 3;
            }
        } else {
            status = controller_write_cfg("SetRoleEnable", 0);
            /* switched back inside the election window: finish the Mode change now */
            if (!status && state->role_settle_at) {
                status = controller_write_cfg("Mode", 2); /* the election window is open, so Mode is still 0 */
                state->role_settle_at = 0;
                state->respawn_pending = 1;
            }
        }
        printf("ZTE_MESH_SERVICE role_update role=%s status=%d epoch=%ld\n", role_name(next.role), status,
               (long)time(NULL));
        if (status) {
            state->restart_required = 1;
            snprintf(state->config_error, sizeof(state->config_error), "mesh.role update failed, restart required");
        } else {
            current->role = next.role;
        }
    }
    if (next.mlo_enable != current->mlo_enable) {
        /* The factory MLO switch (FEATURE_MAP section 7) writes WLANSetting.MLOEnable, renews both bands (the M2
         * loses its Wi-Fi 7 attributes when off) and has cspd's wlan_mgr tell every agent the new value, which
         * mlo_reconcile() does from the configured value. */
        int status = controller_set_mlo_live(next.mlo_enable);
        int renew = status ? -1 : controller_renew_agents();
        printf("ZTE_MESH_SERVICE mlo_update enabled=%d status=%d renew=%d\n", next.mlo_enable, status, renew);
        if (status) {
            state->restart_required = 1;
            snprintf(state->config_error, sizeof(state->config_error), "MLO update failed, restart required");
        } else {
            current->mlo_enable = next.mlo_enable;
        }
    }
    if (next.dual_band_sync != current->dual_band_sync || strcmp(next.essid_suffix, current->essid_suffix)) {
        /* The factory dual-band switch writes WLANSetting.DualBandSync (FEATURE_MAP section 4); the 5G name follows from the
         * profile comparison below (derived name = radio0 name + suffix, or the same name while synchronised). */
        int status = controller_set_wlan_setting_live(next.dual_band_sync, next.essid_suffix);
        printf("ZTE_MESH_SERVICE wlan_setting_update dual_band_sync=%d suffix_len=%zu status=%d\n", next.dual_band_sync,
               strlen(next.essid_suffix), status);
        if (status) {
            state->restart_required = 1;
            snprintf(state->config_error, sizeof(state->config_error), "dual-band setting update failed, restart required");
        } else {
            if (next.dual_band_sync != current->dual_band_sync)
                setting_renew = 3U; /* the factory switch renews both bands (the names also change in the same step) */
            current->dual_band_sync = next.dual_band_sync;
            memcpy(current->essid_suffix, next.essid_suffix, sizeof(current->essid_suffix));
        }
    }
    {
        /* Master policy keys are applied live: write the rows the factory setters write, then queue the CM
         * notification that makes the master push the new policy. A key that appears or disappears still needs a
         * restart (the start-up rows of the policy domain are only prepared when a key is present, and "unset"
         * has no value to go back to). */
        int failed_write = 0;
        unsigned int pending = 0;
        if (next.steer_set != current->steer_set || next.steer_rssi_set != current->steer_rssi_set ||
            next.roam_set != current->roam_set || next.force_set != current->force_set) {
            state->restart_required = 1;
            printf("ZTE_MESH_SERVICE policy_keys_changed restart_required=1\n");
        } else {
            if (next.roam_set && (next.roam_rssi24 != current->roam_rssi24 || next.roam_rssi5 != current->roam_rssi5)) {
                int status = mesh_policies_live_roam(next.roam_rssi24, next.roam_rssi5);
                printf("ZTE_MESH_SERVICE roam_update rssi24=%d rssi5=%d status=%d\n", next.roam_rssi24, next.roam_rssi5, status);
                if (status) failed_write = 1; else pending |= 1U;
            }
            if (next.steer_rssi_set && (next.steer_rssi24 != current->steer_rssi24 || next.steer_rssi5 != current->steer_rssi5)) {
                int status = mesh_policies_live_steer_rssi(next.steer_rssi24, next.steer_rssi5);
                printf("ZTE_MESH_SERVICE band_steer_rssi_update rssi24=%d rssi5=%d status=%d\n", next.steer_rssi24, next.steer_rssi5, status);
                if (status) failed_write = 1; else pending |= 1U;
            }
            if (next.steer_set && next.steer_enabled != current->steer_enabled) {
                int status = mesh_policies_live_steer_enabled(next.steer_enabled);
                printf("ZTE_MESH_SERVICE band_steer_update enabled=%d status=%d\n", next.steer_enabled, status);
                if (status) failed_write = 1; else pending |= 2U;
            }
            if (next.force_set && next.force_on != current->force_on) {
                int status = mesh_policies_live_force_domain(next.force_on) | mesh_policies_live_force_master(next.force_on);
                printf("ZTE_MESH_SERVICE roam_force_update enabled=%d status=%d\n", next.force_on, status);
                if (status) failed_write = 1; else pending |= 3U;
            }
            if (failed_write) {
                state->restart_required = 1;
                snprintf(state->config_error, sizeof(state->config_error), "policy update failed, restart required");
            } else {
                current->roam_rssi24 = next.roam_rssi24; current->roam_rssi5 = next.roam_rssi5;
                current->steer_enabled = next.steer_enabled;
                current->steer_rssi24 = next.steer_rssi24; current->steer_rssi5 = next.steer_rssi5;
                current->force_on = next.force_on;
            }
            state->notify_pending |= pending;
        }
    }
    for (radio = 0; radio < 2; radio++) {
        if ((next.channel[radio] != current->channel[radio] && next.channel[radio] >= 0) ||
            (next.tx_power[radio] != current->tx_power[radio] && next.tx_power[radio] >= 0))
            state->radio_dirty |= 1U << radio;
        if ((next.channel[radio] < 0 && current->channel[radio] >= 0) ||
            (next.tx_power[radio] < 0 && current->tx_power[radio] >= 0))
            printf("ZTE_MESH_SERVICE radio=%u channel/tx_power key removed: the agent keeps its current setting\n", radio);
        current->channel[radio] = next.channel[radio];
        current->tx_power[radio] = next.tx_power[radio];
    }
    {
        /* MAC filter: rules are edited in place in the pre-created AclCfg rows and the policies
         * in WLANCfg, then both bands renew so the agent re-runs its vendor phase. */
        unsigned int slot;
        int changed = 0, bad = 0;
        if (current->acl_count != next.acl_count || memcmp(current->acl, next.acl, sizeof(next.acl))) {
            bad |= wlan_profiles_acl_apply(next.acl, next.acl_count, 1);
            changed = 1;
        }
        for (radio = 0; radio < 2; radio++)
            for (slot = 0; slot < 4; slot++) {
                int fallback = slot == 3 ? 0 : 1; /* the agent's own defaults (primary/guest 1, backhaul 0) */
                int wanted = next.acl_policy[radio][slot] >= 0 ? next.acl_policy[radio][slot] : fallback;
                int have = current->acl_policy[radio][slot] >= 0 ? current->acl_policy[radio][slot] : fallback;
                if (wanted != have && (slot == 0 || next.bss_set[radio][slot])) {
                    bad |= wlan_profiles_set_acl_policy(radio, slot, wanted, 1);
                    changed = 1;
                }
            }
        if (changed) {
            int renew = bad ? -1 : (run_helper("renew-acd", "0") | run_helper("renew-acd", "1"));
            printf("ZTE_MESH_SERVICE acl_update rules=%u status=%d renew=%d\n", next.acl_count, bad, renew);
            if (!bad) {
                current->acl_count = next.acl_count;
                memcpy(current->acl, next.acl, sizeof(current->acl));
                memcpy(current->acl_policy, next.acl_policy, sizeof(current->acl_policy));
            }
        }
    }
    for (radio = 0; radio < 2; radio++) {
        unsigned int wanted = next.beacon_interval[radio] ? next.beacon_interval[radio] : 100;
        unsigned int have = current->beacon_interval[radio] ? current->beacon_interval[radio] : 100;
        if (wanted != have) {
            /* WLANBase row first; the agent reads it again when the Renew sends it back
             * through the band-config query of its vendor phase. */
            int set = wlan_profiles_set_beacon_interval(radio, wanted);
            int renew = set ? -1 : run_helper("renew-acd", radio ? "1" : "0");
            printf("ZTE_MESH_SERVICE beacon_interval radio=%u value=%u set=%d renew=%d\n", radio, wanted, set, renew);
            if (!set)
                current->beacon_interval[radio] = next.beacon_interval[radio];
        }
    }
    {
        /* Primary profile, primary style and extra slots: written live; only the band(s) that changed renew, as the
         * factory multiapd_mgr does (a change of one band's name, key, enable, hiding or security renews that band
         * only, FEATURE_MAP section 8). The profile bridge rolls back its own write; a style or slot write that fails
         * may be partial, which needs a restart to get back to a known state. */
        unsigned int changed = setting_renew, slot;
        int bridge_failed = 0, other_failed = 0;
        for (radio = 0; radio < 2; radio++) {
            if (profile_differs(current, &next, radio) || main_style_differs(current, &next, radio))
                changed |= 1U << radio;
            for (slot = 1; slot < 4; slot++)
                if (current->bss_set[radio][slot] && next.bss_set[radio][slot] && slot_differs(current, &next, radio, slot))
                    changed |= 1U << radio;
        }
        for (radio = 0; radio < 2 && !bridge_failed && !other_failed; radio++) {
            if (!(changed & (1U << radio)))
                continue;
            if (profile_differs(current, &next, radio))
                bridge_failed = wlan_profiles_bridge_apply(&next.profiles[radio], 0, &response);
            /* the bridge resets BeaconType to WPA2: the primary style is written after every profile write */
            if (!bridge_failed && (profile_differs(current, &next, radio) || main_style_differs(current, &next, radio)))
                other_failed = wlan_profiles_set_main_style(radio, next.main_hidden[radio], next.main_beacon[radio]);
            for (slot = 1; slot < 4 && !bridge_failed && !other_failed; slot++)
                if (current->bss_set[radio][slot] && next.bss_set[radio][slot] && slot_differs(current, &next, radio, slot))
                {
                    other_failed = wlan_profiles_update_extra_bss(radio, slot, next.bss_enabled[radio][slot],
                                                                  next.bss_ssid[radio][slot], next.bss_key[radio][slot],
                                                                  next.bss_hidden[radio][slot], next.bss_beacon[radio][slot]);
                    if (!other_failed && next.bss_wifi5[radio][slot] >= 0 && next.bss_wifi5[radio][slot] != current->bss_wifi5[radio][slot])
                        other_failed = wlan_profiles_set_force_wifi5(radio, slot, next.bss_wifi5[radio][slot]);
                }
        }
        failed = bridge_failed || other_failed;
        if (changed && failed) {
            if (other_failed) {
                state->restart_required = 1;
                snprintf(state->config_error, sizeof(state->config_error), "update failed (partial), restart required");
            } else {
                snprintf(state->config_error, sizeof(state->config_error), "profile update failed and was rolled back");
            }
            printf("ZTE_MESH_SERVICE profile_update=failed status=%u other=%d\n", response.status, other_failed);
        } else if (changed) {
            int renew = 0;
            for (radio = 0; radio < 2 && !renew; radio++)
                if (changed & (1U << radio))
                    renew = run_helper("renew-acd", radio ? "1" : "0");
            state->applied++;
            printf("ZTE_MESH_SERVICE profile_update=ok bands=%u renew_status=%d applied=%lu ssid_len=%u,%u\n", changed, renew,
                   state->applied, next.profiles[0].ssid_length, next.profiles[1].ssid_length);
            memcpy(current->profiles, next.profiles, sizeof(current->profiles));
            memcpy(current->main_hidden, next.main_hidden, sizeof(current->main_hidden));
            memcpy(current->main_beacon, next.main_beacon, sizeof(current->main_beacon));
            for (radio = 0; radio < 2; radio++)
                for (slot = 1; slot < 4; slot++)
                    if (current->bss_set[radio][slot] && next.bss_set[radio][slot]) {
                        current->bss_enabled[radio][slot] = next.bss_enabled[radio][slot];
                        current->bss_hidden[radio][slot] = next.bss_hidden[radio][slot];
                        current->bss_beacon[radio][slot] = next.bss_beacon[radio][slot];
                        memcpy(current->bss_ssid[radio][slot], next.bss_ssid[radio][slot], sizeof(current->bss_ssid[radio][slot]));
                        memcpy(current->bss_key[radio][slot], next.bss_key[radio][slot], sizeof(current->bss_key[radio][slot]));
                    }
        }
    }
    /* what the schedule and the access-time countdowns are computed from */
    memcpy(current->timer, next.timer, sizeof(current->timer));
    current->timer_count = next.timer_count;
    current->timer_offset = next.timer_offset;
    memcpy(current->profile_enabled_cfg, next.profile_enabled_cfg, sizeof(current->profile_enabled_cfg));
    memcpy(current->bss_enabled_cfg, next.bss_enabled_cfg, sizeof(current->bss_enabled_cfg));
    memcpy(current->bss_access, next.bss_access, sizeof(current->bss_access));
    memcpy(current->bss_wifi5, next.bss_wifi5, sizeof(current->bss_wifi5));
    fflush(NULL);
    mb_wipe(&next, sizeof(next));
}

int controller_service(void)
{
    struct service_config config;
    struct config_stamp stamp = {0};
    struct service_state state = {0};
    struct timespec pause_time = {0, 250000000L}, now, last_state = {0, 0};
    siginfo_t info;
    char version[8];
    pid_t child;
    int id = -1, status;
    if (parse_config(&config, &stamp)) {
        printf("ZTE_MESH_SERVICE fail=config-missing-or-invalid\n");
        fflush(NULL);
        return -1;
    }
    snprintf(version, sizeof(version), "%u", config.version_mode);
    {
        /* the schedule and the access countdowns already apply to what the database starts with */
        unsigned int windows = sched_windows(&config, time(NULL)), r, k;
        struct timespec boot;
        clock_gettime(CLOCK_MONOTONIC, &boot);
        state.sched_off_all = (int)(windows & 1U);
        state.sched_off_guest = (int)(windows >> 1 & 1U);
        for (r = 0; r < 2; r++)
            for (k = 1; k < 4; k++)
                if (config.bss_access[r][k] && sched_armed(&config, &state, r, k))
                    state.guest_since[r][k] = boot.tv_sec;
        sched_mask(&config, &state);
        state.sched_rules = config.timer_count;
        if (windows)
            printf("ZTE_MESH_SERVICE sched off_all=%d off_guest=%d at start\n", state.sched_off_all, state.sched_off_guest);
    }
    {
        unsigned int radio;
        for (radio = 0; radio < 2; radio++)
            if (config.beacon_interval[radio] && wlan_profiles_set_beacon_interval(radio, config.beacon_interval[radio])) {
                printf("ZTE_MESH_SERVICE fail=beacon-interval radio=%u\n", radio);
                fflush(NULL);
                return -1;
            }
    }
    controller_set_extra_bss(config.bss_set, config.bss_enabled, config.bss_ssid, config.bss_key);
    controller_set_extra_bss_style(config.bss_hidden, config.bss_beacon);
    controller_set_main_style(config.main_hidden, config.main_beacon);
    controller_set_acl(config.acl, config.acl_count, config.acl_policy);
    controller_set_v2(config.v2_enable);
    controller_set_role(config.role_set ? config.role : CONTROLLER_ROLE_UNSET);
    controller_set_wlan_setting(config.dual_band_sync, config.mlo_enable, config.essid_suffix);
    {
        struct mesh_startup_policy policy = { .essid = (const char *)config.profiles[0].ssid,
            .roam_set = config.roam_set, .roam24 = config.roam_rssi24, .roam5 = config.roam_rssi5,
            .steer_set = config.steer_set, .steer_enabled = config.steer_enabled,
            .steer_rssi_set = config.steer_rssi_set, .steer24 = config.steer_rssi24, .steer5 = config.steer_rssi5,
            .force_set = config.force_set, .force = config.force_on };
        if (config.roam_set || config.steer_set || config.steer_rssi_set || config.force_set)
            mesh_policies_set_startup_policy(&policy);
    }
    if (controller_boot(config.profiles, version, 0, &child, &id))
        return -1;
    {
        unsigned int r, k;
        for (r = 0; r < 2; r++)
            for (k = 1; k < 4; k++)
                if (config.bss_set[r][k] && config.bss_wifi5[r][k] >= 0 && wlan_profiles_set_force_wifi5(r, k, config.bss_wifi5[r][k]))
                    printf("ZTE_MESH_SERVICE force_wifi5 radio=%u slot=%u status=failed\n", r, k);
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    state.started = now.tv_sec;
    state.running = 1;
    state.daemon = child;
    state.version_mode = config.version_mode;
    printf("ZTE_MESH_SERVICE running=1 version_mode=%u\n", config.version_mode);
    fflush(NULL);
    (void)mkdir(SERVICE_DIR "/peers", 0755);
    {
        /* digests of an earlier run say nothing about the agents of this one */
        DIR *dir = opendir(SERVICE_DIR "/peers");
        struct dirent *entry;
        char path[128];
        while (dir && (entry = readdir(dir)) != NULL)
            if (entry->d_name[0] != '.' && snprintf(path, sizeof(path), SERVICE_DIR "/peers/%s", entry->d_name) < (int)sizeof(path))
                (void)unlink(path);
        if (dir)
            closedir(dir);
    }
    (void)mkdir(ACTION_DIR, 0700);
    /* A daemon in the agent role is not a controller: the node actions, renews, policy pushes and radio/MLO pushes below
     * all address *its* agents, which do not exist (docs/research/AGENT_PROBE.md). */
    int slave = config.role_set && config.role == CONTROLLER_ROLE_SLAVE;
    if (slave)
        printf("ZTE_MESH_SERVICE role=slave controller_features=off epoch=%ld\n", (long)time(NULL));
    action_reconcile(0); /* a request from before this start is stale: a reboot must not be replayed */
    int renew_sent = 0, election_attempts = 0;
    struct radio_push radio_push[2] = {{0, 0, 0}, {0, 0, 0}};
    struct radio_agent radio_agent = {{0}, 0};
    struct mlo_push mlo_push[MLO_NODES];
    memset(mlo_push, 0, sizeof(mlo_push));
    for (;;) {
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT)) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (info.si_pid == child)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!renew_sent && now.tv_sec - state.started >= 5) {
            /* Once the daemon's acd task is up, broadcast an AP-autoconfiguration
             * Renew so an agent that had onboarded against a previous controller
             * instance re-runs autoconfiguration against this one instead of
             * staying put. Standard Multi-AP behaviour; harmless if no agent.
             * A daemon that runs as the agent has no agents of its own: skip it. */
            renew_sent = 1;
            if (slave)
                printf("ZTE_MESH_SERVICE startup_renew=skipped role=slave epoch=%ld\n", (long)time(NULL));
            else
                printf("ZTE_MESH_SERVICE startup_renew=%d epoch=%ld\n", controller_renew_agents(), (long)time(NULL));
            fflush(NULL);
        }
        /* The agent's Search is what starts EasyMesh: cspd's cooperator sets g_TriggerSelectFlag once the interface
         * has an address and calls CfIeee1905RoleElect, and multiapd only reaches that entry through its acd task.
         * There is no cspd here, so the same probe event zte-meshctl sends for the renew is poked at it directly
         * (0x909b, multiapd.acd.acd, docs/research/AGENT_PROBE.md section 5.3). The payload is a four-byte zero
         * probe: if the trigger does not exist the status is non-zero and nothing is lost. Retried a few times
         * because the acd task is not necessarily up when the daemon reports itself started. */
        if (slave && election_attempts < 3 && now.tv_sec - state.started >= 8 + election_attempts * 12) {
            int status = controller_election_trigger();
            election_attempts++;
            printf("ZTE_MESH_SERVICE agent_election attempt=%d status=%d epoch=%ld\n", election_attempts, status,
                   (long)time(NULL));
            fflush(NULL);
        }
        apply_new_config(&config, &stamp, &state);
        if (state.respawn_pending) {
            pid_t old = child;
            int status = controller_respawn_daemon(&child);
            state.respawn_pending = 0;
            printf("ZTE_MESH_SERVICE daemon_respawn old=%d new=%d status=%d epoch=%ld\n", (int)old, (int)child, status,
                   (long)time(NULL));
            fflush(NULL);
            if (status) {
                /* nothing runs any more: leave, the init system starts the service afresh */
                state.running = 0;
                state.daemon = 0;
                state.restart_required = 1;
                snprintf(state.config_error, sizeof(state.config_error), "daemon respawn failed");
                (void)write_state(&state);
                mb_wipe(&config, sizeof(config));
                (void)controller_shutdown_private(id);
                return -1;
            }
            /* the new daemon starts with an empty topology: forget what was pushed to the agents of the old one,
             * and the policy messages that were waiting (it reads the database again anyway) */
            state.daemon = child;
            state.respawns++;
            state.notify_pending = 0;
            state.notify_attempts = 0;
            memset(mlo_push, 0, sizeof(mlo_push));
            memset(radio_push, 0, sizeof(radio_push));
            radio_agent.mac[0] = 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (state.role_settle_at && now.tv_sec >= state.role_settle_at && !state.respawn_pending) {
            /* stand-in for cspd's cooperator_mgr + the daemon's _autoSetRole (router mode: master): Mode 0 -> 2, then the
             * second restart that _autoSetRole requests with ASEND(0xa93d) */
            int status = controller_write_cfg("Mode", 2);
            state.role_settle_at = 0;
            printf("ZTE_MESH_SERVICE role_settle mode=2 status=%d epoch=%ld\n", status, (long)time(NULL));
            if (status) {
                state.restart_required = 1;
                snprintf(state.config_error, sizeof(state.config_error), "role settle failed, restart required");
            } else {
                state.respawn_pending = 1;
            }
        }
        if (now.tv_sec != last_state.tv_sec) {
            sched_tick(&config, &state, time(NULL), now.tv_sec);
            cfg_watch();
            if (!slave) {
                radio_reconcile(&config, &state, radio_push, &radio_agent, now.tv_sec);
                mlo_reconcile(&config, mlo_push, now.tv_sec);
                if (!state.respawn_pending)
                    action_reconcile(1);
                policy_notify_reconcile(&state, now.tv_sec);
            } else {
                /* A child's Wi-Fi profile comes from the M2 the controller sends, which the daemon writes into
                 * WLANCfg/WLANPSK and announces with CmMultiAPWlanRenewNotify. Its subscribers in the factory
                 * image are cspd and the driver layer, neither of which exists here, so the rows the notify is
                 * about are polled and turned into the host-side plan (agent-sync.c, docs/research/AGENT_PROBE.md
                 * section 11). This is idempotent: it writes only when the rows changed. */
                (void)agent_sync_poll(WLAN_PLAN_PATH);
            }
            last_state = now;
            state.sequence++;
            if (write_state(&state))
                printf("ZTE_MESH_SERVICE state_write=failed\n");
            fflush(NULL);
        }
        nanosleep(&pause_time, NULL);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    state.running = 0;
    state.daemon = 0;
    (void)write_state(&state);
    printf("ZTE_MESH_SERVICE running=0 exited=%d exit=%d signal=%d\n", WIFEXITED(status),
           WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(NULL);
    mb_wipe(&config, sizeof(config));
    (void)controller_shutdown_private(id);
    return 1;
}
