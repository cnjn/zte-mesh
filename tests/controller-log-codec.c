/* Host tests for how much of the vendor daemon's log reaches the device log.
 *
 * The claim is A22's log half: a long run must not push the user's own lines out of the
 * device's log ring, and a reader must be able to tell "nothing happened" from "the log
 * stopped telling". Both are functions of the format text and a clock the caller passes,
 * so this runs without a daemon - the numbers come from what one real child router was
 * measured doing during one configuration push (1012 repeating lines in 7 minutes, on a
 * device whose ring holds about a thousand lines).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/controller-log.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "log gate failure line %d\n", __LINE__); exit(1); } } while (0)

static const char *ordinary_line = "ZTE_CONTROLLER_TRACE call=OSSStart\n";
static const char *protocol_line = "ZTE_CONTROLLER_TRACE rx1905 cmdu=9 bytes=300 ifindex=4 protocol=0\n";
static const char *device_line = " base: device_event dev=1 online=1\n";
static const char *renew_line = " cmapi: renewACD band=5g\n";
static const char *value_line = "ZTE_TRACE_VALUE text=acdcParseM2Msg cfgChg=1\n";
static const char *poll_line = "ZTE_TRACE_POLL text=acdc current state[0]:3,event:10\n";
static const char *firehose_line = "ZTE_TRACE_ALL vendor_format=whatever\n";

/* Counts what a stream of one class does over `seconds` starting at `base`. */
struct tally {
    unsigned int kept, notices, dropped;
};

static void feed(struct log_gate *gate, const char *line, unsigned int count, long base, long step,
                 struct tally *out)
{
    unsigned int i;
    const char *notice;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < count; i++) {
        notice = NULL;
        if (controller_log_admit(gate, base + (long)i * step, line, &notice))
            out->kept++;
        else {
            out->dropped++;
            if (notice)
                out->notices++;
        }
    }
}

static void test_class_and_shape(void)
{
    CHECK(controller_log_class(ordinary_line) == ZTE_LOG_ORDINARY);
    CHECK(controller_log_class(protocol_line) == ZTE_LOG_PROTOCOL);
    CHECK(controller_log_class(device_line) == ZTE_LOG_DEVICE);
    CHECK(controller_log_class(renew_line) == ZTE_LOG_RENEW);
    CHECK(controller_log_class(value_line) == ZTE_LOG_VALUE);
    CHECK(controller_log_class(poll_line) == ZTE_LOG_POLL);
    CHECK(controller_log_class(firehose_line) == ZTE_LOG_FIREHOSE);
    CHECK(controller_log_class("") == ZTE_LOG_ORDINARY);
    CHECK(controller_log_class(NULL) == ZTE_LOG_ORDINARY);

    /* The four texts the agent criterion reads are per-event and must never be sampled:
     * they are the evidence that the M2 landed, and a sampled stream hides it. */
    CHECK(controller_log_shape("acdcParseM2Msg cfgChg=%d") == ZTE_LOG_VALUE);
    CHECK(controller_log_shape("The WSC-%d will cfg idx=%d") == ZTE_LOG_VALUE);
    CHECK(controller_log_shape("M2 total Length=%d") == ZTE_LOG_VALUE);
    CHECK(controller_log_shape("mesh sync for M2 ok=%d") == ZTE_LOG_VALUE);
    /* The two the state machine repeats until the push settles. */
    CHECK(controller_log_shape("acdc current state[%d]:%d,event:%d") == ZTE_LOG_POLL);
    CHECK(controller_log_shape("WLAN_ASSERT_RET_CMAPI_OK(iRet, return iRet) failed! iRet=%d") == ZTE_LOG_POLL);
    CHECK(controller_log_shape("nothing of interest here") == -1);

    /* A text carrying a key or an SSID is never expanded, even when it also matches a
     * shape: a value line prints the daemon's formatted result, not its format. */
    CHECK(controller_log_shape("Bss current state[%d]") == -1);
    CHECK(controller_log_shape("SSID PWKey current state") == -1);
    CHECK(controller_log_shape("WLANCFG WLAN_ASSERT_RET") == -1);
    CHECK(controller_log_shape(NULL) == -1);
}

static void test_per_process_budgets(void)
{
    struct log_gate gate;
    struct tally t;

    controller_log_open(&gate);
    feed(&gate, ordinary_line, ZTE_LOG_ORDINARY_LIMIT + 40, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_ORDINARY_LIMIT);
    CHECK(t.notices == 1);
    CHECK(t.dropped == 40);

    controller_log_open(&gate);
    feed(&gate, protocol_line, ZTE_LOG_PROTOCOL_LIMIT + 40, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_PROTOCOL_LIMIT);
    CHECK(t.notices == 1);

    /* A budget spent by one class does not touch another: the database flood is the whole
     * reason the config-push and device lines have budgets of their own. */
    controller_log_open(&gate);
    feed(&gate, ordinary_line, 4000, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_ORDINARY_LIMIT);
    feed(&gate, renew_line, ZTE_LOG_RENEW_LIMIT + 40, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_RENEW_LIMIT);
    feed(&gate, device_line, ZTE_LOG_DEVICE_LIMIT + 40, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_DEVICE_LIMIT);
    feed(&gate, value_line, 5000, 1000, 0, &t);
    CHECK(t.kept == 5000 && t.notices == 0);
    feed(&gate, firehose_line, ZTE_LOG_FIREHOSE_LIMIT + 1, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_FIREHOSE_LIMIT);

    /* One gate per case, so a budget cannot leak between rules. */
    controller_log_open(&gate);
    feed(&gate, ordinary_line, ZTE_LOG_ORDINARY_LIMIT, 1000, 0, &t);
    CHECK(t.kept == ZTE_LOG_ORDINARY_LIMIT && t.notices == 0);
}

static void test_notices_say_what_stopped(void)
{
    struct log_gate gate;
    const char *notice;
    unsigned int i;

    /* A reader who finds no more lines must be able to tell the limit from the silence. */
    controller_log_open(&gate);
    for (i = 0; i < ZTE_LOG_ORDINARY_LIMIT + 1; i++)
        controller_log_admit(&gate, 10, ordinary_line, &notice);
    CHECK(notice != NULL);
    CHECK(strstr(notice, "log_budget=ordinary") && strstr(notice, "limit=160") && strstr(notice, "exhausted=1"));
    CHECK(notice[strlen(notice) - 1] == '\n');
    /* Written straight to stderr, never back through the gate. If that ever changes, the
     * notice must not be a class the flood it reports has already spent. */
    CHECK(controller_log_class(notice) == ZTE_LOG_ORDINARY);

    controller_log_open(&gate);
    for (i = 0; i < ZTE_LOG_PROTOCOL_LIMIT + 1; i++)
        controller_log_admit(&gate, 10, protocol_line, &notice);
    CHECK(strstr(notice, "log_budget=packet") && strstr(notice, "limit=64"));

    /* The poll notice states the rate, so sparse lines are not read as a slow daemon. */
    controller_log_open(&gate);
    notice = NULL;
    for (i = 0; i < ZTE_LOG_POLL_LIMIT + 1; i++)
        controller_log_admit(&gate, 10, poll_line, &notice);
    CHECK(strstr(notice, "log_budget=poll") && strstr(notice, "limit=4") && strstr(notice, "window=60"));
    CHECK(controller_log_class(notice) == ZTE_LOG_ORDINARY);
}

/* The rule this file exists for: one push, measured on a real child router. */
static void test_one_push_leaves_the_ring_alone(void)
{
    struct log_gate gate;
    struct tally t;
    unsigned int minute;
    long base = 6000;

    /* Seven minutes at the measured rate (about 145 lines a minute), one gate, one clock
     * advancing a minute at a time. */
    controller_log_open(&gate);
    t.kept = t.notices = t.dropped = 0;
    for (minute = 0; minute < 7; minute++) {
        struct tally one;
        feed(&gate, poll_line, 145, base + (long)minute * 60, 0, &one);
        t.kept += one.kept;
        t.notices += one.notices;
        t.dropped += one.dropped;
    }
    CHECK(t.kept == 7 * ZTE_LOG_POLL_LIMIT);
    CHECK(t.notices == 7);
    CHECK(t.dropped == 7 * 145 - t.kept);
    /* The header promises "about thirty lines" for a push: 28 lines plus 7 notices. */
    CHECK(t.kept + t.notices < 40);
    /* Against the old behaviour: the same push wrote every one of the 1012 lines. */
    CHECK(t.kept * 25 < 1012);

    /* An under-full minute is silent and complete. */
    controller_log_open(&gate);
    feed(&gate, poll_line, ZTE_LOG_POLL_LIMIT, base, 0, &t);
    CHECK(t.kept == ZTE_LOG_POLL_LIMIT && t.notices == 0 && t.dropped == 0);

    /* A clock that steps back must not re-award a budget on every line. */
    controller_log_open(&gate);
    feed(&gate, poll_line, 60, 9000, -1, &t);
    CHECK(t.kept == ZTE_LOG_POLL_LIMIT);

    /* Long runs keep seeing the stream: the budget is per window, not per process. */
    controller_log_open(&gate);
    for (minute = 0; minute < 60; minute++) {
        feed(&gate, poll_line, 145, 20000 + (long)minute * 60, 0, &t);
        CHECK(t.kept == ZTE_LOG_POLL_LIMIT);
    }
}

static void test_lines_that_are_not_poll_are_never_sampled(void)
{
    struct log_gate gate;
    struct tally t;

    /* The measured flood is poll lines only; cfgChg and the setter answers ride past a
     * busy minute untouched. */
    controller_log_open(&gate);
    feed(&gate, value_line, 4 * ZTE_LOG_POLL_WINDOW, 30000, 1, &t);
    CHECK(t.kept == 4 * ZTE_LOG_POLL_WINDOW && t.notices == 0);

    controller_log_open(&gate);
    feed(&gate, ordinary_line, 20, 30000, 0, &t);
    CHECK(t.kept == 20);
    feed(&gate, poll_line, 100, 30000, 0, &t);
    CHECK(t.kept == ZTE_LOG_POLL_LIMIT && t.notices == 1);
    /* ... and a later minute still gets its own four. */
    feed(&gate, poll_line, 100, 30120, 0, &t);
    CHECK(t.kept == ZTE_LOG_POLL_LIMIT);
}

static unsigned int notice_dropped(const char *notice)
{
    static const char key[] = "dropped_total=";
    const char *at = strstr(notice, key);
    CHECK(at != NULL);
    return (unsigned int)strtoul(at + (sizeof(key) - 1), NULL, 10);
}

/* Measured 2026-11-03 on the lab slave: the sampled notice proved the four-a-minute cap was
 * holding, but a reader could not tell "four kept, five dropped" from "four kept, a thousand
 * dropped" - and that difference is the whole point of the cap. The count is cumulative, so
 * two consecutive notices differ by exactly what the earlier window lost. */
static void test_the_notice_reports_what_it_dropped(void)
{
    struct log_gate gate;
    const char *notice;
    unsigned int minute, line, notices = 0, previous = 0;
    long base = 6000;
    /* One push at the measured rate: 145 lines a window, four of them kept. */
    const unsigned int per_window = 145, lost = per_window - ZTE_LOG_POLL_LIMIT;

    controller_log_open(&gate);
    for (minute = 0; minute < 7; minute++) {
        for (line = 0; line < per_window; line++) {
            unsigned int value;
            notice = NULL;
            controller_log_admit(&gate, base + (long)minute * ZTE_LOG_POLL_WINDOW, poll_line, &notice);
            if (!notice)
                continue;
            /* One notice per over-full window, and it counts from the first dropped line. */
            value = notice_dropped(notice);
            CHECK(value == (minute == 0 ? 1u : previous + lost));
            previous = value;
            notices++;
        }
    }
    CHECK(notices == 7);
    /* The last window was still dropping when its notice went out, so the number a reader
     * sees last is a lower bound for the push, not the total. */
    CHECK(previous == 1 + 6 * lost);
    CHECK(previous < 7 * per_window - 7 * ZTE_LOG_POLL_LIMIT);
    /* A window that stays under the cap says nothing - no notice, no count. */
    controller_log_open(&gate);
    for (line = 0; line < ZTE_LOG_POLL_LIMIT; line++) {
        notice = NULL;
        controller_log_admit(&gate, 7000, poll_line, &notice);
        CHECK(notice == NULL);
    }
    CHECK(gate.poll_dropped == 0);
}

#define MUTATIONS (1u << 17)

static unsigned int seed = 20261103u;
static unsigned int random_below(unsigned int limit)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % limit;
}

/* No shape of a format text may break the two rules the gate promises: the decision is an
 * admit or a drop with an optional fixed notice, and no class ever exceeds its limit. */
static void test_mutated_formats(void)
{
    static const char *seeds[] = {
        "ZTE_CONTROLLER_TRACE call=%s\n",
        "ZTE_CONTROLLER_TRACE rx1905 bytes=%u\n",
        " base: device_event dev=%d\n",
        " cmapi: renewACD band=%d\n",
        "ZTE_TRACE_VALUE text=acdcParseM2Msg cfgChg=%d\n",
        "ZTE_TRACE_POLL text=acdc current state[%d]:%d\n",
        "ZTE_TRACE_ALL vendor_format=%.150s\n",
        "acdc current state[%d]:%d,event:%d\n",
        "Bss PWKey=%s",
        "%s",
        "",
    };
    static const unsigned int limits[] = {
        ZTE_LOG_ORDINARY_LIMIT, ZTE_LOG_PROTOCOL_LIMIT, ZTE_LOG_DEVICE_LIMIT, ZTE_LOG_RENEW_LIMIT,
        ZTE_LOG_VALUE_LIMIT, ZTE_LOG_POLL_LIMIT, ZTE_LOG_FIREHOSE_LIMIT,
    };
    struct log_gate gate;
    unsigned int admitted[ZTE_LOG_COUNT_CLASSES];
    char text[96];
    unsigned int i, k, windows_seen, notices = 0;
    controller_log_open(&gate);
    memset(admitted, 0, sizeof(admitted));
    for (i = 0; i < MUTATIONS; i++) {
        const char *seed_text = seeds[random_below(sizeof(seeds) / sizeof(seeds[0]))];
        size_t length = strlen(seed_text);
        const char *notice = NULL;
        int class, shape, allowed;
        if (length >= sizeof(text))
            length = sizeof(text) - 1;
        memcpy(text, seed_text, length + 1);
        for (k = 0; k < 3 && length; k++) {
            static const char alphabet[] = "ZTE0189_-%sacvpollvalue_ridgefnwVLAN_ASSERT";
            unsigned int position = random_below((unsigned int)length);
            /* Never touch the terminator: a truncated text is a different input, and the
             * preload always passes a formatted, terminated line. */
            if (position + 1 == length && text[position] == '\n')
                continue;
            text[position] = alphabet[random_below(sizeof(alphabet) - 1)];
        }
        CHECK(strlen(text) < sizeof(text));
        class = controller_log_class(text);
        CHECK(class >= 0 && class < ZTE_LOG_COUNT_CLASSES);
        shape = controller_log_shape(text);
        CHECK(shape == -1 || shape == ZTE_LOG_VALUE || shape == ZTE_LOG_POLL);
        allowed = controller_log_admit(&gate, (long)i, text, &notice);
        CHECK(allowed == 0 || allowed == 1);
        /* An admitted line never carries a notice: the two are alternatives. */
        CHECK(!allowed || notice == NULL);
        if (notice) {
            /* Only the three streams that can go quiet without looking broken may say so,
             * and what they say is fixed text - never part of the line that triggered it. */
            CHECK(class == ZTE_LOG_ORDINARY || class == ZTE_LOG_PROTOCOL || class == ZTE_LOG_POLL);
            CHECK(strncmp(notice, "ZTE_CONTROLLER_TRACE log_budget=",
                          strlen("ZTE_CONTROLLER_TRACE log_budget=")) == 0);
            if (class == ZTE_LOG_POLL)
                notices++;
        }
        if (allowed)
            admitted[class]++;
    }
    /* Nothing exceeded a limit, whatever text arrived. The poll budget re-arms once per
     * window, so its total is bounded by the number of windows the clock walked through. */
    windows_seen = (MUTATIONS + ZTE_LOG_POLL_WINDOW - 1) / ZTE_LOG_POLL_WINDOW + 1;
    for (i = 0; i < ZTE_LOG_COUNT_CLASSES; i++) {
        unsigned int ceiling = limits[i];
        if (i == ZTE_LOG_POLL)
            ceiling = ZTE_LOG_POLL_LIMIT * windows_seen;
        CHECK(admitted[i] <= ceiling);
    }
    /* The flood classes really were exercised, or the ceilings above proved nothing:
     * ordinary runs out and stops, and poll keeps flowing across many windows. */
    CHECK(admitted[ZTE_LOG_ORDINARY] == ZTE_LOG_ORDINARY_LIMIT);
    CHECK(admitted[ZTE_LOG_POLL] >= ZTE_LOG_POLL_LIMIT * 100);
    /* A notice is not its own flood: the two exhaustion lines are written once each and
     * the sampling notice at most once per window. */
    CHECK(notices <= windows_seen + 2);
}

int main(void)
{
    test_class_and_shape();
    test_per_process_budgets();
    test_notices_say_what_stopped();
    test_one_push_leaves_the_ring_alone();
    test_lines_that_are_not_poll_are_never_sampled();
    test_the_notice_reports_what_it_dropped();
    test_mutated_formats();
    printf("log gate rules hold: %u checks\n", checks);
    return 0;
}
