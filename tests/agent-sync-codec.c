/* Host test of the agent synchroniser: the plan text the host-side wlan-sync parses, the change detector that
 * decides whether a plan has to be written at all, and the write itself. The database side is exercised on the
 * lab router (docs/LAB.md section 3.1), not here: agent_sync_read() needs the vendor shared-memory engine, so
 * the five vendor symbols it uses are stubbed. With those stubs no row is readable, which is what lets
 * agent_sync_poll() run all the way to plan_write() with an empty plan.
 */
#define _GNU_SOURCE
#include "agent-sync.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* Link stubs: the reader walks them and finds no readable row, so the poll publishes an empty plan. */
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

static int file_text(const char *path, char *out, size_t capacity)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(out, 1, capacity - 1, f);
    out[n] = 0;
    fclose(f);
    return 0;
}

/* The temp name is fixed ("<path>.tmp") and its directory is the one host path the sandbox may write, and the
 * sandbox runs as this same UID. A symlink planted there must not steer the write: the plan carries passphrases. */
static void plan_write_tests(void)
{
    char root[80], path[128], temp[128], victim[128], escaped[128], text[512];
    struct stat st;
    int fd;

    snprintf(root, sizeof(root), "/tmp/zte-agentsync-%ld-XXXXXX", (long)getpid());
    if (!mkdtemp(root)) {
        fprintf(stderr, "agent-sync-codec: cannot make a temporary directory\n");
        failures++;
        return;
    }
    snprintf(path, sizeof(path), "%s/wlan-plan", root);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    snprintf(victim, sizeof(victim), "%s/victim", root);
    snprintf(escaped, sizeof(escaped), "%s/escaped", root);

    agent_sync_reset();
    CHECK(agent_sync_poll(path) == 1);
    CHECK(stat(path, &st) == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(lstat(temp, &st) != 0);
    CHECK(file_text(path, text, sizeof(text)) == 0);
    CHECK(strstr(text, "\nrows=0\n") != NULL);

    /* An existing file behind the temp name: refused, and neither it nor the published plan is touched. */
    fd = open(victim, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "untouched\n", 10) == 10);
        (void)close(fd);
    }
    CHECK(symlink(victim, temp) == 0);
    agent_sync_reset();
    CHECK(agent_sync_poll(path) == -1);
    CHECK(lstat(temp, &st) == 0);
    CHECK(S_ISLNK(st.st_mode));
    CHECK(file_text(victim, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "untouched\n"));
    CHECK(file_text(path, text, sizeof(text)) == 0);
    CHECK(strstr(text, "\nrows=0\n") != NULL);
    (void)unlink(temp);

    /* A name that does not exist yet: without O_NOFOLLOW, O_CREAT would make it and put the plan there. */
    CHECK(symlink(escaped, temp) == 0);
    agent_sync_reset();
    CHECK(agent_sync_poll(path) == -1);
    CHECK(stat(escaped, &st) != 0);
    CHECK(lstat(temp, &st) == 0);
    CHECK(S_ISLNK(st.st_mode));

    (void)unlink(temp);
    (void)unlink(victim);
    (void)unlink(path);
    (void)rmdir(root);
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

    plan_write_tests();

    if (failures) {
        printf("agent-sync-codec: %u assertion(s) failed\n", failures);
        return 1;
    }
    printf("agent-sync-codec: ok\n");
    return 0;
}
