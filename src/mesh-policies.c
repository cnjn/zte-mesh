/* Real vendor Master/Domain policy views; no WLAN profile or Controller. */
#include "mesh-policies.h"
#include "vendor-db.h"
#include <stdio.h>
#include <string.h>

#define MAGIC 0x01020304U
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GUARD 0xa5a5a5a5U

static const struct zte_db_domain master_fields[] = {
#define MESH_FIELD(label, dtype, key, saved, value) \
    { .name = label, .type = dtype, .primary_key = key, .save = saved, .default_value = value },
#include "multiap_master.def"
#undef MESH_FIELD
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain domain_fields[] = {
#define MESH_FIELD(label, dtype, key, saved, value) \
    { .name = label, .type = dtype, .primary_key = key, .save = saved, .default_value = value },
#include "multiap_domain.def"
#undef MESH_FIELD
    { .name = "", .default_value = "0" }
};
_Static_assert(ARRAY_SIZE(master_fields) == 56, "Master registry count");
_Static_assert(ARRAY_SIZE(domain_fields) == 107, "Domain registry count");

struct policy {
    const char *name;
    const struct zte_db_domain *fields;
    unsigned int count, bytes, max_rows;
};
static const struct policy policies[] = {
    { "MultiAPMaster", master_fields, 55, MESH_MASTER_SIZE, 1 },
    { "MultiAPDomain", domain_fields, 106, MESH_DOMAIN_SIZE, 32 }
};

extern int dbAPIGetView(const char *, const char *, void *);
extern int dbAPISetView(const char *, const char *, void *);
extern int MapGetMasterDB(void *);
extern int MapSetMasterDB(void *);
extern int CmMultiAPGetMasterCfg(const char *, void *);
extern int CmMultiAPGetDomainCfg(const char *, void *);
struct cm_list { uint32_t capacity, count; char *names; };
extern int CmMultiAPListDomain(const char *, struct cm_list *);

static const struct policy *spec(enum mesh_policy id)
{
    return (unsigned int)id < ARRAY_SIZE(policies) ? &policies[id] : NULL;
}

static size_t offset(const struct policy *p, unsigned int field)
{
    size_t pos = 8;
    unsigned int i;
    if (!p || field > p->count)
        return 0;
    for (i = 0; i < field; i++) {
        unsigned int length = dbGetPmTypeValLen(p->fields[i].type);
        if (!length || length > p->bytes - pos)
            return 0;
        pos += length;
    }
    return pos;
}

static int field_number(const struct policy *p, const char *name)
{
    unsigned int i;
    if (!p || !name)
        return -1;
    for (i = 0; i < p->count; i++)
        if (!strcmp(name, p->fields[i].name))
            return i;
    return -1;
}

static int is_string(unsigned int type)
{
    return type == 2 || type == 3 || type == 5 || type == 7;
}

int mesh_policy_validate(enum mesh_policy id, const void *query, size_t capacity)
{
    const struct policy *p = spec(id);
    const uint8_t *q = query;
    uint32_t header[2], status;
    uint16_t type;
    unsigned int i;
    if (!p || !q || capacity != p->bytes || offset(p, p->count) != p->bytes)
        return -1;
    memcpy(header, q, sizeof(header));
    if (header[0] != p->bytes || header[1] != MAGIC)
        return -1;
    for (i = 0; i < p->count; i++) {
        size_t pos = offset(p, i);
        unsigned int length = dbGetPrmValLen(p->fields[i].type);
        memcpy(&type, q + pos, sizeof(type));
        memcpy(&status, q + pos + 24, sizeof(status));
        if (type != p->fields[i].type || status || !length ||
            (is_string(type) && !memchr(q + pos + 28, 0, length)))
            return -1;
    }
    return 0;
}

static int writable_field(enum mesh_policy id, const void *query, size_t capacity, const char *name)
{
    const struct policy *p = spec(id);
    uint32_t header[2];
    int n = field_number(p, name);
    if (!p || !query || capacity != p->bytes || n < 0 || p->fields[n].primary_key)
        return -1;
    memcpy(header, query, sizeof(header));
    if (header[0] != p->bytes || header[1] != MAGIC)
        return -1;
    return n;
}

int mesh_policy_set_scalar(enum mesh_policy id, void *query, size_t capacity, const char *name, uint32_t value)
{
    const struct policy *p = spec(id);
    uint16_t mask = 1;
    unsigned int type;
    uint8_t *q = query;
    int n = writable_field(id, query, capacity, name);
    size_t pos;
    if (n < 0)
        return -1;
    type = p->fields[n].type;
    if (type < 12 || type > 15 || (type == 15 && value > 255))
        return -1;
    pos = offset(p, n);
    if (!pos)
        return -1;
    memcpy(q + pos + 28, &value, type == 15 ? 1 : 4);
    memcpy(q + pos + 2, &mask, sizeof(mask));
    return 0;
}

int mesh_policy_set_string(enum mesh_policy id, void *query, size_t capacity, const char *name, const char *value)
{
    const struct policy *p = spec(id);
    uint16_t mask = 1;
    uint8_t *q = query;
    unsigned int length;
    size_t pos, used;
    int n = writable_field(id, query, capacity, name);
    if (n < 0 || !value || !is_string(p->fields[n].type))
        return -1;
    length = dbGetPrmValLen(p->fields[n].type);
    used = strlen(value);
    if (length <= 4 || used >= length - 4 || !(pos = offset(p, n)))
        return -1;
    memset(q + pos + 28, 0, length);
    memcpy(q + pos + 28, value, used);
    memcpy(q + pos + 2, &mask, sizeof(mask));
    return 0;
}

static uint32_t scalar(enum mesh_policy id, const void *query, const char *name)
{
    const struct policy *p = spec(id);
    int n = field_number(p, name);
    uint32_t value = 0;
    if (n >= 0 && p->fields[n].type >= 12 && p->fields[n].type <= 15)
        memcpy(&value, (const uint8_t *)query + offset(p, n) + 28,
               p->fields[n].type == 15 ? 1 : 4);
    return value;
}

static const char *string_value(enum mesh_policy id, const void *query, const char *name)
{
    const struct policy *p = spec(id);
    int n = field_number(p, name);
    return n >= 0 && is_string(p->fields[n].type) ? (const char *)query + offset(p, n) + 28 : NULL;
}

static int create_table(const struct policy *p, uint32_t **created)
{
    uint32_t descriptor[41] = {0};
    uint32_t *table;
    descriptor[0] = (uint32_t)(uintptr_t)p->name;
    descriptor[2] = p->max_rows;
    descriptor[3] = 1;
    if (offset(p, p->count) != p->bytes)
        return -1;
    table = dbCreateTbl(descriptor);
    if (!table || dbCreateDomainNew(table, p->fields, (p->count + 1) * sizeof(*p->fields)) ||
        table[18] != p->count || dbMgrCommViewAddItem(p->name, p->name, "", p->bytes,
                                                   1024, NULL, 63, p->name, "", ""))
        return -1;
    *created = table;
    return 0;
}

static char startup_essid[33];
static struct mesh_startup_policy startup_policy;
static int startup_policy_valid;

void mesh_policies_set_startup_policy(const struct mesh_startup_policy *policy)
{
    if (!policy || !policy->essid || !*policy->essid || strlen(policy->essid) >= sizeof(startup_essid))
        return;
    startup_policy = *policy;
    snprintf(startup_essid, sizeof(startup_essid), "%s", policy->essid);
    startup_policy.essid = startup_essid;
    startup_policy_valid = 1;
}

static uint32_t *startup_domain_table, *startup_master_table;

int mesh_policies_create_test_tables(void)
{
    uint32_t *master, *domain;
    if (create_table(&policies[MESH_MASTER], &master) ||
        create_table(&policies[MESH_DOMAIN], &domain) || !dbAddRow(master) ||
        dbSetValCommByRowNo(master, 0, "ViewName", "MULTIAPMASTER") ||
        dbSetValCommByRowNo(master, 0, "EnMultiDomain", "1") ||
        dbSetValCommByRowNo(master, 0, "EnRoam", "1") ||
        dbSetValCommByRowNo(master, 0, "ApPeriodRptIntval", "30") ||
        !dbAddRow(domain) || dbSetValCommByRowNo(domain, 0, "ViewName", MESH_DOMAIN_A) ||
        dbSetValCommByRowNo(domain, 0, "ESSID", "zte-shim-domain-a") ||
        dbSetValCommByRowNo(domain, 0, "RoamRssiLmt24G", "-65") ||
        dbSetValCommByRowNo(domain, 0, "DomainUsed", "1") ||
        !dbAddRow(domain) || dbSetValCommByRowNo(domain, 1, "ViewName", MESH_DOMAIN_B) ||
        dbSetValCommByRowNo(domain, 1, "ESSID", "zte-shim-domain-b") ||
        dbSetValCommByRowNo(domain, 1, "RoamRssiLmt24G", "-78") ||
        dbSetValCommByRowNo(domain, 1, "DomainUsed", "1") || master[27] != 1 || domain[27] != 2)
        return -1;
    startup_domain_table = domain;
    startup_master_table = master;
    return 0;
}


/* Optional baseline profile: /run/zte-mesh/db-profile, lines "TABLE ROW FIELD VALUE" (value = rest of line).
 * Written by the host from a vendor DB snapshot so the shim's policy rows carry the same values as the
 * original controller (the registry defaults are not what cspd leaves in the rows). Unknown fields are
 * counted as rejected, never fatal. Explicit service settings are applied AFTER this and win. */
#define DB_PROFILE_PATH "/run/zte-mesh/db-profile"
static void profile_apply(const char *table_name, void *table, unsigned int row_offset,
                          unsigned int *applied, unsigned int *rejected)
{
    FILE *f;
    char line[256], t[32], field[64], value[128];
    unsigned int row;
    if (!table || !(f = fopen(DB_PROFILE_PATH, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%31s %u %63s %127[^\n]", t, &row, field, value) != 4 || strcmp(t, table_name))
            continue;
        if (dbSetValCommByRowNo(table, row + row_offset, field, value))
            (*rejected)++;
        else
            (*applied)++;
    }
    fclose(f);
}

int mesh_policies_apply_profile_view(const char *table_name, void *table)
{
    unsigned int applied = 0, rejected = 0;
    profile_apply(table_name, table, 0, &applied, &rejected);
    if (applied || rejected)
        printf("ZTE_CONTROLLER_STARTUP db_profile table=%s applied=%u rejected=%u\n", table_name, applied, rejected);
    return 0;
}

/* The actual daemon requests MULTIAPDOMAIN1..8, unlike synthetic row tests.
 * Called under the server pool lock; unused Controller rows have no PSK.
 */
int mesh_policies_startup_rows(void)
{
    unsigned int row;
    char key[32];
    if (!startup_domain_table || startup_domain_table[27] != 2 ||
        dbSetValCommByRowNo(startup_domain_table, 0, "DomainUsed", "0") ||
        dbSetValCommByRowNo(startup_domain_table, 1, "DomainUsed", "0"))
        return -1;
    /* The factory Master default for "report STA RSSI" is -110 dBm (report nothing,
     * what a real ZTE device ships with). The public-zero registry default would be
     * pushed to agents as RCPI 220 = 0 dBm, i.e. "report every station always",
     * silently changing the agent's metric-reporting policy. */
    if (!startup_master_table ||
        dbSetValCommByRowNo(startup_master_table, 0, "StaRssiRptThr24G", "-110") ||
        dbSetValCommByRowNo(startup_master_table, 0, "StaRssiRptThr5G", "-110"))
        return -1;
    for (row = 0; row < 8; row++) {
        snprintf(key, sizeof(key), "MULTIAPDOMAIN%u", row + 1);
        if (!dbAddRow(startup_domain_table) ||
            dbSetValCommByRowNo(startup_domain_table, row + 2, "ViewName", key) ||
            dbSetValCommByRowNo(startup_domain_table, row + 2, "DomainUsed", "0"))
            return -1;
    }
    {
        unsigned int applied = 0, rejected = 0;
        profile_apply("MultiAPMaster", startup_master_table, 0, &applied, &rejected);
        profile_apply("MultiAPDomain", startup_domain_table, 2, &applied, &rejected);   /* rows 0..7 -> MULTIAPDOMAIN1..8 */
        if (applied || rejected)
            printf("ZTE_CONTROLLER_STARTUP db_profile table=master+domain applied=%u rejected=%u\n", applied, rejected);
    }
    if (startup_policy_valid) {
        const struct mesh_startup_policy *p = &startup_policy;
        char value[16];
        int used = p->roam_set || p->steer_rssi_set || p->force_set;
        /* Row 2 is MULTIAPDOMAIN1 (domain 0): master_get_domain(0) feeds the vendor
         * roam-threshold TLV, and the domain is matched to BSSes by ESSID. */
        if (used && (dbSetValCommByRowNo(startup_domain_table, 2, "ESSID", p->essid) ||
                     dbSetValCommByRowNo(startup_domain_table, 2, "DomainUsed", "1")))
            return -1;
        if (p->roam_set) {
            snprintf(value, sizeof(value), "%d", p->roam24);
            if (dbSetValCommByRowNo(startup_domain_table, 2, "RoamRssiLmt24G", value))
                return -1;
            snprintf(value, sizeof(value), "%d", p->roam5);
            if (dbSetValCommByRowNo(startup_domain_table, 2, "RoamRssiLmt5G", value))
                return -1;
        }
        if (p->steer_rssi_set) {
            snprintf(value, sizeof(value), "%d", p->steer24);
            if (dbSetValCommByRowNo(startup_domain_table, 2, "BsRssiLmt24G", value))
                return -1;
            snprintf(value, sizeof(value), "%d", p->steer5);
            if (dbSetValCommByRowNo(startup_domain_table, 2, "BsRssiLmt5G", value))
                return -1;
        }
        if (p->steer_set &&
            dbSetValCommByRowNo(startup_master_table, 0, "EnBandSteer", p->steer_enabled ? "1" : "0"))
            return -1;
        if (p->force_set) {
            /* same fields the factory weak-signal switch changes (FEATURE_MAP section 3): all 8 domains + Master */
            unsigned int domain_row;
            for (domain_row = 2; domain_row < 10; domain_row++)
                if (dbSetValCommByRowNo(startup_domain_table, domain_row, "EnLegacyStaRoam", p->force ? "1" : "0"))
                    return -1;
            if (dbSetValCommByRowNo(startup_master_table, 0, "EnBtmKickoff", p->force ? "1" : "0") ||
                dbSetValCommByRowNo(startup_master_table, 0, "EnRoamKick", p->force ? "1" : "0"))
                return -1;
        }
    }
    return 0;
}

struct guarded_query {
    uint32_t before[4], data[MESH_DOMAIN_SIZE / 4], after[4];
};

static void init_query(struct guarded_query *q, enum mesh_policy id)
{
    memset(q, 0xa5, sizeof(*q));
    memset(q->data, 0, sizeof(q->data));
    q->data[0] = policies[id].bytes;
    q->data[1] = MAGIC;
}

static int query_guard(const struct guarded_query *q, enum mesh_policy id)
{
    unsigned int i;
    for (i = 0; i < 4; i++)
        if (q->before[i] != GUARD || q->after[i] != GUARD)
            return -1;
    /* The same backing buffer is used for both views; catch Master writes
     * beyond its declared 2312-byte boundary, not just the outer Domain guard.
     */
    for (i = policies[id].bytes / 4; i < ARRAY_SIZE(q->data); i++)
        if (q->data[i])
            return -1;
    return 0;
}

extern int ShmPoolLock(unsigned int);
extern int ShmPoolUnlock(void);

/* Live write of "field = value" pairs into all eight MULTIAPDOMAIN rows (2..9) or Master row 0, under the pool lock as
 * the other hot writers do, then a read-back of every written row through the same view API the daemon's CM getters use
 * (a field that did not stick is an error). */
static int live_write(uint32_t *table, enum mesh_policy id, unsigned int first_row, unsigned int rows,
                      const char *const *fields, const int *values, unsigned int count)
{
    unsigned int row, i;
    char text[16], key[32];
    int status = 0;
    struct guarded_query q;
    if (!table || ShmPoolLock(1000))
        return -1;
    for (row = first_row; row < first_row + rows && !status; row++)
        for (i = 0; i < count && !status; i++) {
            snprintf(text, sizeof(text), "%d", values[i]);
            status = dbSetValCommByRowNo(table, row, fields[i], text);
        }
    (void)ShmPoolUnlock();
    if (status)
        return -1;
    for (row = first_row; row < first_row + rows; row++) {
        if (id == MESH_MASTER)
            snprintf(key, sizeof(key), "MULTIAPMASTER");
        else
            snprintf(key, sizeof(key), "MULTIAPDOMAIN%u", row - 1);
        init_query(&q, id);
        if (dbAPIGetView(policies[id].name, key, q.data) || mesh_policy_validate(id, q.data, policies[id].bytes))
            return -1;
        for (i = 0; i < count; i++)
            if ((int32_t)scalar(id, q.data, fields[i]) != values[i])
                return -1;
    }
    return 0;
}

int mesh_policies_live_roam(int rssi24, int rssi5)
{
    static const char *const fields[] = { "RoamRssiLmt24G", "RoamRssiLmt5G" };
    const int values[] = { rssi24, rssi5 };
    return live_write(startup_domain_table, MESH_DOMAIN, 2, 8, fields, values, 2);
}

int mesh_policies_live_steer_rssi(int rssi24, int rssi5)
{
    static const char *const fields[] = { "BsRssiLmt24G", "BsRssiLmt5G" };
    const int values[] = { rssi24, rssi5 };
    return live_write(startup_domain_table, MESH_DOMAIN, 2, 8, fields, values, 2);
}

int mesh_policies_live_steer_enabled(int enabled)
{
    static const char *const fields[] = { "EnBandSteer" };
    const int values[] = { enabled ? 1 : 0 };
    return live_write(startup_master_table, MESH_MASTER, 0, 1, fields, values, 1);
}

int mesh_policies_live_force_domain(int on)
{
    static const char *const fields[] = { "EnLegacyStaRoam" };
    const int values[] = { on ? 1 : 0 };
    return live_write(startup_domain_table, MESH_DOMAIN, 2, 8, fields, values, 1);
}

int mesh_policies_live_force_master(int on)
{
    static const char *const fields[] = { "EnBtmKickoff", "EnRoamKick" };
    const int values[] = { on ? 1 : 0, on ? 1 : 0 };
    return live_write(startup_master_table, MESH_MASTER, 0, 1, fields, values, 2);
}

int mesh_policies_client_test(void)
{
    struct guarded_query master, a, b;
    struct { uint32_t before[4]; uint8_t data[4096]; uint32_t after[4]; } cm;
    struct { uint32_t before[4]; char names[8][32]; uint32_t after[4]; } names;
    struct cm_list list;
    uint32_t value;
    unsigned int i;
    char oversized[33];
    init_query(&master, MESH_MASTER);
    init_query(&a, MESH_DOMAIN);
    init_query(&b, MESH_DOMAIN);
    if (MapGetMasterDB(master.data) ||
        mesh_policy_validate(MESH_MASTER, master.data, MESH_MASTER_SIZE) ||
        scalar(MESH_MASTER, master.data, "EnMultiDomain") != 1 ||
        scalar(MESH_MASTER, master.data, "ApPeriodRptIntval") != 30)
        return -1;
    memset(&cm, 0xa5, sizeof(cm));
    memset(cm.data, 0, sizeof(cm.data));
    if (CmMultiAPGetMasterCfg("", cm.data))
        return -1;
    memcpy(&value, cm.data + 28, 4);
    if (value != 1)
        return -1;
    for (i = 0; i < 4; i++)
        if (cm.before[i] != GUARD || cm.after[i] != GUARD)
            return -1;
    memset(master.data + 2, 0, MESH_MASTER_SIZE - 8);
    if (mesh_policy_set_scalar(MESH_MASTER, master.data, MESH_MASTER_SIZE, "ApPeriodRptIntval", 45) ||
        MapSetMasterDB(master.data) || MapGetMasterDB(master.data) ||
        mesh_policy_validate(MESH_MASTER, master.data, MESH_MASTER_SIZE) ||
        scalar(MESH_MASTER, master.data, "ApPeriodRptIntval") != 45 ||
        scalar(MESH_MASTER, master.data, "EnRoam") != 1)
        return -1;
    if (dbAPIGetView("MultiAPDomain", MESH_DOMAIN_A, a.data) ||
        dbAPIGetView("MultiAPDomain", MESH_DOMAIN_B, b.data) ||
        mesh_policy_validate(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE) ||
        mesh_policy_validate(MESH_DOMAIN, b.data, MESH_DOMAIN_SIZE) ||
        (int32_t)scalar(MESH_DOMAIN, a.data, "RoamRssiLmt24G") != -65 ||
        (int32_t)scalar(MESH_DOMAIN, b.data, "RoamRssiLmt24G") != -78)
        return -1;
    memset(cm.data, 0, sizeof(cm.data));
    if (CmMultiAPGetDomainCfg(MESH_DOMAIN_A, cm.data) ||
        strcmp((char *)cm.data + 28, "zte-shim-domain-a"))
        return -1;
    /* Original CM query filters DomainUsed==1 and returns up to eight keys. */
    memset(&names, 0xa5, sizeof(names));
    memset(names.names, 0, sizeof(names.names));
    list.capacity = 8; list.count = 0; list.names = &names.names[0][0];
    if (CmMultiAPListDomain("", &list) || list.count != 2 ||
        !((!strcmp(names.names[0], MESH_DOMAIN_A) && !strcmp(names.names[1], MESH_DOMAIN_B)) ||
          (!strcmp(names.names[1], MESH_DOMAIN_A) && !strcmp(names.names[0], MESH_DOMAIN_B))))
        return -1;
    list.capacity = 1; list.count = 0;
    if (CmMultiAPListDomain("", &list) != -12)
        return -1;
    /* Prove the CM list is a real DomainUsed filter, not a fabricated key
     * array or an unfiltered enumeration. Restore B before later readbacks.
     */
    memset(b.data + 2, 0, MESH_DOMAIN_SIZE - 8);
    if (mesh_policy_set_scalar(MESH_DOMAIN, b.data, MESH_DOMAIN_SIZE, "DomainUsed", 0) ||
        dbAPISetView("MultiAPDomain", MESH_DOMAIN_B, b.data))
        return -1;
    list.capacity = 8; list.count = 0;
    if (CmMultiAPListDomain("", &list) || list.count != 1 || strcmp(names.names[0], MESH_DOMAIN_A) ||
        mesh_policy_set_scalar(MESH_DOMAIN, b.data, MESH_DOMAIN_SIZE, "DomainUsed", 1) ||
        dbAPISetView("MultiAPDomain", MESH_DOMAIN_B, b.data))
        return -1;
    if (dbAPIGetView("MultiAPDomain", "ZTE_SHIM_MISSING", a.data) != -5)
        return -1;
    master.data[0] = MESH_MASTER_SIZE - 4;
    if (dbAPIGetView("MultiAPMaster", "MULTIAPMASTER", master.data) != -15)
        return -1;
    master.data[0] = MESH_MASTER_SIZE;
    a.data[1] = 0;
    if (dbAPIGetView("MultiAPDomain", MESH_DOMAIN_A, a.data) != -19)
        return -1;
    a.data[1] = MAGIC;
    /* Semantic setters reject key changes, wrong capacities/types, unknown
     * fields, u8 overflow and overlong strings before touching the DB.
     */
    memset(oversized, 'x', 32); oversized[32] = 0;
    if (mesh_policy_validate(MESH_DOMAIN, NULL, MESH_DOMAIN_SIZE) != -1 ||
        mesh_policy_validate((enum mesh_policy)-1, a.data, MESH_DOMAIN_SIZE) != -1 ||
        mesh_policy_set_scalar(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "ESSID", 0) != -1 ||
        mesh_policy_set_string(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "DomainUsed", "wrong-type") != -1 ||
        mesh_policy_set_string(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "ViewName", "new-key") != -1 ||
        mesh_policy_set_scalar(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE - 4, "DomainUsed", 0) != -1 ||
        mesh_policy_set_scalar(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "unknown", 0) != -1 ||
        mesh_policy_set_scalar(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "BsActive", 256) != -1 ||
        mesh_policy_set_string(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "ESSID", oversized) != -1)
        return -1;
    memset(a.data + 2, 0, MESH_DOMAIN_SIZE - 8);
    if (mesh_policy_set_scalar(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "RoamRssiLmt24G", (uint32_t)-66) ||
        mesh_policy_set_string(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE, "ESSID", "zte-shim-domain-updated") ||
        dbAPISetView("MultiAPDomain", MESH_DOMAIN_A, a.data) ||
        dbAPIGetView("MultiAPDomain", MESH_DOMAIN_A, a.data) ||
        dbAPIGetView("MultiAPDomain", MESH_DOMAIN_B, b.data) ||
        mesh_policy_validate(MESH_DOMAIN, a.data, MESH_DOMAIN_SIZE) ||
        mesh_policy_validate(MESH_DOMAIN, b.data, MESH_DOMAIN_SIZE) ||
        (int32_t)scalar(MESH_DOMAIN, a.data, "RoamRssiLmt24G") != -66 ||
        (int32_t)scalar(MESH_DOMAIN, b.data, "RoamRssiLmt24G") != -78 ||
        scalar(MESH_DOMAIN, b.data, "DomainUsed") != 1 ||
        strcmp(string_value(MESH_DOMAIN, a.data, "ESSID"), "zte-shim-domain-updated") ||
        strcmp(string_value(MESH_DOMAIN, b.data, "ESSID"), "zte-shim-domain-b"))
        return -1;
    if (query_guard(&master, MESH_MASTER) || query_guard(&a, MESH_DOMAIN) || query_guard(&b, MESH_DOMAIN))
        return -1;
    for (i = 0; i < 4; i++)
        if (cm.before[i] != GUARD || cm.after[i] != GUARD ||
            names.before[i] != GUARD || names.after[i] != GUARD)
            return -1;
    printf("ZTE_DB_ENGINE master_get_set=1 domain_get_set=1 domain_rows=2 cmapi_policy_get=1 cmapi_domain_list=1 domain_filter=1 row_isolation=1 payload_logged=0\n");
    return 0;
}

int mesh_policies_parent_verify(void)
{
    struct guarded_query q;
    init_query(&q, MESH_MASTER);
    if (MapGetMasterDB(q.data) || mesh_policy_validate(MESH_MASTER, q.data, MESH_MASTER_SIZE) ||
        scalar(MESH_MASTER, q.data, "ApPeriodRptIntval") != 45 || query_guard(&q, MESH_MASTER))
        return -1;
    init_query(&q, MESH_DOMAIN);
    if (dbAPIGetView("MultiAPDomain", MESH_DOMAIN_A, q.data) ||
        mesh_policy_validate(MESH_DOMAIN, q.data, MESH_DOMAIN_SIZE) ||
        (int32_t)scalar(MESH_DOMAIN, q.data, "RoamRssiLmt24G") != -66 ||
        strcmp(string_value(MESH_DOMAIN, q.data, "ESSID"), "zte-shim-domain-updated") ||
        dbAPIGetView("MultiAPDomain", MESH_DOMAIN_B, q.data) ||
        mesh_policy_validate(MESH_DOMAIN, q.data, MESH_DOMAIN_SIZE) ||
        (int32_t)scalar(MESH_DOMAIN, q.data, "RoamRssiLmt24G") != -78 || query_guard(&q, MESH_DOMAIN))
        return -1;
    return 0;
}
