#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "controller-1905.h"
#include "controller-peer.h"
#include "controller-steer.h"

#define PEERS_KNOWN 32
#define BSS_STATIONS_SUFFIX ".sta"

/* The vendor's glibc 2.26 on ARM32 exports only the versioned stat entry points, as controller-service.c notes for
 * its own reads; a host build, which is where the peer codec test runs, has the plain fstat(). */
#if defined(__arm__)
extern int __fxstat(int, int, struct stat *);
#define fstat(fd, info) __fxstat(3, (fd), (info))
#endif

static const char *peers_dir(void)
{
    const char *dir = getenv("ZTE_MESH_PEERS_DIR");
    return dir && *dir ? dir : "/run/zte-mesh/peers";
}

/* One path under the peers directory, through a temporary name in the same directory and then renamed, so the
 * service never reads a half value. `name` is "<MAC>.<kind>", or "<MAC>.<BSSID>.sta" for one BSS of one node.
 * 1 = the file is in place. */
static int write_named(const char *name, const char *text, size_t bytes)
{
    char path[256], temp[264];
    int fd;
    if (snprintf(path, sizeof(path), "%s/%s", peers_dir(), name) >= (int)sizeof(path) ||
        snprintf(temp, sizeof(temp), "%s.t", path) >= (int)sizeof(temp))
        return 0;
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return 0;
    {
        ssize_t written = write(fd, text, bytes);
        int closed = close(fd);
        if (written == (ssize_t)bytes && !closed && !rename(temp, path))
            return 1;
    }
    (void)unlink(temp);
    return 0;
}

/* "<MAC>.<kind>": one file per (node, thing the page shows). 1 = the file is in place. */
static int write_peer(const uint8_t *mac, const char *kind, const char *text, size_t bytes)
{
    char name[48];
    if (snprintf(name, sizeof(name), "%02x:%02x:%02x:%02x:%02x:%02x.%s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], kind) >= (int)sizeof(name))
        return 0;
    return write_named(name, text, bytes);
}

/* "<MAC>.<BSSID>.sta": how many stations the node counted on that BSS, one number per frame that reported it.
 * The count of a BSS with nobody associated is a zero, and a zero written here is the node saying so - which is
 * why a frame that names a BSS at all is enough, and why the reader tells "no entry" from "entry: 0". */
static int write_bss(const uint8_t *mac, const struct ap_bss_metric *bss)
{
    char name[64], text[24];
    int n;
    if (snprintf(name, sizeof(name),
                 "%02x:%02x:%02x:%02x:%02x:%02x.%02x:%02x:%02x:%02x:%02x:%02x" BSS_STATIONS_SUFFIX,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5])
        >= (int)sizeof(name))
        return 0;
    n = snprintf(text, sizeof(text), "%u\n", bss->stations);
    return n > 0 && write_named(name, text, (size_t)n);
}

/* Is this MAC one of the PEERS_KNOWN the daemon will record? 1 = admitted (a known MAC, or a new one with room
 * left), 0 = the budget is full and this frame is from a stranger. */
static int admit(const uint8_t *mac)
{
    static uint8_t known[PEERS_KNOWN][6];
    static unsigned int count;
    unsigned int k;
    for (k = 0; k < count; k++)
        if (!memcmp(known[k], mac, 6))
            return 1;
    if (count >= PEERS_KNOWN)
        return 0;
    memcpy(known[count++], mac, 6);
    return 1;
}

int controller_peer_note(const uint8_t *frame, size_t bytes)
{
    struct cmdu_header header;
    struct tlv_cursor cursor;
    struct node_metrics metrics;
    struct ap_bss_metric bss_list[CONTROLLER_AP_BSS_MAX];
    const uint8_t *value, *mac = frame + 6;
    unsigned int type, bss_count = 0, k;
    size_t length, used = 0;
    int light = -1, written = 0;
    char text[72];
    char software[CONTROLLER_INVENTORY_TEXT + 1];
    int n;

    if (bytes < CONTROLLER_1905_HEADER || !cmdu_decode(frame, bytes, &header) ||
        !header.last_fragment || header.fragment)
        return 0;

    /* The answers to a client action: a BTM report (0x8015) says whether the client accepted, and a Steering
     * Completed (0x8017) says it left. Both name the client, which is what the action table keys on, so the file is
     * written for the client's MAC and not for the agent that sent the frame. */
    if (header.type == STEER_MSG_BTM_REPORT) {
        struct steer_btm_report report;
        if (!steer_decode_btm_report(frame, bytes, &report))
            return 0;
        if (!admit(report.client))
            return 0;
        n = snprintf(text, sizeof(text), "ok=%d\n", report.status == 0 ? 1 : 0);
        return n > 0 && write_peer(report.client, "btm", text, (size_t)n);
    }
    if (header.type == STEER_MSG_COMPLETED) {
        struct steer_completed done;
        static const char accepted[] = "ok=1\n";
        if (!steer_decode_completed(frame, bytes, &done) || !admit(done.client))
            return 0;
        return write_peer(done.client, "btm", accepted, sizeof(accepted) - 1);
    }

    software[0] = 0;
    tlv_open(&cursor, frame, bytes);
    while (tlv_next(&cursor, &type, &value, &length)) {
        struct zte_cursor zte;
        /* The version only means something inside an AP Capability Report: the factory daemon puts this TLV in
         * that message and reads it back from it, and the number is its own, not the standard's. */
        if (type == CONTROLLER_TLV_DEVICE_INVENTORY) {
            if (header.type == CONTROLLER_MSG_AP_CAPABILITY_REPORT)
                device_inventory_software(value, length, software, sizeof(software));
            continue;
        }
        /* Only a HigherLayerResponse carries the light: the same sub-TLV number inside another message type is a
         * different field, and the master reads it from the response too. */
        if (header.type != 0x000e || type != CONTROLLER_TLV_VENDOR || !zte_vendor_open(&zte, value, length))
            continue;
        while (zte_vendor_next(&zte, &type, &value, &length))
            if (type == 0x21 && length >= 7) {
                light = value[6] ? 1 : 0;
                mac = value; /* the AL MAC the child reports for itself */
            }
    }
    if (!node_metrics_decode(frame, bytes, &metrics))
        return 0;
    /* Only frames that describe the link or its load are worth a file: the daemon's status chatter (CMDU 0x0004 on
     * the lab link, ~2/s) says nothing the page shows, and rewriting a file for it would churn the tmpfs for no
     * gain. */
    if (!(metrics.fields & (NODE_METRIC_STATIONS | NODE_METRIC_BSS)) &&
        header.type != 0x0003 && header.type != 0x0006 && header.type != 0x000e && light < 0 && !software[0])
        return 0;
    if (!admit(mac))
        return 0;
    if (metrics.fields & NODE_METRIC_STATIONS)
        bss_count = ap_metrics_by_bss(frame, bytes, bss_list, CONTROLLER_AP_BSS_MAX);

    /* A version is not a number, and it does not go stale the way a station count does, so it gets its own file
     * with the text in it instead of a key in the metrics digest. */
    if (software[0]) {
        n = snprintf(text, sizeof(text), "%s\n", software);
        if (n > 0 && (size_t)n < sizeof(text))
            written += write_peer(mac, "software", text, (size_t)n);
    }
    if (light >= 0) {
        n = snprintf(text, sizeof(text), "light=%d\n", light);
        if (n > 0)
            written += write_peer(mac, "light", text, (size_t)n);
    }
    /* A count that the frame did not carry is left out, so the reader sees "no data" instead of a zero that looks
     * measured. */
    if (metrics.fields & NODE_METRIC_STATIONS)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "stations=%u\n", metrics.stations);
    if (metrics.fields & NODE_METRIC_BSS)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "bss=%u\n", metrics.bss);
    if (metrics.hops > 0)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "hops=%d\n", metrics.hops);
    if (used)
        written += write_peer(mac, "metrics", text, used);
    /* The same frame counted its stations per BSS; that breakdown is the only way the page can tell a busy 5G
     * from an idle radio with one guest. The digest above stays the frame's own total, so a frame whose list this
     * decoder will not split (a repeated BSSID, more than CONTROLLER_AP_BSS_MAX of them) still leaves a number. */
    for (k = 0; k < bss_count; k++)
        written += write_bss(mac, &bss_list[k]);
    return written;
}

/* The one number in "<dir>/<mac>.<bssid>.sta", or -1: the text this side wrote, checked rather than trusted,
 * because the directory is world-writable tmpfs and a stray file has to read as "no entry". */
static int read_bss_entry(const char *name, time_t *when)
{
    char path[256], text[24];
    struct stat info;
    ssize_t bytes;
    unsigned int value = 0;
    int fd, k;
    if (snprintf(path, sizeof(path), "%s/%s", peers_dir(), name) >= (int)sizeof(path))
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size >= (off_t)sizeof(text)) {
        (void)close(fd);
        return -1;
    }
    bytes = read(fd, text, sizeof(text) - 1);
    (void)close(fd);
    if (bytes < 1)
        return -1;
    text[bytes] = 0;
    /* One line, the way this side writes it: the end-of-line is not part of the number, and anything else that is
     * not a digit is not a station count either. */
    while (bytes && (text[bytes - 1] == '\n' || text[bytes - 1] == '\r' || text[bytes - 1] == ' '))
        text[--bytes] = 0;
    if (!bytes)
        return -1;
    for (k = 0; text[k]; k++) {
        if (text[k] < '0' || text[k] > '9')
            return -1;
        value = value * 10u + (unsigned int)(text[k] - '0');
        if (value > 0xffffu) /* a u16 is what the wire carries; anything wider is not a station count */
            return -1;
    }
    *when = info.st_mtime;
    return (int)value;
}

/* "<MAC>.<BSSID>.sta": the two MAC halves in their own places, the tail as it is spelled here. Nothing else in the
 * directory is an entry, and a name that only looks like one is what someone else's file would be. */
static int is_bss_entry_name(const char *name, const char *mac)
{
    const size_t bssid = 18; /* strlen("aa:bb:cc:dd:ee:ff.") */
    size_t k;
    if (strlen(name) != 17 + 1 + 17 + (sizeof(BSS_STATIONS_SUFFIX) - 1) || memcmp(name, mac, 17) || name[17] != '.')
        return 0;
    for (k = 0; k < 17; k++) {
        const char c = name[bssid + k];
        if (k % 3 == 2) {
            if (c != ':')
                return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return !memcmp(name + bssid + 17, BSS_STATIONS_SUFFIX, sizeof(BSS_STATIONS_SUFFIX) - 1);
}

/* One live entry into the detail list, kept in BSSID order: readdir() gives the directory's own order, which
 * changes when a file is rewritten, and a page that shows the same rows in a different order every second looks
 * like the network is. A list that is already full drops the largest BSSID - the sum above still counted it. */
static void put_detail(struct peer_bss_load *load, const char *name, unsigned int stations, long age)
{
    struct peer_bss_entry *d = load->detail;
    const char *bssid = name + 18; /* "<mac 17>.<bssid 17>.sta" */
    unsigned int k = load->listed;
    if (k >= CONTROLLER_AP_BSS_MAX) {
        if (strcmp(d[CONTROLLER_AP_BSS_MAX - 1].bssid, bssid) <= 0)
            return;
        k = CONTROLLER_AP_BSS_MAX - 1;
    }
    while (k > 0 && strcmp(d[k - 1].bssid, bssid) > 0) {
        d[k] = d[k - 1];
        k--;
    }
    memcpy(d[k].bssid, bssid, 17);
    d[k].bssid[17] = 0;
    d[k].stations = stations;
    d[k].age = age;
    if (load->listed < CONTROLLER_AP_BSS_MAX)
        load->listed++;
}

int controller_peer_bss_load(const char *mac, time_t max_age, struct peer_bss_load *load)
{
    DIR *dir;
    struct dirent *entry;
    time_t now = time(NULL);

    if (!mac || !load)
        return 0;
    memset(load, 0, sizeof(*load));
    dir = opendir(peers_dir());
    if (!dir)
        return 0;
    while ((entry = readdir(dir))) {
        time_t when;
        int stations;
        if (!is_bss_entry_name(entry->d_name, mac))
            continue;
        stations = read_bss_entry(entry->d_name, &when);
        if (stations < 0)
            continue;
        if (max_age && now - when > max_age)
            continue;
        load->stations += (unsigned int)stations;
        load->entries++;
        if (load->entries == 1 || now - when > load->age)
            load->age = now - when;
        put_detail(load, entry->d_name, (unsigned int)stations, now - when);
    }
    (void)closedir(dir);
    return load->entries ? 1 : 0;
}
