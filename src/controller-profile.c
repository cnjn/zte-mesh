/* Controller-only logical profile readers, NOT a local radio/driver backend.
 * Factory CM setters/CSP exchanges are not executed. DB GET errors stay errors.
 * Only two approved fixture profiles and WPA2-PSK/AES are currently supported.
 */
#define _GNU_SOURCE
#include "mesh-bridge.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))
/* See controller-trace.c: -Werror=unused-result rejects `(void)write(...)` on GCC 11. */
static void emit(const char *data, size_t len)
{
    ssize_t written = write(STDERR_FILENO, data, len);
    (void)written;
}
#define GUARD 0xa5a5a5a5U
#define CM_AP_BYTES 2496U
#define CM_PSK_BYTES 324U
#define FIELD(label, dtype, key, saved, value) {label, dtype},
#define PROFILE_FIELD FIELD
struct field { const char *name; unsigned int type; };
static const struct field cfg_fields[] = {
#include "wlan_cfg.def"
};
static const struct field psk_fields[] = {
#include "wlan_psk.def"
};
#undef PROFILE_FIELD
#undef FIELD
struct copy { unsigned short source, dest, bytes, type; };
#define CM_COPY(source, dest, bytes, type) {source, dest, bytes, type},
static const struct copy copies[] = {
#include "cm-profile-layout.inc"
};
#undef CM_COPY
struct query { uint32_t before[4]; uint8_t data[2312]; uint32_t after[4]; };
/* Byte-buffer readers only; no pointer-sized DB domain struct is used here. */
extern unsigned int dbGetPmTypeValLen(unsigned int);
extern unsigned int dbGetPrmValLen(unsigned int);
extern int dbAPIGetView(const char *, const char *, void *);
extern int WlanIndex2InstId(unsigned int, unsigned int, char *);
extern int WlanIndex2PSKInstId(unsigned int, unsigned int, char *);
extern int WlanCardIndex2BaseInstId(unsigned int, char *);
/* Verified pure profile classifiers: no DB writes, CSP or driver calls. */
extern int wlan_get_websec(void *);
extern void wlan_set_wifi_ssid_type(void *);

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}
static void mask(void *p, unsigned int payload)
{
    uint16_t v = 1;
    memcpy((uint8_t *)p + payload - 26, &v, sizeof(v));
}
static void log_status(const char *api, int status)
{
    static atomic_uint count;
    char text[128];
    int n;
    if (atomic_fetch_add(&count, 1) >= 48) return;
    n = snprintf(text, sizeof(text), "ZTE_CM_PROFILE api=%s status=%d logical_profile=1 hardware_claimed=0\n", api, status);
    if (n > 0 && (size_t)n < sizeof(text)) emit(text, (size_t)n);
}
/* ifidx is the BSS slot within the radio. Only slot 0 carries a logical
 * profile; acdGetWLANCFG nevertheless walks slots 0..3 and aborts the whole
 * walk (leaving the M2 tear-down byte at its default of 1) if ANY slot's
 * CmWLANGetAPConfig fails, so the other slots must answer "not instantiated"
 * instead of an error. */
static int radio_for(const char *instance, int psk, unsigned int *ifidx)
{
    const char *sandbox = getenv("ZTE_MESH_DB_SANDBOX");
    unsigned int radio, idx;
    char ap[32], key[32];
    if (!sandbox || strcmp(sandbox, "1") || !instance || strnlen(instance, 32) == 32)
        return -1;
    for (radio = 0; radio < 2; radio++) {
        for (idx = 0; idx < 4; idx++) {
            if (WlanIndex2InstId(radio, idx, ap) || WlanIndex2PSKInstId(radio, idx, key)) return -1;
            if (!strcmp(instance, ap) || (psk && !strcmp(instance, key))) {
                if (ifidx) *ifidx = idx;
                return (int)radio;
            }
        }
    }
    return -1;
}
static int read_query(const char *view, const char *key, struct query *q,
                      const struct field *fields, unsigned int count, unsigned int bytes)
{
    unsigned int i, pos = 8;
    uint32_t magic = 0x01020304;
    int status;
    memset(q, 0xa5, sizeof(*q)); memset(q->data, 0, sizeof(q->data));
    memcpy(q->data, &bytes, 4); memcpy(q->data + 4, &magic, 4);
    status = dbAPIGetView(view, key, q->data);
    if (status) return status;
    if (memcmp(q->data, &bytes, 4) || memcmp(q->data + 4, &magic, 4)) return -1;
    for (i = 0; i < 4; i++) if (q->before[i] != GUARD || q->after[i] != GUARD) return -1;
    for (i = bytes; i < sizeof(q->data); i++) if (q->data[i]) return -1;
    for (i = 0; i < count; i++) {
        uint16_t type;
        uint32_t error;
        unsigned int n = dbGetPmTypeValLen(fields[i].type), cap = dbGetPrmValLen(fields[i].type);
        if (n < 28 || pos > bytes || n > bytes - pos) return -1;
        memcpy(&type, q->data + pos, 2); memcpy(&error, q->data + pos + 24, 4);
        if (type != fields[i].type || error) return -1;
        if (type < 12 && (cap <= 4 || !memchr(q->data + pos + 28, 0, cap - 4))) return -1;
        pos += n;
    }
    return pos == bytes ? 0 : -1;
}
static int cfg_read(unsigned int radio, unsigned int ifidx, struct query *q, char *key)
{
    int status;
    if (WlanIndex2InstId(radio, ifidx, key)) return -1;
    status = read_query("WLANCfg", key, q, cfg_fields, 53, 2312);
    if (!status) {
        uint32_t beacon, auth, cipher;
        memcpy(&beacon, q->data + 900, 4); memcpy(&cipher, q->data + 1188, 4);
        memcpy(&auth, q->data + 1220, 4);
        /* PSK + AES fields only. The factory UI encodes the whole security mode in BeaconType alone and leaves
         * 11iAuthMode / 11iEncryptType untouched (FEATURE_MAP section 12): 0 open, 3 WPA2, 6 WPA + WPA2,
         * 10 WPA3, 12 WPA3 + WPA2. The daemon derives the M2 authentication types from it and offers the agent
         * only those it advertised in its own M1 (a Z506 has no SAE, so 12 reaches it as WPA2 and 10 as none).
         * EAP and WEP are not profiles this controller can push. */
        if ((beacon != 0 && beacon != MB_VENDOR_BEACON_WPA2 && beacon != 6 && beacon != 10 && beacon != 12) ||
            auth != MB_VENDOR_AUTH_PSK || cipher != MB_VENDOR_CIPHER_AES)
            return -95;
    }
    return status;
}

/* Value offset of a WLANCfg field in the view buffer (header 8 bytes, then one record per field; the value starts 28
 * bytes into its record), 0 if unknown. */
static unsigned int cfg_value_offset(const char *name)
{
    unsigned int i, pos = 8;
    for (i = 0; i < 53; i++) {
        if (!strcmp(cfg_fields[i].name, name))
            return pos + 28;
        pos += dbGetPmTypeValLen(cfg_fields[i].type);
    }
    return 0;
}

EXPORT int CmWLANGetAPConfig(const char *instance, void *output)
{
    struct query q;
    char key[32], base[32];
    uint8_t data[CM_AP_BYTES];
    uint32_t index = 0, band;
    unsigned int i;
    unsigned int ifidx = 0;
    int radio = radio_for(instance, 0, &ifidx), status = -5, missing = 0;
    if (!output || radio < 0) goto done;
    status = cfg_read((unsigned int)radio, ifidx, &q, key);
    /* A slot without its own row is "not instantiated": answer with slot 0's
     * record and InstExist cleared, never an error (an error aborts the walk). */
    if (status && ifidx) { missing = 1; status = cfg_read((unsigned int)radio, 0, &q, key); }
    if (status) goto done;
    if (WlanCardIndex2BaseInstId((unsigned int)radio, base)) { status = -1; goto done; }
    /* Preserve caller selection/mode records; touch only proven output fields. */
    memcpy(data, output, sizeof(data));
    for (i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        memcpy(data + copies[i].dest, q.data + copies[i].source, copies[i].bytes);
        mask(data, copies[i].dest);
    }
    band = (uint32_t)radio; /* Approved logical band slots, not RF capabilities. */
    index = ifidx; memcpy(data + 28, &index, 4); mask(data, 28);
    memcpy(data + 60, &band, 4); mask(data, 60);
    memset(data + 316, 0, 36); memcpy(data + 316, base, strlen(base)); mask(data, 316);
    /* The real getter also derives the output's Enable byte (offset 412, set flag at 386) from the row: without it the
     * ACD reads 0 and tears every BSS down unless a per-BSS override (P1905AcdBssCfg CustomTag bit 0, read once when the
     * daemon starts) says otherwise. Filling it from WLANCfg.Enable makes the enable bit live at every Renew, as at the
     * factory (REPLAY_RESULTS section 14). */
    { unsigned int at = cfg_value_offset("Enable"); if (at) { data[412] = q.data[at] ? 1 : 0; mask(data, 412); } }
    if (missing) { memset(data + 380, 0, 1); data[412] = 0; } /* InstExist=0: slot not instantiated */
    status = wlan_get_websec(data);
    if (!status) wlan_set_wifi_ssid_type(data);
    if (!status) memcpy(output, data, sizeof(data));
done:
    wipe(&q, sizeof(q)); wipe(data, sizeof(data));
    log_status("CmWLANGetAPConfig", status);
    return status;
}

EXPORT int CmWLANGetPsk(const char *instance, void *output)
{
    struct query cfg, q;
    char ap[32], key[32];
    uint8_t data[CM_PSK_BYTES] = {0};
    size_t length;
    unsigned int ifidx = 0;
    int radio = radio_for(instance, 1, &ifidx), status = -5;
    if (!output || radio < 0) goto done;
    status = cfg_read((unsigned int)radio, ifidx, &cfg, ap);
    if (status) goto done;
    if (WlanIndex2PSKInstId((unsigned int)radio, ifidx, key)) { status = -1; goto done; }
    status = read_query("WLANPSK", key, &q, psk_fields, 4, 328);
    if (status) goto done;
    length = strnlen((const char *)q.data + 260, 64);
    if (length < 8 || length > 63 || strcmp((char *)q.data + 100, ap)) { status = -95; goto done; }
    /* CM ViewName is WLANViewName (the AP), not the WLANPSK row key. */
    memcpy(data + 28, q.data + 100, 36); mask(data, 28);
    memcpy(data + 92, q.data + 164, 68); mask(data, 92);
    memcpy(data + 188, q.data + 260, 68); mask(data, 188);
    memcpy(output, data, sizeof(data));
done:
    wipe(&cfg, sizeof(cfg)); wipe(&q, sizeof(q)); wipe(data, sizeof(data));
    log_status("CmWLANGetPsk", status);
    return status;
}
