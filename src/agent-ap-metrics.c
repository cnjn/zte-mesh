#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "agent-ap-metrics.h"
#include "controller-1905.h"

/* The whole table the collector can fill in one pass: sixteen BSSes of "<bssid> <count>" is a few hundred bytes,
 * and the read that comes back longer than this is a table this unit will not finish, so it answers from what it
 * read and treats the rest as not measured. */
#define TABLE_BYTES 4096

/* Six collector intervals, and the same 30 seconds the master gives a per-BSS file before it stops counting it:
 * a table older than this was not written by anything still running and its counts describe Wi-Fi interfaces that
 * are gone, so silence — which is what the vendor agent does — is the honest answer. */
#define TABLE_MAX_AGE 30

static const char *table_path(void)
{
    const char *file = getenv("ZTE_MESH_AP_STA_FILE");
    return file && *file ? file : "/run/zte-mesh/ap-sta";
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A MAC in the spelling sysfs and the collector use: six hex pairs, a colon between each pair. Looks at exactly
 * seventeen characters - the caller checks what follows. */
static int parse_bssid(const char *text, uint8_t out[6])
{
    int k;
    for (k = 0; k < 6; k++) {
        int hi = hex_nibble(text[k * 3]), lo = hex_nibble(text[k * 3 + 1]);
        if (hi < 0 || lo < 0 || (k < 5 && text[k * 3 + 2] != ':'))
            return 0;
        out[k] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* A decimal number with nothing after it. Bounded because a field wider than the byte it goes into is not a value
 * this unit can report: a count above 65535 is half a station count that looks measured, and an epoch past
 * UINT32_MAX is not a time this daemon can compare with its clock. */
static int parse_number(const char *text, unsigned long max, unsigned long *out)
{
    unsigned long value = 0;
    int digits = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            return 0;
        value = value * 10u + (unsigned long)(*text - '0');
        if (value > max)
            return 0;
        digits++;
    }
    if (!digits)
        return 0;
    *out = value;
    return 1;
}

/* The table as it stands: rows of "<bssid> <count>" under an "epoch=<unix>" line, replaced whole every few
 * seconds. A line that is neither of those is not a BSS and is skipped - the file is root-owned tmpfs, and a row
 * that is damage must not become a number on the page. A broken epoch line is different: the age of every row
 * would be unknown, so the whole table is read as nothing measured. */
static unsigned int table_load(struct ap_bss_metric *rows, unsigned int max, time_t *epoch)
{
    static const char header[] = "epoch=";
    char buffer[TABLE_BYTES], *start, *next;
    unsigned int count = 0;
    int fd;
    ssize_t bytes;
    *epoch = 0;
    fd = open(table_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    bytes = read(fd, buffer, sizeof(buffer) - 1);
    (void)close(fd);
    if (bytes < 0)
        return 0;
    buffer[bytes] = 0;
    for (start = buffer; *start; start = next) {
        size_t len;
        unsigned long value;
        next = strchr(start, '\n');
        if (next)
            *next++ = 0;
        len = strlen(start);
        if (len && start[len - 1] == '\r')
            start[--len] = 0;
        if (!strncmp(start, header, sizeof(header) - 1)) {
            if (!parse_number(start + sizeof(header) - 1, 2147483647ul, &value))
                return 0;
            *epoch = (time_t)value;
        } else if (len > 18 && start[17] == ' ' && count < max && parse_bssid(start, rows[count].bssid) &&
                   parse_number(start + 18, 65535ul, &value)) {
            rows[count].stations = (unsigned int)value;
            count++;
        }
        if (!next)
            break;
    }
    return count;
}

/* One more BSS of the answer, or the same one again: a query that names a BSSID twice is one BSS, and the master
 * sums the 0x94s of a frame, so a repeated entry would report that BSS's clients twice. */
static int answer_add(struct ap_bss_metric *answers, unsigned int *count, const uint8_t bssid[6],
                      unsigned int stations)
{
    unsigned int k;
    if (*count >= CONTROLLER_AP_BSS_MAX)
        return 0;
    for (k = 0; k < *count; k++)
        if (!memcmp(answers[k].bssid, bssid, 6))
            return 1;
    memcpy(answers[*count].bssid, bssid, 6);
    answers[*count].stations = stations;
    (*count)++;
    return 1;
}

size_t agent_ap_metrics_answer(const uint8_t *frame, size_t bytes, uint8_t *reply, size_t capacity, uint8_t dest[6])
{
    struct ap_bss_metric rows[CONTROLLER_AP_BSS_MAX], answers[CONTROLLER_AP_BSS_MAX];
    uint8_t asked[CONTROLLER_AP_BSS_MAX][6];
    struct cmdu_header header;
    time_t epoch, now;
    unsigned int listed, row_count, k, j, answer_count = 0;
    if (!ap_metrics_query_decode(frame, bytes, &header, asked, &listed))
        return 0;
    /* A query sent to a group address is not addressed to this node's AL MAC, and answering one would put that
     * group address in the reply's source - the key the master files the counts under. */
    if (frame[0] & 1)
        return 0;
    row_count = table_load(rows, CONTROLLER_AP_BSS_MAX, &epoch);
    if (!row_count)
        return 0;
    now = time(NULL);
    if (now < epoch || now - epoch > TABLE_MAX_AGE)
        return 0;
    if (!listed) {
        /* The count byte of 0x93 is the list's length, and the reading of an empty list is "every BSS this node
         * runs" - which is the only question our own query builder does not ask, since it names what it knows. */
        for (k = 0; k < row_count; k++)
            answer_add(answers, &answer_count, rows[k].bssid, rows[k].stations);
    } else {
        for (k = 0; k < listed; k++)
            for (j = 0; j < row_count; j++)
                if (!memcmp(asked[k], rows[j].bssid, 6)) {
                    answer_add(answers, &answer_count, rows[j].bssid, rows[j].stations);
                    break;
                }
    }
    if (!answer_count)
        return 0;
    memcpy(dest, frame + 6, 6);
    return ap_metrics_response_build(reply, capacity, dest, frame, header.message_id, answers, answer_count);
}
