/* The rules in controller-log.h, with no stderr and no clock of its own.
 *
 * Two shapes of limit live here. The per-process budgets (ordinary, protocol, device,
 * renew) are what the preload has always had: a sandbox that restarts gets a fresh
 * budget, and a crash loop therefore costs one short burst per restart. The poll stream
 * is different - it is the daemon's own state machine repeating itself every few seconds
 * while a configuration push is in flight, and one push measured 1012 of those lines.
 * That is sampled against the clock instead of counted once per process, so a long run
 * keeps seeing the transitions and no single window can outwrite the device's log ring.
 *
 * Nothing here formats a value from the daemon: the notices are our own sentences and the
 * decision reads only the format text, which is what keeps SSIDs and keys out of the log.
 */
#include <stdio.h>
#include <string.h>
#include "controller-log.h"

static const char notice_ordinary[] = "ZTE_CONTROLLER_TRACE log_budget=ordinary exhausted=1 limit=160\n";
static const char notice_protocol[] = "ZTE_CONTROLLER_TRACE log_budget=packet exhausted=1 limit=64\n";
/* The rate is fixed text; only the count is formatted, so a reader can tell how much the
 * sampler cut. Nothing here is ever copied from the line that triggered it. */
static const char notice_poll_head[] =
    "ZTE_CONTROLLER_TRACE log_budget=poll sampled=1 limit=4 window=60s dropped_total=";

/* The markers the preload prints its own lines with. Classifying by the marker rather
 * than by the daemon's wording keeps the decision in one place: the value lines the
 * agent criterion reads and the polling it does not are separated where they are printed. */
static const char *const protocol_shapes[] = {
    " rx1905 ", " tx1905 ", " packet_bind ", " native_dh_keygen ",
    " packet_decision ", " oss_dispatch ", " wsc_status ", " bss_config ",
};

/* The daemon's own format texts, split by how often a push repeats them. These are the
 * four that fire a handful of times: whether the M2 landed, which BSS it went to, how
 * long it was, and that the sync finished. */
static const char *const value_shapes[] = {
    "acdcParseM2Msg cfgChg", "The WSC-%d will cfg", "M2 total Length", "mesh sync for M2",
};
/* And the two the acdc state machine repeats every few seconds until the push settles.
 * Both carry status codes only. */
static const char *const poll_shapes[] = {
    "current state", "WLAN_ASSERT_RET",
};
/* Never expanded: a value line prints the daemon's formatted result, and these are the
 * texts whose arguments are keys, SSIDs or BSS identities. */
static const char *const secret_shapes[] = {
    "PWKey", "WEPKey", "WEP", "Wep", "BeaconType", "PSK", "psk", "key", "Key",
    "SSID", "ssid", "Bss", "bss", "WLANCFG",
};

#define SHAPES(list) (sizeof(list) / sizeof((list)[0]))

static int contains_any(const char *format, const char *const *needles, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; i++)
        if (strstr(format, needles[i]))
            return 1;
    return 0;
}

int controller_log_class(const char *format)
{
    if (!format)
        return ZTE_LOG_ORDINARY;
    if (strstr(format, "ZTE_TRACE_ALL"))
        return ZTE_LOG_FIREHOSE;
    if (strstr(format, "ZTE_TRACE_VALUE"))
        return ZTE_LOG_VALUE;
    if (strstr(format, "ZTE_TRACE_POLL"))
        return ZTE_LOG_POLL;
    if (strstr(format, " device_event "))
        return ZTE_LOG_DEVICE;
    /* "renewACD" and friends: config-push lines keep their own budget so database noise
     * cannot spend it first. The match is the loose substring it always was. */
    if (strstr(format, "enew"))
        return ZTE_LOG_RENEW;
    if (contains_any(format, protocol_shapes, SHAPES(protocol_shapes)))
        return ZTE_LOG_PROTOCOL;
    return ZTE_LOG_ORDINARY;
}

int controller_log_shape(const char *format)
{
    if (!format)
        return -1;
    if (contains_any(format, secret_shapes, SHAPES(secret_shapes)))
        return -1;
    if (contains_any(format, value_shapes, SHAPES(value_shapes)))
        return ZTE_LOG_VALUE;
    if (contains_any(format, poll_shapes, SHAPES(poll_shapes)))
        return ZTE_LOG_POLL;
    return -1;
}

void controller_log_open(struct log_gate *gate)
{
    memset(gate, 0, sizeof(*gate));
    /* A zero-initialised gate is already usable; this only moves the notice marker off
     * the first window, so a service whose very first minute overflows the poll budget
     * says so instead of dropping silently. */
    gate->poll_notice_window = -1;
}

static unsigned int class_limit(int class)
{
    switch (class) {
    case ZTE_LOG_ORDINARY:   return ZTE_LOG_ORDINARY_LIMIT;
    case ZTE_LOG_PROTOCOL:   return ZTE_LOG_PROTOCOL_LIMIT;
    case ZTE_LOG_DEVICE:     return ZTE_LOG_DEVICE_LIMIT;
    case ZTE_LOG_RENEW:      return ZTE_LOG_RENEW_LIMIT;
    case ZTE_LOG_VALUE:      return ZTE_LOG_VALUE_LIMIT;
    case ZTE_LOG_FIREHOSE:   return ZTE_LOG_FIREHOSE_LIMIT;
    default:                 return ZTE_LOG_POLL_LIMIT;
    }
}

int controller_log_admit(struct log_gate *gate, long now, const char *format, const char **notice)
{
    int class = controller_log_class(format);
    unsigned int count;
    long window;

    if (notice)
        *notice = NULL;
    if (!gate)
        return 0;

    if (class == ZTE_LOG_POLL) {
        /* Floor division: seconds -10..49 share window -1, so a timestamp below the
         * clock's origin still gets one sampled window rather than a permanent notice. */
        window = now / ZTE_LOG_POLL_WINDOW;
        if (now < 0 && now % ZTE_LOG_POLL_WINDOW != 0)
            window--;
        /* A clock that steps back is treated as the same window: it must never re-award
         * a whole budget per line. */
        if (window > gate->poll_window) {
            gate->poll_window = window;
            gate->kept[class] = 0;
        }
        if (gate->kept[class] < ZTE_LOG_POLL_LIMIT) {
            gate->kept[class]++;
            return 1;
        }
        /* Every line this window loses, counted - including the ones in a window that
         * already got its notice, so the number between two notices is what the earlier
         * window dropped. */
        gate->poll_dropped++;
        if (notice && gate->poll_notice_window != window) {
            gate->poll_notice_window = window;
            snprintf(gate->notice, sizeof(gate->notice), "%s%u\n",
                     notice_poll_head, gate->poll_dropped);
            *notice = gate->notice;
        }
        return 0;
    }

    count = __sync_fetch_and_add(&gate->kept[class], 1);
    if (count < class_limit(class))
        return 1;
    /* Announce a stopped stream exactly once, and only for the two budgets a reader
     * would otherwise mistake for a quiet device. */
    if (!notice)
        return 0;
    if (class == ZTE_LOG_ORDINARY && !gate->ordinary_announced++)
        *notice = notice_ordinary;
    else if (class == ZTE_LOG_PROTOCOL && !gate->protocol_announced++)
        *notice = notice_protocol;
    return 0;
}
