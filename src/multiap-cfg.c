/* Original common-view engine + recovered descriptors, not fake DB results. */
#include "multiap-cfg.h"
#include "vendor-db.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(void *) == 4, "factory DB requires ARM32 pointers");
_Static_assert(MAP_FIELD_COUNT == 50, "baseline MultiAPCfg has 50 fields");

/* Strings/offsets are copied and initialized by the real DB engine. */
static const struct zte_db_domain domains[] = {
#define MAP_FIELD(id, label, dtype, saved, value) \
    { .name = label, .type = dtype, .save = saved, .default_value = value },
#include "multiap_cfg.def"
#undef MAP_FIELD
    { .name = "", .default_value = "0" }
};

/* Test-only PUBLIC fixture. Mode=2 is the baseline CM API's "Master" enum.
 * This is not a recovered customer profile or an operational Controller.
 * VersionMode is deliberately left at the registry's 0: cspd's product-
 * dependent dbSetDefVersionMode is not executed or replaced with a guess.
 */
static const struct { const char *name, *value; } test_values[] = {
    {"Mode", "2"}, {"Enable", "1"}, {"SyncEnable", "1"},
    {"SetRoleEnable", "1"}, {"EnMasterOnly", "1"},
    /* The agent module's slave_init (skipped while EnMasterOnly=1, as on this radio-less
     * gateway) is what gives a Master CurHop = MaxHop; the daemon advertises CurHop to children
     * in its higher-layer vendor TLV, and the DB default of -2 means "not attached". */
    {"CurHop", "2"},
    {"RetryTimes", "4"}, {"MaxCfgingBandNum", "5"},
    {"MaxCfgingWaitTime", "10"}, {"OffStaCleanInterval", "3600"},
    {"StaSyncInterval", "0"}, {"EnP1905DevSave", "1"},
    {"MeshName", "zte-shim-lab"}, {"IFLink", "br-lan"}
};

size_t map_cfg_offset(enum map_field field)
{
    size_t offset = 8;
    unsigned int i;
    if ((unsigned int)field > MAP_FIELD_COUNT)
        return 0;
    for (i = 0; i < (unsigned int)field; i++) {
        unsigned int length = dbGetPmTypeValLen(domains[i].type);
        if (!length || length > MAP_CFG_SIZE - offset)
            return 0;
        offset += length;
    }
    return offset;
}

int map_cfg_create_test_table(void)
{
    uint32_t descriptor[41] = {0};
    uint32_t *table;
    unsigned int i;
    /* dbCreateMultiAPConfigTbl @0x1f16c0: one row, byte +12 == 1. */
    descriptor[0] = (uint32_t)(uintptr_t)"MultiAPCfg";
    descriptor[2] = 1;
    descriptor[3] = 1;
    if (map_cfg_offset(MAP_FIELD_COUNT) != MAP_CFG_SIZE)
        return -1;
    table = dbCreateTbl(descriptor);
    if (!table || dbCreateDomainNew(table, domains, sizeof(domains)) != 0 ||
        table[18] != MAP_FIELD_COUNT || !dbAddRow(table) || table[27] != 1)
        return -1;
    /* Exact _dbInitTbl @0x1f1c68 arguments: generic GET|SET (0x0c),
     * no callbacks, 2952 bytes, empty instance/parameter selector strings.
     */
    if (dbMgrCommViewAddItem("MultiAPCfg", "MultiAPCfg", "", MAP_CFG_SIZE,
                            0, NULL, 12, "MultiAPCfg", "", "") != 0)
        return -1;
    for (i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++)
        if (dbSetValCommByRowNo(table, 0, test_values[i].name, test_values[i].value) != 0)
            return -1;
    return 0;
}

static uint16_t u16(const void *p)
{
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

int map_cfg_validate(const void *query)
{
    const uint8_t *p = query;
    uint32_t header[2], status;
    unsigned int i;
    memcpy(header, p, sizeof(header));
    if (header[0] != MAP_CFG_SIZE || header[1] != MAP_CFG_MAGIC ||
        map_cfg_offset(MAP_FIELD_COUNT) != MAP_CFG_SIZE)
        return -1;
    for (i = 0; i < MAP_FIELD_COUNT; i++) {
        size_t offset = map_cfg_offset(i);
        unsigned int type = domains[i].type;
        unsigned int length = dbGetPrmValLen(type);
        memcpy(&status, p + offset + 24, sizeof(status));
        if (u16(p + offset) != type || status || !length)
            return -1;
        if ((type == 3 || type == 5 || type == 7) &&
            memchr(p + offset + 28, 0, length) == NULL)
            return -1;
    }
    return 0;
}

uint32_t map_cfg_scalar(const void *query, enum map_field field)
{
    uint32_t value = 0;
    size_t offset = map_cfg_offset(field);
    if ((unsigned int)field >= MAP_FIELD_COUNT || !offset ||
        domains[field].type < 12 || domains[field].type > 15)
        return 0;
    memcpy(&value, (const uint8_t *)query + offset + 28,
           domains[field].type == 15 ? 1 : 4);
    return value;
}

const char *map_cfg_string(const void *query, enum map_field field)
{
    size_t offset = map_cfg_offset(field);
    if ((unsigned int)field >= MAP_FIELD_COUNT || !offset ||
        (domains[field].type != 3 && domains[field].type != 5 && domains[field].type != 7))
        return NULL;
    return (const char *)query + offset + 28;
}

int map_cfg_set_scalar(void *query, enum map_field field, uint32_t value)
{
    uint16_t mask = 1;
    uint8_t *p = query;
    size_t offset = map_cfg_offset(field);
    unsigned int type;
    if ((unsigned int)field >= MAP_FIELD_COUNT || !offset)
        return -1;
    type = domains[field].type;
    if (type < 12 || type > 15 || (type == 15 && value > 255))
        return -1;
    memcpy(p + offset + 28, &value, type == 15 ? 1 : 4);
    memcpy(p + offset + 2, &mask, sizeof(mask));
    return 0;
}

int map_cfg_set_string(void *query, enum map_field field, const char *value)
{
    uint16_t mask = 1;
    uint8_t *p = query;
    size_t offset = map_cfg_offset(field), length;
    unsigned int type, capacity;
    if ((unsigned int)field >= MAP_FIELD_COUNT || !offset || !value)
        return -1;
    type = domains[field].type;
    if (type != 3 && type != 5 && type != 7)
        return -1;
    capacity = dbGetPrmValLen(type);
    length = strlen(value);
    /* Original types reserve a pointer-sized slot in their value length.
     * Limit the public test setter to the actual fixed string capacity.
     */
    if (capacity < 4 || length >= capacity - 4)
        return -1;
    memset(p + offset + 28, 0, capacity);
    memcpy(p + offset + 28, value, length);
    memcpy(p + offset + 2, &mask, sizeof(mask));
    return 0;
}
