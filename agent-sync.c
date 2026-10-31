/* The library-to-hostapd synchroniser of the child (agent) role. See agent-sync.h for the contract.

 * What is read: the WLANCfg and WLANPSK rows the daemon's M2 apply writes (acdcConfigM2EncryptedSettings ->
 * MeshWlanSetAPCfgByBandAndIfIndex / acdcWlanSetPWKey). Field offsets are walked from the pinned view
 * definitions rather than hard-coded, exactly the way controller-profile.c reads the same rows: a row is a
 * size word, a magic word, then one typed record per field whose value starts 28 bytes into the record.
 *
 * The rows are read through dbAPIGetView() without the shared-memory pool lock, the same way the lab's
 * dump_psk_rows() reads them (the setter path reads them the same way too). A read that races a write can
 * therefore see a row half updated; the poll repeats every second, so the next pass publishes the settled
 * state. Taking the pool lock around a dbAPIGetView() is not obviously safe - the engine may take it itself.
 */
#define _GNU_SOURCE
#include "agent-sync.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_MAGIC 0x01020304U
#define DB_CFG_BYTES 2312U   /* WLANCfg row: 53 fields, the size CmWLANGetAPConfig is given */
#define DB_PSK_BYTES 328U    /* WLANPSK row: ViewName, WLANViewName, PreSharedKey, KeyPassphrase */
#define DB_HEADER 8U
#define DB_VALUE_OFFSET 28U
#define PLAN_BYTES AGENT_SYNC_PLAN_BYTES

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

extern int dbAPIGetView(const char *, const char *, void *);
extern unsigned int dbGetPmTypeValLen(unsigned int);
extern unsigned int dbGetPrmValLen(unsigned int);
extern int WlanIndex2InstId(unsigned int, unsigned int, char *);
extern int WlanIndex2PSKInstId(unsigned int, unsigned int, char *);

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--)
        *v++ = 0;
}

/* Where a field lives inside the row buffer and what type it has, {0, 0} when the name is not in the definition. */
struct field_ref { unsigned int offset, type; };
static struct field_ref field_ref_of(const struct field *fields, unsigned int count, const char *name)
{
    struct field_ref ref = {0, 0};
    unsigned int i, pos = DB_HEADER;
    for (i = 0; i < count; i++) {
        unsigned int bytes = dbGetPmTypeValLen(fields[i].type);
        if (!bytes)
            return ref;
        if (!strcmp(fields[i].name, name)) {
            ref.offset = pos + DB_VALUE_OFFSET;
            ref.type = fields[i].type;
            return ref;
        }
        pos += bytes;
    }
    return ref;
}

static struct field_ref cfg_field(const char *name)
{
    return field_ref_of(cfg_fields, sizeof(cfg_fields) / sizeof(cfg_fields[0]), name);
}

static struct field_ref psk_field(const char *name)
{
    return field_ref_of(psk_fields, sizeof(psk_fields) / sizeof(psk_fields[0]), name);
}

/* A scalar cell: one, two or four bytes, little endian (the type lengths are what dbGetPrmValLen reports). */
static unsigned int scalar_at(const uint8_t *row, unsigned int offset, unsigned int type)
{
    unsigned int bytes = dbGetPrmValLen(type), value = 0;
    if (bytes > 4)
        bytes = 4;
    memcpy(&value, row + offset, bytes);
    return value;
}

/* Read one row the way the factory finder does, and check the header it echoes back. */
static int row_read(const char *view, const char *instance, uint8_t *buffer, uint32_t bytes)
{
    uint32_t magic = DB_MAGIC;
    int status;
    if (!instance || !*instance)
        return -1;
    memset(buffer, 0, bytes);
    memcpy(buffer, &bytes, sizeof(bytes));
    memcpy(buffer + sizeof(bytes), &magic, sizeof(magic));
    status = dbAPIGetView(view, instance, buffer);
    if (status || memcmp(buffer, &bytes, sizeof(bytes)) || memcmp(buffer + sizeof(bytes), &magic, sizeof(magic)))
        return -1;
    return 0;
}

/* Text that survives a line-oriented consumer: the vendor rejects control characters in these fields too, but a
 * plan that a shell cannot parse is worse than one with a replacement character. */
static void copy_text(char *out, size_t capacity, const char *text, size_t bytes, size_t *written)
{
    size_t i, n = 0;
    for (i = 0; i < bytes && n + 1 < capacity; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!c)
            break;
        out[n++] = (c < 0x20 || c > 0x7e) ? '?' : (char)c;
    }
    out[n] = '\0';
    if (written)
        *written = n;
}

int agent_sync_read(struct agent_sync_row *rows, char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES],
                    unsigned int *count)
{
    uint8_t cfg[DB_CFG_BYTES], psk[DB_PSK_BYTES];
    unsigned int radio, slot, used = 0;
    static int layout_logged;
    if (!rows || !keys || !count)
        return -1;
    if (!layout_logged) {
        /* The field map as this build walks it: if a vendor image ever moves a field, the plan names the new
         * offsets instead of silently publishing an empty value. */
        layout_logged = 1;
        printf("ZTE_AGENT_SYNC layout essid=%u enable=%u hide=%u access=%u beacon=%u key=%u passphrase=%u record2=%u record3=%u\n",
               cfg_field("ESSID").offset, cfg_field("Enable").offset, cfg_field("ESSIDHideEnable").offset,
               cfg_field("MapAccessAttr").offset, cfg_field("BeaconType").offset, psk_field("PreSharedKey").offset, psk_field("KeyPassphrase").offset,
               dbGetPmTypeValLen(2), dbGetPmTypeValLen(3));
        fflush(NULL);
    }
    memset(rows, 0, sizeof(struct agent_sync_row) * AGENT_SYNC_ROWS);
    memset(keys, 0, AGENT_SYNC_KEY_BYTES * AGENT_SYNC_ROWS);
    for (radio = 0; radio < 2; radio++) {
        for (slot = 0; slot < 4 && used < AGENT_SYNC_ROWS; slot++) {
            struct agent_sync_row *row = &rows[used];
            char ap[36] = "", psk_name[36] = "";
            struct field_ref ssid = cfg_field("ESSID"), enable = cfg_field("Enable"), hide = cfg_field("ESSIDHideEnable");
            struct field_ref access = cfg_field("MapAccessAttr"), beacon = cfg_field("BeaconType");
            struct field_ref key = psk_field("PreSharedKey"), key_pass = psk_field("KeyPassphrase");
            if (WlanIndex2InstId(radio, slot, ap) || WlanIndex2PSKInstId(radio, slot, psk_name))
                continue;
            if (!ssid.offset || !enable.offset || !hide.offset || !access.offset || !beacon.offset ||
                row_read("WLANCfg", ap, cfg, sizeof(cfg)))
                continue;
            row->radio = radio;
            row->slot = slot;
            copy_text(row->instance, sizeof(row->instance), ap, strlen(ap), NULL);
            copy_text(row->psk_instance, sizeof(row->psk_instance), psk_name, strlen(psk_name), NULL);
            copy_text(row->ssid, sizeof(row->ssid), (const char *)cfg + ssid.offset, sizeof(row->ssid) - 1, NULL);
            row->ssid_length = (unsigned int)strlen(row->ssid);
            row->enabled = scalar_at(cfg, enable.offset, enable.type);
            row->hidden = scalar_at(cfg, hide.offset, hide.type);
            row->access_attr = scalar_at(cfg, access.offset, access.type);
            row->beacon_type = scalar_at(cfg, beacon.offset, beacon.type);
            row->readable = 1;
            if ((key.offset || key_pass.offset) && !row_read("WLANPSK", psk_name, psk, sizeof(psk))) {
                /* KeyPassphrase first: that is the cell the vendor's own CmWLANGetPsk reads (libcmapi copies
                 * offset 260 of the row into the CM key), and it is where an ASCII passphrase lands.
                 * PreSharedKey (offset 164) is the fallback for a raw 64-hex key. */
                unsigned int at = key_pass.offset && psk[key_pass.offset] ? key_pass.offset : key.offset;
                size_t n = 0;
                if (at && psk[at])
                    copy_text(keys[used], AGENT_SYNC_KEY_BYTES, (const char *)psk + at, AGENT_SYNC_KEY_BYTES - 1, &n);
                row->key_length = (unsigned int)n;
                {
                    uint32_t hash = 2166136261U;
                    size_t i;
                    for (i = 0; i < n; i++)
                        hash = (hash ^ (uint8_t)keys[used][i]) * 16777619U;
                    row->key_hash = hash;
                }
                wipe(psk, sizeof(psk));
            }
            used++;
        }
    }
    *count = used;
    return 0;
}

int agent_sync_changed(const struct agent_sync_row *before, unsigned int before_count,
                       const struct agent_sync_row *after, unsigned int after_count)
{
    unsigned int i;
    if (before_count != after_count)
        return 1;
    for (i = 0; i < after_count; i++) {
        const struct agent_sync_row *a = &before[i], *b = &after[i];
        if (a->radio != b->radio || a->slot != b->slot || strcmp(a->instance, b->instance) ||
            strcmp(b->ssid, a->ssid) || a->ssid_length != b->ssid_length || a->enabled != b->enabled ||
            a->hidden != b->hidden || a->access_attr != b->access_attr || a->beacon_type != b->beacon_type ||
            a->key_length != b->key_length || a->key_hash != b->key_hash)
            return 1;
    }
    return 0;
}

/* One value, ready for a line-oriented consumer. The reader already sanitises what it copies out of the
 * database; doing it again here means the plan stays parseable even if a caller fills a row by hand. */
static const char *plan_text(char *buffer, size_t capacity, const char *value)
{
    size_t i, n = 0;
    for (i = 0; value && value[i] && n + 1 < capacity; i++) {
        unsigned char c = (unsigned char)value[i];
        buffer[n++] = (c < 0x20 || c > 0x7e) ? '?' : (char)c;
    }
    buffer[n] = '\0';
    return buffer;
}

int agent_sync_format(const struct agent_sync_row *rows, const char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES],
                      unsigned int count, char *out, size_t capacity)
{
    size_t used = 0;
    unsigned int i;
    int n;
    char ssid[sizeof(rows[0].ssid) + 1], key[AGENT_SYNC_KEY_BYTES + 1];
    char instance[sizeof(rows[0].instance) + 1], psk_instance[sizeof(rows[0].psk_instance) + 1];
    if (!rows || !keys || !out || !capacity || count > AGENT_SYNC_ROWS)
        return -1;
    n = snprintf(out, capacity,
                 "# zte-mesh agent synchronisation plan. Generated by the service; applied by wlan-sync.\n"
                 "# Line format key=value, one option per line, value is everything after the first '='.\n"
                 "rows=%u\n", count);
    if (n < 0 || (size_t)n >= capacity)
        return -1;
    used = (size_t)n;
    for (i = 0; i < count; i++) {
        const struct agent_sync_row *row = &rows[i];
        if (!row->readable)
            continue;
        n = snprintf(out + used, capacity - used,
                     "%u.radio=%u\n%u.slot=%u\n%u.instance=%s\n%u.psk_instance=%s\n%u.ssid=%s\n%u.key=%s\n"
                     "%u.enabled=%u\n%u.hidden=%u\n%u.access=%u\n%u.beacon_type=%u\n",
                     i, row->radio, i, row->slot, i,
                     plan_text(instance, sizeof(instance), row->instance), i,
                     plan_text(psk_instance, sizeof(psk_instance), row->psk_instance), i,
                     plan_text(ssid, sizeof(ssid), row->ssid), i, plan_text(key, sizeof(key), keys[i]), i,
                     row->enabled, i, row->hidden, i, row->access_attr, i, row->beacon_type);
        if (n < 0 || (size_t)n >= capacity - used)
            return -1;
        used += (size_t)n;
    }
    return 0;
}

/* Write and rename, so the applier never sees half a plan. 0600: the file carries passphrases. */
static int plan_write(const char *path, const char *text, size_t bytes)
{
    char temp[160];
    int fd, status = -1, saved;
    if (!path || snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return -1;
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, text, bytes) == (ssize_t)bytes && fsync(fd) == 0)
        status = 0;
    saved = errno;
    (void)close(fd);
    if (!status && rename(temp, path))
        status = -1;
    if (status)
        (void)unlink(temp);
    errno = saved;
    return status;
}

static struct agent_sync_row published[AGENT_SYNC_ROWS];
static unsigned int published_count;
static int published_valid;

void agent_sync_reset(void)
{
    memset(published, 0, sizeof(published));
    published_count = 0;
    published_valid = 0;
}

int agent_sync_poll(const char *path)
{
    struct agent_sync_row rows[AGENT_SYNC_ROWS];
    char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES];
    char plan[PLAN_BYTES];
    unsigned int count = 0, i;
    int status;
    size_t plan_bytes;
    if (agent_sync_read(rows, keys, &count))
        return -1;
    if (published_valid && !agent_sync_changed(published, published_count, rows, count)) {
        wipe(keys, sizeof(keys));
        return 0;
    }
    if (agent_sync_format(rows, keys, count, plan, sizeof(plan))) {
        wipe(keys, sizeof(keys));
        wipe(plan, sizeof(plan));
        return -1;
    }
    plan_bytes = strlen(plan);
    status = plan_write(path, plan, plan_bytes);
    wipe(keys, sizeof(keys));
    wipe(plan, sizeof(plan));
    if (status)
        return -1;
    memcpy(published, rows, sizeof(published));
    published_count = count;
    published_valid = 1;
    printf("ZTE_AGENT_SYNC published=1 path=%s rows=%u plan_bytes=%u", path ? path : "-", count,
           (unsigned int)plan_bytes);
    for (i = 0; i < count; i++)
        printf(" [%u/%u ssid=%s enabled=%u beacon=%u key_len=%u]", rows[i].radio, rows[i].slot, rows[i].ssid,
               rows[i].enabled, rows[i].beacon_type, rows[i].key_length);
    printf("\n");
    fflush(NULL);
    return 1;
}
