/* The child-side configuration-manager stand-in (libzte_agent_mm.so).

 * A Multi-AP agent applies the M2 it receives through libcmapi's WLAN setters. Each setter writes the
 * configuration database first and only then pushes the change to the configuration manager (cspd), so on a
 * box without cspd the row is already correct while the return code is -3 - and
 * acdcConfigM2EncryptedSettings() (0x75b7c) treats anything other than 0 or -9 as fatal, which drops the rest
 * of the apply: the PSK write (acdcWlanSetPWKey) and the repeater sync never run. docs/research/AGENT_PROBE.md
 * sections 8 and 9 record the disassembly and the runtime evidence.
 *
 * This library reports success for exactly that case and nothing else. Before it substitutes the status it
 * re-reads the row the setter just wrote and requires the database to hold the value the caller asked for;
 * an unreadable row, a different value, or any other failure code is propagated untouched. So the claim it
 * makes is "the row is what you wanted, only the push to a manager that is not there failed" - not "assume
 * it worked".
 *
 * Only the two setters the apply path ends in are interposed. CmSendMsg2MM is deliberately NOT: it has 324
 * call sites in libcmapi, and a blanket success would hand every reader an unfilled buffer.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

/* What libcmapi returns when the configuration manager does not answer its socket. Measured in the lab
 * (ZTE_TRACE_VALUE text=CmWLANSetAPConfig instid=DEV.WIFI.AP1 status=-3 errno=9) and only ever honoured
 * together with the read-back below. */
#define MM_NO_CONFIGURATION_MANAGER (-3)

/* The two structures and the rows they came from. CmWLANGetAPConfig output: 2496 bytes with the SSID at
 * offset 444. WLANCfg database row: ESSID at 708 (cm-profile-layout.inc maps 708 -> 444, and
 * controller-profile.c reads BeaconType at 900 of the same buffer). PSK: 324-byte CM structure with the key
 * at 188, 328-byte WLANPSK row with PreSharedKey at 260. */
#define CM_AP_BYTES 2496U
#define CM_AP_ESSID 444U
#define DB_CFG_BYTES 2312U
#define DB_CFG_ESSID 708U
#define CM_PSK_BYTES 324U
#define CM_PSK_KEY 188U
#define DB_PSK_BYTES 328U
#define DB_PSK_KEY 260U
#define SSID_CELL 36U
#define KEY_CELL 64U
#define DB_MAGIC 0x01020304U

#define AGENT_MM_LOG_LIMIT 96U

static void log_line(const char *format, ...)
{
    static unsigned int budget;
    char text[192];
    va_list ap;
    int n, saved = errno;
    if (__sync_fetch_and_add(&budget, 1) >= AGENT_MM_LOG_LIMIT)
        return;
    va_start(ap, format);
    n = vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (n > 0) {
        /* The result is consumed rather than cast away: write() is warn_unused_result, and this
         * library is built with -Werror in CI. A short write only costs a truncated trace line. */
        ssize_t written = write(STDERR_FILENO, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
        if (written < 0)
            (void)written;
    }
    errno = saved;
}

/* The lab keeps a way to see the vendor status verbatim; the runtime has no environment to switch it with
 * (the sandbox hands the daemon a single variable), so the switch is a file in the one writable directory. */
static int stand_in_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = access("/run/zte-mesh/mm-off", F_OK) != 0;
    return cached;
}

static void *next_symbol(const char *name)
{
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol)
        log_line("ZTE_AGENT_MM missing_symbol=%s\n", name);
    return symbol;
}

/* Read one view row the way the factory finder does: a size word and a magic word in front of the payload,
 * both of which dbAPIGetView() preserves on success. */
static int row_read(const char *view, const char *instance, void *buffer, uint32_t bytes)
{
    typedef int (*get_view_fn)(const char *, const char *, void *);
    get_view_fn get = (get_view_fn)next_symbol("dbAPIGetView");
    uint32_t magic = DB_MAGIC;
    int status;
    if (!get || !instance)
        return -1;
    memset(buffer, 0, bytes);
    memcpy(buffer, &bytes, sizeof(bytes));
    memcpy((uint8_t *)buffer + sizeof(bytes), &magic, sizeof(magic));
    status = get(view, instance, buffer);
    if (status || memcmp(buffer, &bytes, sizeof(bytes)) ||
        memcmp((uint8_t *)buffer + sizeof(bytes), &magic, sizeof(magic)))
        return -1;
    return 0;
}

/* Does the WLANCfg row already carry the SSID the caller wanted? A stale row differs here, which is what
 * makes the substitution below an observation instead of an assumption. */
static int cfg_applied(const char *instance, const void *config)
{
    const char *wanted = (const char *)config + CM_AP_ESSID;
    char row[DB_CFG_BYTES];
    size_t n = strnlen(wanted, SSID_CELL);
    int status;
    if (n >= SSID_CELL)
        return 0;
    status = row_read("WLANCfg", instance, row, sizeof(row));
    if (status)
        return 0;
    return !memcmp(row + DB_CFG_ESSID, wanted, n) && !row[DB_CFG_ESSID + n];
}

static int psk_applied(const char *instance, const void *config)
{
    const char *wanted = (const char *)config + CM_PSK_KEY;
    char row[DB_PSK_BYTES];
    size_t n = strnlen(wanted, KEY_CELL);
    int status;
    if (n >= KEY_CELL)
        return 0;
    status = row_read("WLANPSK", instance, row, sizeof(row));
    if (status)
        return 0;
    return !memcmp(row + DB_PSK_KEY, wanted, n) && !row[DB_PSK_KEY + n];
}

/* The two WLAN setters acdcConfigM2EncryptedSettings() ends in (through MeshWlanSetAPCfgByBandAndIfIndex and
 * acdcWlanSetPWKey). Instance ids are administrative names, never credentials; the payload is not logged. */
EXPORT int CmWLANSetAPConfig(const char *instance, const void *config)
{
    int (*real)(const char *, const void *) = (int (*)(const char *, const void *))next_symbol("CmWLANSetAPConfig");
    int result, saved, applied = 0, report;
    if (!real)
        return -1;
    errno = 0;
    result = real(instance, config);
    saved = errno;
    if (result == MM_NO_CONFIGURATION_MANAGER && config && stand_in_enabled())
        applied = cfg_applied(instance, config);
    report = applied ? 0 : result;
    if (result)
        log_line("ZTE_AGENT_MM setter=CmWLANSetAPConfig instid=%.40s status=%d row_applied=%d report=%d errno=%d\n",
                 instance ? instance : "-", result, applied, report, saved);
    errno = saved;
    return report;
}

EXPORT int CmWLANSetPsk(const char *instance, const void *config)
{
    int (*real)(const char *, const void *) = (int (*)(const char *, const void *))next_symbol("CmWLANSetPsk");
    int result, saved, applied = 0, report;
    if (!real)
        return -1;
    errno = 0;
    result = real(instance, config);
    saved = errno;
    if (result == MM_NO_CONFIGURATION_MANAGER && config && stand_in_enabled())
        applied = psk_applied(instance, config);
    report = applied ? 0 : result;
    if (result)
        log_line("ZTE_AGENT_MM setter=CmWLANSetPsk instid=%.40s status=%d row_applied=%d report=%d errno=%d\n",
                 instance ? instance : "-", result, applied, report, saved);
    errno = saved;
    return report;
}

/* Unused-but-documented sizes: they are what the read-back assumes about the two structures, and a compiler
 * that sees them here is a compiler that will complain if the offsets above stop making sense. */
static int layout_bounds(void)
{
    return CM_AP_BYTES > CM_AP_ESSID + SSID_CELL && CM_PSK_BYTES > CM_PSK_KEY + KEY_CELL;
}
EXPORT int zte_agent_mm_layout(void)
{
    return layout_bounds();
}
