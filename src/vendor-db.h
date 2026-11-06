#ifndef ZTE_VENDOR_DB_H
#define ZTE_VENDOR_DB_H
#include <stddef.h>
#include <stdint.h>

/* Hash-pinned ARM32 libdb domain descriptor; not an external RPC ABI. */
struct zte_db_domain {
    const char *name;
    uint32_t id, type;
    uint8_t primary_key, save, cp, change;
    int32_t minimum;
    uint32_t maximum;
    const char *default_value;
    uint32_t offset, cell_size, value_size, reserved[5];
    void *next, *previous;
};
_Static_assert(sizeof(void *) == 4, "factory DB requires ARM32 pointers");
_Static_assert(sizeof(struct zte_db_domain) == 68, "domain ABI");
_Static_assert(offsetof(struct zte_db_domain, default_value) == 24, "default ABI");
_Static_assert(offsetof(struct zte_db_domain, next) == 60, "domain list ABI");

extern void *dbCreateTbl(const void *);
extern int dbCreateDomainNew(void *, const void *, unsigned int);
extern void *dbAddRow(void *);
extern int dbMgrCommViewAddItem(const char *, const char *, const char *, unsigned int,
                              unsigned int, const void *, unsigned int,
                              const char *, const char *, const char *);
extern int dbSetValCommByRowNo(void *, unsigned int, const char *, const char *);
extern unsigned int dbGetPrmValLen(unsigned int);
extern unsigned int dbGetPmTypeValLen(unsigned int);
#endif
