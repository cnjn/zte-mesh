#ifndef ZTE_MULTIAP_CFG_H
#define ZTE_MULTIAP_CFG_H
#include <stddef.h>
#include <stdint.h>

#define MAP_CFG_SIZE 2952U
#define MAP_CFG_MAGIC 0x01020304U

enum map_field {
#define MAP_FIELD(id, name, type, save, value) MAP_##id,
#include "multiap_cfg.def"
#undef MAP_FIELD
    MAP_FIELD_COUNT
};

/* These calls only register/read/write the private DB. No cspd, OSS task,
 * WLAN callback, file persistence or network configuration is invoked.
 */
int map_cfg_create_test_table(void);
size_t map_cfg_offset(enum map_field field);
int map_cfg_validate(const void *query);
uint32_t map_cfg_scalar(const void *query, enum map_field field);
const char *map_cfg_string(const void *query, enum map_field field);
int map_cfg_set_scalar(void *query, enum map_field field, uint32_t value);
int map_cfg_set_string(void *query, enum map_field field, const char *value);
#endif
