/* Host boundary tests only; native factory PSK equivalence is tested on device. */
#define _GNU_SOURCE
#include "../src/mesh-bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
struct field { const char *name; unsigned int type; };
#define PROFILE_FIELD(label, dtype, key, saved, value) {label, dtype},
static const struct field cfg[] = {
#include "../src/wlan_cfg.def"
};
static const struct field psk[] = {
#include "../src/wlan_psk.def"
};
#undef PROFILE_FIELD
static int fault, tests;
unsigned int dbGetPrmValLen(unsigned int t)
{
    switch (t) {
    case 0:return 12; case 1:return 20; case 2:return 36; case 3:return 68;
    case 5:return 260; case 15:return 1; case 18:return 6; case 22:return 8;
    default:return t >= 12 && t <= 16 ? 4 : 0;
    }
}
unsigned int dbGetPmTypeValLen(unsigned int t)
{
    unsigned int n = dbGetPrmValLen(t);
    return n ? 28 + ((n + 3) & ~3U) : 0;
}
int WlanIndex2InstId(unsigned int radio, unsigned int index, char *out)
{
    if (radio > 1 || index) return -1;
    snprintf(out, 32, "DEV.WIFI.AP%u", radio ? 9 : 1); return 0;
}
int WlanIndex2PSKInstId(unsigned int radio, unsigned int index, char *out)
{
    if (WlanIndex2InstId(radio, index, out)) return -1;
    strcat(out, ".PSK1"); return 0;
}
int WlanCardIndex2BaseInstId(unsigned int radio, char *out)
{
    snprintf(out, 32, "DEV.WIFI.BASE%u", radio + 1); return 0;
}
int dbAPIGetView(const char *view, const char *key, void *output)
{
    uint8_t *b = output;
    const struct field *fields = !strcmp(view, "WLANCfg") ? cfg : psk;
    unsigned int count = fields == cfg ? 53 : 4, offset = 8, i;
    if (fault == 1) return -2;
    for (i = 0; i < count; i++) {
        uint16_t type = fields[i].type;
        uint32_t v = 0;
        const char *name = fields[i].name;
        memcpy(b + offset, &type, 2);
        if (!strcmp(name, "Enable") || !strcmp(name, "11iEncryptType")) v = 1;
        if (!strcmp(name, "ForceWiFi5")) v = 1; /* must reach struct offset 2268 (the ACD's vendor sub-TLV 0x46), not ForceWiFi6's 2300 */
        if (!strcmp(name, "BeaconType")) v = fault == 7 ? 9 : 3; /* 9 is not a factory BeaconType (0, 3, 6, 10, 12) */
        if (!strcmp(name, "11iAuthMode") && fault == 8) v = 1;
        if (!strcmp(name, "11iEncryptType") && fault == 9) v = 0;
        if (type >= 12) memcpy(b + offset + 28, &v, dbGetPrmValLen(type));
        else if (!strcmp(name, "ViewName")) snprintf((char *)b + offset + 28, 32, "%s", key);
        else if (!strcmp(name, "ESSID")) strcpy((char *)b + offset + 28, "PUBLIC-CM-TEST");
        else if (!strcmp(name, "KeyPassphrase")) strcpy((char *)b + offset + 28, fault == 10 ? "short" : "public-unit-key");
        else if (!strcmp(name, "WLANViewName")) strcpy((char *)b + offset + 28, fault == 11 ? "WRONG-ROW" : "DEV.WIFI.AP1");
        offset += dbGetPmTypeValLen(type);
    }
    if (fault == 2) b[4] = 0;
    if (fault == 3) b[8] = 255;
    if (fault == 4) b[32] = 1;
    if (fault == 5) memset(b + 36, 'X', 32);
    if (fault == 6) b[2312] = 0; /* Deliberate overwrite of callee's outer guard. */
    return 0;
}
int wlan_get_websec(void *p) { (void)p; return 0; }
void wlan_set_wifi_ssid_type(void *p) { (void)p; }
extern int CmWLANGetAPConfig(const char *, void *);
extern int CmWLANGetPsk(const char *, void *);
static void check(int ok, const char *label)
{
    if (!ok) { fprintf(stderr, "FAIL CM profile case=%s\n", label); exit(1); }
    tests++;
}
int main(void)
{
    uint8_t data[2496], saved[2496];
    unsigned int i;
    setenv("ZTE_MESH_DB_SANDBOX", "1", 1);
    memset(data, 0, sizeof(data));
    check(!CmWLANGetAPConfig("DEV.WIFI.AP1", data) && !strcmp((char *)data + 444, "PUBLIC-CM-TEST"), "AP view");
    check(data[2268] == 1 && !data[2269] && !data[2270] && !data[2271] && !data[2300], "ForceWiFi5 copied to the 0x46 source, ForceWiFi6 not");
    memset(data, 0, sizeof(data));
    check(!CmWLANGetPsk("DEV.WIFI.AP1", data) && !strcmp((char *)data + 188, "public-unit-key"), "PSK view");
    memset(data, 0, sizeof(data));
    check(!CmWLANGetPsk("DEV.WIFI.AP1.PSK1", data) && !strcmp((char *)data + 28, "DEV.WIFI.AP1"), "native PSK caller form");
    for (i = 1; i <= 9; i++) {
        fault = (int)i; memset(data, 0xa5, sizeof(data)); memcpy(saved, data, sizeof(data));
        check(CmWLANGetAPConfig("DEV.WIFI.AP1", data) && !memcmp(saved, data, sizeof(data)), "bad DB/security unchanged");
    }
    for (i = 10; i <= 11; i++) {
        fault = (int)i; memset(data, 0xa5, sizeof(data)); memcpy(saved, data, sizeof(data));
        check(CmWLANGetPsk("DEV.WIFI.AP1", data) && !memcmp(saved, data, sizeof(data)), "bad key/association unchanged");
    }
    fault = 0;
    memset(data, 0xa5, sizeof(data)); memcpy(saved, data, sizeof(data));
    check(CmWLANGetAPConfig("DEV.WIFI.AP2", data) && !memcmp(saved, data, sizeof(data)), "unconfigured instance");
    check(CmWLANGetPsk("DEV.WIFI.AP1.PSK2", data) && !memcmp(saved, data, sizeof(data)), "unconfigured PSK instance");
    check(CmWLANGetAPConfig(NULL, data) && CmWLANGetPsk(NULL, data) && !memcmp(saved, data, sizeof(data)), "null instance");
    check(CmWLANGetAPConfig("DEV.WIFI.AP1", NULL) && CmWLANGetPsk("DEV.WIFI.AP1", NULL), "null output");
    unsetenv("ZTE_MESH_DB_SANDBOX");
    check(CmWLANGetAPConfig("DEV.WIFI.AP1", data) && !memcmp(saved, data, sizeof(data)), "no sandbox permission");
    printf("PASS: CM profile scenarios=%d read-only errors-preserve-output PUBLIC-fixtures-only\n", tests);
    return 0;
}
