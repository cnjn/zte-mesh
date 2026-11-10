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

/* One AP of the collector's snapshot: its count, its SSID, and where its client MACs start in the table's one
 * flat MAC array. A row is written by a "b" line before the "s" lines that belong to it, so the collector's own
 * order is the only one that reads. */
struct ap_table_row {
    uint8_t bssid[6];
    unsigned int stations;
    char ssid[CONTROLLER_SSID_MAX + 1];
    unsigned int first;
    unsigned int clients;
};

/* A number that ends at a space, for the count that is followed by the SSID on the same line. `rest` is the text
 * after that space, or the terminator when the line ends with the number. */
static int parse_number_field(const char *text, unsigned long max, unsigned long *out, const char **rest)
{
    unsigned long value = 0;
    int digits = 0;
    for (; *text && *text != ' '; text++) {
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
    if (rest)
        *rest = *text ? text + 1 : text;
    return 1;
}

/* An SSID as it can leave this process: an SSID is bytes, not text, but a tab or a newline in it would break the
 * one-line-per-client file the master keeps, so those and the other control bytes become spaces. Truncated to the
 * thirty-two bytes the wire field carries. */
static void copy_ssid(char out[CONTROLLER_SSID_MAX + 1], const char *text)
{
    size_t used = 0;
    for (; *text && used < CONTROLLER_SSID_MAX; text++) {
        unsigned char c = (unsigned char)*text;
        out[used++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    out[used] = 0;
}

/* The table as it stands, replaced whole every few seconds:
 *
 *   epoch=<unix>
 *   b <bssid> <count> [<ssid>]
 *   s <bssid> <station>
 *
 * A "b" line is an AP this node runs; the "s" lines under it are the stations the collector saw on that AP. A line
 * that is neither is damage and is skipped - the file is root-owned tmpfs, and a row that is damage must not become
 * a number or a device on the page. A broken epoch line is different: the age of every row would be unknown, so the
 * whole table is read as nothing measured. */
static unsigned int table_load(struct ap_table_row *rows, unsigned int max, uint8_t macs[][6],
                               unsigned int max_macs, unsigned int *mac_count, time_t *epoch)
{
    static const char header[] = "epoch=";
    char buffer[TABLE_BYTES], *start, *next;
    unsigned int count = 0;
    int fd;
    ssize_t bytes;
    *epoch = 0;
    *mac_count = 0;
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
        } else if (len > 20 && start[0] == 'b' && start[1] == ' ' && start[19] == ' ') {
            uint8_t bssid[6];
            const char *rest;
            if (count < max && parse_bssid(start + 2, bssid) &&
                parse_number_field(start + 20, 65535ul, &value, &rest)) {
                memcpy(rows[count].bssid, bssid, 6);
                rows[count].stations = (unsigned int)value;
                rows[count].first = *mac_count;
                rows[count].clients = 0;
                copy_ssid(rows[count].ssid, rest);
                count++;
            }
        } else if (len == 37 && start[0] == 's' && start[1] == ' ' && start[19] == ' ') {
            uint8_t bssid[6], mac[6];
            if (parse_bssid(start + 2, bssid) && parse_bssid(start + 20, mac)) {
                unsigned int k;
                for (k = 0; k < count; k++)
                    if (!memcmp(rows[k].bssid, bssid, 6))
                        break;
                if (k < count && *mac_count < max_macs && rows[k].clients < 255) {
                    memcpy(macs[*mac_count], mac, 6);
                    (*mac_count)++;
                    rows[k].clients++;
                }
            }
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

size_t agent_ap_metrics_answer(const uint8_t *frame, size_t bytes, uint8_t *reply, size_t capacity,
                               const uint8_t al[6], uint8_t dest[6])
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    struct ap_table_row rows[CONTROLLER_AP_BSS_MAX];
    struct ap_bss_metric answers[CONTROLLER_AP_BSS_MAX];
    struct ap_client_group groups[CONTROLLER_AP_BSS_MAX];
    uint8_t macs[CONTROLLER_CLIENTS_MAX][6];
    uint8_t asked[CONTROLLER_AP_BSS_MAX][6];
    struct cmdu_header header;
    time_t epoch, now;
    unsigned int listed, row_count, mac_count, k, j, answer_count = 0, group_count = 0;
    if (!al || !ap_metrics_query_decode(frame, bytes, &header, asked, &listed))
        return 0;
    /* Only a query addressed to this node's own AL MAC - or to everyone - is this node's question. A query to any
     * other address, another router's AL or a group address, is answered by nobody here: the reply would either
     * speak for a node that did not ask, or carry an address that is not this node's as its source, which is the key
     * the master files the counts under. */
    if (memcmp(frame, al, 6) && memcmp(frame, broadcast, 6))
        return 0;
    row_count = table_load(rows, CONTROLLER_AP_BSS_MAX, macs, CONTROLLER_CLIENTS_MAX, &mac_count, &epoch);
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
    /* Only the BSSes this answer covers carry clients: a query that names a subset must not report the stations
     * of an AP it did not ask about. The table's one flat MAC array is cut up into the groups that are answered,
     * and a MAC that two rows happen to list (a station moving between two APs of this node mid-scan) is kept once,
     * because the master's copy is keyed by MAC. */
    {
        uint8_t answer_macs[CONTROLLER_CLIENTS_MAX][6];
        unsigned int answer_macs_count = 0;
        for (k = 0; k < answer_count; k++) {
            unsigned int m, kept = 0;
            for (j = 0; j < row_count; j++)
                if (!memcmp(answers[k].bssid, rows[j].bssid, 6))
                    break;
            if (j == row_count || !rows[j].clients)
                continue;
            memcpy(groups[group_count].bssid, rows[j].bssid, 6);
            memcpy(groups[group_count].ssid, rows[j].ssid, sizeof(rows[j].ssid));
            groups[group_count].first = answer_macs_count;
            for (m = 0; m < rows[j].clients && answer_macs_count < CONTROLLER_CLIENTS_MAX; m++) {
                unsigned int seen;
                for (seen = 0; seen < answer_macs_count; seen++)
                    if (!memcmp(answer_macs[seen], macs[rows[j].first + m], 6))
                        break;
                if (seen == answer_macs_count) {
                    memcpy(answer_macs[answer_macs_count], macs[rows[j].first + m], 6);
                    answer_macs_count++;
                    kept++;
                }
            }
            groups[group_count].count = kept;
            if (kept)
                group_count++;
        }
        memcpy(dest, frame + 6, 6);
        return ap_metrics_response_build_clients(reply, capacity, dest, al, header.message_id, answers,
                                                 answer_count, groups, group_count,
                                                 (const uint8_t (*)[6])answer_macs, answer_macs_count);
    }
}
