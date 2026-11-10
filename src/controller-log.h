/* How much of the vendor daemon's log stream reaches the device log.
 *
 * The sandbox writes the daemon's lines on stderr and procd forwards them into the
 * device's log ring, which on both lab machines is 128 KiB (`system.@system[0].log_size
 * ='128'`, logd) - around a thousand lines. Whatever the controller and each child
 * router adds competes for that ring with the user's own Wi-Fi, DHCP and LuCI lines, so
 * the streams are budgeted by class instead of behind one global switch.
 *
 * The class comes from the format text alone and the sampling window from the caller's
 * clock, so a host test (tests/controller-log-codec.c) drives every rule with its own
 * state and numbers. Passing `now` is the same reason controller-actions.c takes it: the
 * daemon ticks with CLOCK_MONOTONIC, and a duration measured against another clock reads
 * as a fault that never happened.
 */
#ifndef ZTE_CONTROLLER_LOG_H
#define ZTE_CONTROLLER_LOG_H

enum zte_log_class {
    ZTE_LOG_ORDINARY = 0, /* call/return/socket shapes: 160 per process */
    ZTE_LOG_PROTOCOL,     /* 1905 and WSC shapes: 64 per process - database noise must not hide them */
    ZTE_LOG_DEVICE,       /* child on/off announcements: 48 per process */
    ZTE_LOG_RENEW,        /* config push: 48 per process */
    ZTE_LOG_VALUE,        /* the per-event value lines the agent criterion reads: 20000 */
    ZTE_LOG_POLL,         /* the value lines a push repeats: 4 per 60 seconds */
    ZTE_LOG_FIREHOSE,     /* ZTE_TRACE_ALL, only when the lab turns it on: 20000 */
    ZTE_LOG_COUNT_CLASSES
};

#define ZTE_LOG_ORDINARY_LIMIT 160
#define ZTE_LOG_PROTOCOL_LIMIT 64
#define ZTE_LOG_DEVICE_LIMIT 48
#define ZTE_LOG_RENEW_LIMIT 48
#define ZTE_LOG_VALUE_LIMIT 20000
#define ZTE_LOG_FIREHOSE_LIMIT 20000
/* Measured 2026-11-02 on a real child router during one configuration push: 1012 poll
 * lines in seven minutes - more than the whole ring, and 94% of what it still held four
 * hours later. Four a minute keeps the transitions visible across a push and leaves that
 * push at 28 lines plus one notice per minute. */
#define ZTE_LOG_POLL_LIMIT 4
#define ZTE_LOG_POLL_WINDOW 60
/* Long enough for the sampled notice with a 32-bit count at full width. */
#define ZTE_LOG_NOTICE_SIZE 96

struct log_gate {
    /* Lines seen per class. The budgets are generous enough that a racing duplicate never
     * matters; the poll window is not on the flood path, so it is reset rather than
     * atomically claimed - one thread can win the same window twice, which costs a line. */
    unsigned int kept[ZTE_LOG_COUNT_CLASSES];
    int ordinary_announced, protocol_announced; /* already said these stopped */
    long poll_window;         /* first second of the window being sampled */
    long poll_notice_window;  /* window that already got the "sampled" notice */
    /* Poll lines dropped since this process started. Measured 2026-11-03 on the lab slave:
     * the notice proved the cap was holding, but a reader could not tell "four kept, five
     * dropped" from "four kept, a thousand dropped" - the rate alone does not say how much
     * of the daemon's story was cut. The count is cumulative, so two consecutive notices
     * differ by exactly what the earlier window lost. */
    unsigned int poll_dropped;
    char notice[ZTE_LOG_NOTICE_SIZE]; /* the last formatted notice */
};

/* Prepares a gate: budgets at zero, and the notice marker moved off the first window so
 * a service whose very first minute overflows the poll budget says so. A test opens one
 * gate per rule, so a budget spent by one cannot change what the next sees. */
void controller_log_open(struct log_gate *gate);

/* Which budget a line belongs to, decided from the marker the preload printed it with.
 * A format that matches nothing is ordinary: the preload's own call/return lines are the
 * majority and the least urgent. */
int controller_log_class(const char *format);

/* Whether one of the daemon's own format texts may be expanded into a value line, and
 * which marker it becomes: ZTE_LOG_VALUE for the per-event ones the agent criterion
 * reads, ZTE_LOG_POLL for the ones a push repeats hundreds of times, -1 for anything
 * else - including every text that carries key material or an SSID, because a value line
 * prints the daemon's formatted result and not its format. */
int controller_log_shape(const char *format);

/* Decides one line at monotonic second `now`. Returns non-zero when the line may be
 * written. When a budget has just run out, or a sampling window has just begun dropping,
 * `*notice` points at a sentence for the caller to write instead - a reader must be able
 * to tell "nothing happened" from "the log stopped telling". The text is fixed except for
 * the count in the sampled notice, so it never carries anything from the line that
 * triggered it; the pointer is into the gate and lives until the next call on that gate,
 * which is why callers write it immediately. */
int controller_log_admit(struct log_gate *gate, long now, const char *format, const char **notice);

#endif
