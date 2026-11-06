/* Host test of the agent synchroniser's pure part: the plan text the host-side wlan-sync parses, and the
 * change detector that decides whether a plan has to be written at all. The database side is exercised on the
 * lab router (docs/LAB.md section 3.1), not here: agent_sync_read() needs the vendor shared-memory engine, so
 * the four vendor symbols it uses are stubbed and only agent_sync_format()/agent_sync_changed() are driven.
 */
#include "agent-sync.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* Link stubs: never reached by the two functions under test. */
int dbAPIGetView(const char *view, const char *instance, void *data)
{
    (void)view; (void)instance; (void)data;
    return -1;
}
unsigned int dbGetPmTypeValLen(unsigned int type)
{
    (void)type;
    return 0;
}
unsigned int dbGetPrmValLen(unsigned int type)
{
    (void)type;
    return 0;
}
int WlanIndex2InstId(unsigned int radio, unsigned int slot, char *out)
{
    (void)radio; (void)slot; (void)out;
    return -1;
}
int WlanIndex2PSKInstId(unsigned int radio, unsigned int slot, char *out)
{
    (void)radio; (void)slot; (void)out;
    return -1;
}

/* Bounded copy: the row fields are fixed cells and -Werror=format-truncation rejects snprintf("%s"). */
static void set_text(char *out, size_t capacity, const char *text)
{
    size_t n = strlen(text);
    if (n >= capacity)
        n = capacity - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

static void row_init(struct agent_sync_row *row, unsigned int radio, const char *ssid, unsigned int enabled,
                     unsigned int beacon, unsigned int key_length)
{
    char psk[36];
    memset(row, 0, sizeof(*row));
    row->radio = radio;
    row->slot = 0;
    set_text(row->instance, sizeof(row->instance), radio ? "DEV.WIFI.AP5" : "DEV.WIFI.AP1");
    set_text(psk, sizeof(psk), row->instance);
    set_text(psk + strlen(psk), sizeof(psk) - strlen(psk), ".PSK1");
    set_text(row->psk_instance, sizeof(row->psk_instance), psk);
    set_text(row->ssid, sizeof(row->ssid), ssid);
    row->ssid_length = (unsigned int)strlen(row->ssid);
    row->enabled = enabled;
    row->hidden = 0;
    row->access_attr = 1;
    row->beacon_type = beacon;
    row->key_length = key_length;
    row->readable = 1;
}

static int count_lines(const char *text, const char *prefix)
{
    int n = 0;
    size_t at = 0, length = strlen(prefix);
    while (text[at]) {
        const char *end = strchr(text + at, '\n');
        if (!end)
            break;
        if (!strncmp(text + at, prefix, length))
            n++;
        at = (size_t)(end - text) + 1;
    }
    return n;
}

int main(void)
{
    struct agent_sync_row rows[AGENT_SYNC_ROWS], before[AGENT_SYNC_ROWS];
    char keys[AGENT_SYNC_ROWS][AGENT_SYNC_KEY_BYTES];
    char plan[AGENT_SYNC_PLAN_BYTES];
    char small[64];

    /* 1. two rows, the shape the child publishes */
    memset(rows, 0, sizeof(rows));
    memset(keys, 0, sizeof(keys));
    row_init(&rows[0], 0, "zte-agent-2g", 1, 3, 16);
    row_init(&rows[1], 1, "zte-agent-5g", 1, 6, 16);
    snprintf(keys[0], sizeof(keys[0]), "%s", "m2-public-2g-key");
    snprintf(keys[1], sizeof(keys[1]), "%s", "m2-public-5g-key");
    CHECK(agent_sync_format(rows, keys, 2, plan, sizeof(plan)) == 0);
    CHECK(!strncmp(plan, "# zte-mesh agent synchronisation plan.", 38));
    CHECK(strstr(plan, "\nrows=2\n") != NULL);
    CHECK(strstr(plan, "\n0.instance=DEV.WIFI.AP1\n") != NULL);
    CHECK(strstr(plan, "\n0.psk_instance=DEV.WIFI.AP1.PSK1\n") != NULL);
    CHECK(strstr(plan, "\n0.ssid=zte-agent-2g\n") != NULL);
    CHECK(strstr(plan, "\n0.key=m2-public-2g-key\n") != NULL);
    CHECK(strstr(plan, "\n0.enabled=1\n") != NULL);
    CHECK(strstr(plan, "\n0.beacon_type=3\n") != NULL);
    CHECK(strstr(plan, "\n1.radio=1\n") != NULL);
    CHECK(strstr(plan, "\n1.ssid=zte-agent-5g\n") != NULL);
    CHECK(strstr(plan, "\n1.key=m2-public-5g-key\n") != NULL);
    /* every row carries ten options, so a parser can walk them by index */
    CHECK(count_lines(plan, "0.") == 10);
    CHECK(count_lines(plan, "1.") == 10);

    /* 2. a value with '=' survives: the reader splits on the first one only */
    memset(rows, 0, sizeof(rows));
    row_init(&rows[0], 0, "lab=a=b", 1, 3, 8);
    snprintf(keys[0], sizeof(keys[0]), "%s", "key=with=equals");
    CHECK(agent_sync_format(rows, keys, 1, plan, sizeof(plan)) == 0);
    CHECK(strstr(plan, "\n0.ssid=lab=a=b\n") != NULL);
    CHECK(strstr(plan, "\n0.key=key=with=equals\n") != NULL);

    /* 3. control characters cannot break the line format */
    memset(rows, 0, sizeof(rows));
    row_init(&rows[0], 0, "x", 1, 3, 0);
    snprintf(rows[0].ssid, sizeof(rows[0].ssid), "a\tb\nc");
    rows[0].ssid_length = 5;
    CHECK(agent_sync_format(rows, keys, 1, plan, sizeof(plan)) == 0);
    CHECK(strstr(plan, "\n0.ssid=a?b?c\n") != NULL);

    /* 4. an unreadable row is still counted but contributes no options */
    memset(rows, 0, sizeof(rows));
    row_init(&rows[0], 0, "kept", 1, 3, 4);
    row_init(&rows[1], 1, "gone", 1, 3, 4);
    rows[1].readable = 0;
    CHECK(agent_sync_format(rows, keys, 2, plan, sizeof(plan)) == 0);
    CHECK(strstr(plan, "\nrows=2\n") != NULL);
    CHECK(count_lines(plan, "1.") == 0);

    /* 5. a plan that does not fit is refused instead of truncated */
    CHECK(agent_sync_format(rows, keys, 2, small, sizeof(small)) != 0);
    CHECK(agent_sync_format(rows, keys, 2, plan, 0) != 0);
    CHECK(agent_sync_format(rows, keys, AGENT_SYNC_ROWS + 1, plan, sizeof(plan)) != 0);

    /* 6. the change detector: identity, then one field at a time */
    memset(before, 0, sizeof(before));
    memset(rows, 0, sizeof(rows));
    row_init(&before[0], 0, "zte-agent-2g", 1, 3, 16);
    row_init(&rows[0], 0, "zte-agent-2g", 1, 3, 16);
    before[0].key_hash = rows[0].key_hash = 12345;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 0);
    rows[0].ssid[4] = 'X'; /* zte-agent-2g -> zte-Xgent-2g */
    CHECK(agent_sync_changed(before, 1, rows, 1) == 1);
    memcpy(rows[0].ssid, before[0].ssid, sizeof(rows[0].ssid));
    rows[0].key_hash = 999;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 1);
    rows[0].key_hash = before[0].key_hash;
    rows[0].enabled = 0;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 1);
    rows[0].enabled = 1;
    rows[0].beacon_type = 12;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 1);
    rows[0].beacon_type = 3;
    rows[0].hidden = 1;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 1);
    rows[0].hidden = 0;
    CHECK(agent_sync_changed(before, 1, rows, 1) == 0);
    /* a different number of rows is a change even when the first ones agree */
    CHECK(agent_sync_changed(before, 1, rows, 0) == 1);
    CHECK(agent_sync_changed(before, 0, rows, 1) == 1);

    if (failures) {
        printf("agent-sync-codec: %u assertion(s) failed\n", failures);
        return 1;
    }
    printf("agent-sync-codec: ok\n");
    return 0;
}
