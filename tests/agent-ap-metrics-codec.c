/* Host tests for the child's station-count stand-in.
 *
 * The claim is the one combinations ② and ③ have to satisfy: a master that asks for station counts gets an answer
 * from an OpenWrt child whose sandboxed vendor agent cannot produce one. So the pieces are checked as the master
 * will meet them - a query recognised, the counts of the host-side table turned into one AP Metrics TLV per BSSID
 * named, a table nobody refreshed answered with nothing, and the whole reply put back through the master's own
 * writer (controller_peer_note) to prove the files behind the page's terminal column come out of it.
 *
 * The frame builders are the shapes tests/controller-1905-codec.c and tests/controller-peer-codec.c already use,
 * and the table is the text openwrt/zte-mesh/files/sta-collect writes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../src/agent-ap-metrics.h"
#include "../src/controller-1905.h"
#include "../src/controller-peer.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "ap metrics stand-in failure line %d\n", __LINE__); exit(1); } } while (0)

static const uint8_t master[6] = {2, 0, 0, 0, 0x19, 0x03};
static const uint8_t child[6] = {2, 0, 0, 0, 0x19, 0xe3};
static const uint8_t group[6] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};
static const uint8_t bss_2g[6] = {2, 0, 0, 0, 0x19, 0xb1};
static const uint8_t bss_5g[6] = {2, 0, 0, 0, 0x19, 0xb2};
static const uint8_t bss_other[6] = {2, 0, 0, 0, 0x19, 0xb9};

static char root[256];
static char table[300];
static char peers[300];

static void write_table(const char *text)
{
    FILE *f = fopen(table, "w");
    if (!f) {
        fprintf(stderr, "ap metrics stand-in: cannot write the table\n");
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

/* A table of one fresh epoch line and these rows: "<bssid> <count>" is what the collector writes, so the fixtures
 * are its own output shape. */
static void table_of(char *out, size_t room, long age, const uint8_t *bssid, const unsigned int *counts,
                     unsigned int rows)
{
    size_t used;
    unsigned int k;
    used = (size_t)snprintf(out, room, "epoch=%ld\n", (long)time(NULL) - age);
    for (k = 0; k < rows && used < room - 22; k++)
        used += (size_t)snprintf(out + used, room - used, "%02x:%02x:%02x:%02x:%02x:%02x %u\n",
                                 bssid[k * 6], bssid[k * 6 + 1], bssid[k * 6 + 2], bssid[k * 6 + 3],
                                 bssid[k * 6 + 4], bssid[k * 6 + 5], counts[k]);
}

/* AP Metric Query (CMDU 0x800b) with TLV 0x93 = {count, BSSID[count]}, addressed to `dest`. `fragment` and `flags`
 * are arguments because the header conditions the responder checks are ones a master splitting a long list would
 * set differently; `bad_length` makes the TLV length disagree with its own count byte. */
static size_t query(uint8_t *frame, const uint8_t dest[6], const uint8_t source[6], unsigned int mid,
                    unsigned int fragment, uint8_t flags, const uint8_t *bssids, unsigned int count,
                    int bad_length)
{
    size_t at = CONTROLLER_1905_HEADER, length = 1 + 6 * (size_t)count;
    unsigned int k;
    memset(frame, 0, CONTROLLER_1905_MAX_BYTES);
    memcpy(frame, dest, 6);
    memcpy(frame + 6, source, 6);
    frame[12] = 0x89;
    frame[13] = 0x3a;
    frame[16] = (uint8_t)(CONTROLLER_MSG_AP_METRICS_QUERY >> 8);
    frame[17] = (uint8_t)CONTROLLER_MSG_AP_METRICS_QUERY;
    frame[18] = (uint8_t)(mid >> 8);
    frame[19] = (uint8_t)mid;
    frame[20] = (uint8_t)fragment;
    frame[21] = flags;
    frame[at++] = CONTROLLER_TLV_AP_METRICS_QUERY;
    frame[at++] = (uint8_t)(length >> 8);
    frame[at++] = bad_length ? (uint8_t)(length + 5) : (uint8_t)length;
    frame[at++] = (uint8_t)count;
    for (k = 0; k < count; k++) {
        memcpy(frame + at, bssids + 6 * (size_t)k, 6);
        at += 6;
    }
    frame[at++] = 0;
    frame[at++] = 0;
    frame[at++] = 0;
    if (at < 60)
        at = 60; /* the raw socket does not pad an Ethernet frame */
    return at;
}

/* The complete, unfragmented query to the child that answers_the_table() is built around. */
static size_t ask(uint8_t *frame, const uint8_t *bssids, unsigned int count)
{
    return query(frame, child, master, 0x4100, 0, 0x80, bssids, count, 0);
}

static void text_of(const char *path, char *out, size_t room)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;
    out[0] = 0;
    if (!f)
        return;
    n = fread(out, 1, room - 1, f);
    fclose(f);
    out[n] = 0;
}

static void mac_text(char *out, size_t room, const uint8_t *mac)
{
    snprintf(out, room, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void query_decode(void)
{
    uint8_t frame[CONTROLLER_1905_MAX_BYTES + 8], two[12], many[(CONTROLLER_AP_BSS_MAX + 1) * 6];
    struct cmdu_header header;
    uint8_t asked[CONTROLLER_AP_BSS_MAX][6];
    unsigned int count, k;

    memcpy(two, bss_2g, 6);
    memcpy(two + 6, bss_5g, 6);

    CHECK(ask(frame, two, 2) == 60);
    CHECK(ap_metrics_query_decode(frame, 60, &header, asked, &count));
    CHECK(count == 2 && !memcmp(asked[0], bss_2g, 6) && !memcmp(asked[1], bss_5g, 6));
    CHECK(header.message_id == 0x4100 && header.last_fragment && !header.fragment);

    /* One BSS, and none: an empty list is the master asking about every BSS this node runs. */
    CHECK(ap_metrics_query_decode(frame, ask(frame, two, 1), &header, asked, &count) && count == 1);
    CHECK(ap_metrics_query_decode(frame, ask(frame, two, 0), &header, asked, &count) && count == 0);

    /* A query split over fragments is not a question to answer: the station counts belong to the whole list, and
     * an answer to half of it is the master reading a short list as the whole question. */
    CHECK(query(frame, child, master, 3, 1, 0x80, two, 1, 0) == 60);
    CHECK(!ap_metrics_query_decode(frame, 60, &header, asked, &count));
    frame[20] = 0;
    frame[21] = 0; /* the last fragment bit is what makes a single-frame query complete */
    CHECK(!ap_metrics_query_decode(frame, 60, &header, asked, &count));
    frame[21] = 0xc0; /* last fragment, relayed by a neighbor: still complete for the node it names */
    CHECK(ap_metrics_query_decode(frame, 60, &header, asked, &count) && header.relayed);

    /* Damage: a TLV whose length disagrees with its own count byte, a list longer than this answer would hold, a
     * frame that stops inside the list, a message that is not a query, and a query with no 0x93 in it. */
    CHECK(query(frame, child, master, 4, 0, 0x80, two, 1, 1) == 60);
    CHECK(!ap_metrics_query_decode(frame, 60, &header, asked, &count));
    for (k = 0; k <= CONTROLLER_AP_BSS_MAX; k++)
        memcpy(many + 6 * (size_t)k, bss_2g, 6);
    CHECK(query(frame, child, master, 5, 0, 0x80, many, CONTROLLER_AP_BSS_MAX + 1, 0) == 131);
    CHECK(!ap_metrics_query_decode(frame, 131, &header, asked, &count));
    CHECK(query(frame, child, master, 6, 0, 0x80, two, 1, 0) == 60);
    CHECK(!ap_metrics_query_decode(frame, 28, &header, asked, &count));
    frame[17] = (uint8_t)(CONTROLLER_MSG_AP_METRICS_RESPONSE & 0xff);
    CHECK(!ap_metrics_query_decode(frame, 60, &header, asked, &count));
    frame[17] = (uint8_t)(CONTROLLER_MSG_AP_METRICS_QUERY & 0xff);
    frame[22] = CONTROLLER_TLV_AL_MAC;
    CHECK(!ap_metrics_query_decode(frame, 60, &header, asked, &count));
    CHECK(!ap_metrics_query_decode(frame, 21, &header, asked, &count)); /* not even a header */
}

static void response_build(void)
{
    struct ap_bss_metric bsses[3], read_back[CONTROLLER_AP_BSS_MAX];
    uint8_t frame[CONTROLLER_1905_MAX_BYTES];
    size_t used;

    memcpy(bsses[0].bssid, bss_2g, 6);
    bsses[0].stations = 3;
    memcpy(bsses[1].bssid, bss_5g, 6);
    bsses[1].stations = 65535;
    memcpy(bsses[2].bssid, bss_other, 6);
    bsses[2].stations = 0;

    used = ap_metrics_response_build(frame, sizeof(frame), master, child, 0x4100, bsses, 2);
    CHECK(used == 60);
    CHECK(!memcmp(frame, master, 6) && !memcmp(frame + 6, child, 6));
    CHECK(frame[12] == 0x89 && frame[13] == 0x3a);
    CHECK(frame[16] == 0x80 && frame[17] == 0x0c); /* CMDU 0x800c, big-endian in the two header bytes */
    CHECK(frame[18] == 0x41 && frame[19] == 0x00); /* the message id of the query, so the master matches it up */
    CHECK(frame[20] == 0 && frame[21] == 0x80);    /* last fragment, not relayed: it goes over the direct link */
    CHECK(frame[22] == CONTROLLER_TLV_AP_METRICS && frame[23] == 0 && frame[24] == 10);
    CHECK(!memcmp(frame + 25, bss_2g, 6));
    CHECK(frame[31] == 0);           /* channel utilisation: not measured here, so not claimed */
    CHECK(frame[32] == 0 && frame[33] == 3);
    CHECK(frame[34] == 0);           /* presence bitmap: no estimated-service block follows */
    CHECK(frame[35] == CONTROLLER_TLV_AP_METRICS && frame[36] == 0 && frame[37] == 10);
    CHECK(!memcmp(frame + 38, bss_5g, 6));
    CHECK(frame[44] == 0 && frame[45] == 0xff && frame[46] == 0xff && frame[47] == 0);
    CHECK(frame[48] == 0 && frame[49] == 0 && frame[50] == 0); /* End of Message, and nothing after it */

    /* Nothing to say, and nowhere enough room to say it. */
    CHECK(ap_metrics_response_build(frame, sizeof(frame), master, child, 1, bsses, 0) == 0);
    CHECK(ap_metrics_response_build(frame, 59, master, child, 1, bsses, 1) == 0);
    /* A caller that sized the buffer for the TLVs but not for the Ethernet minimum gets a refusal, not a short
     * frame: a raw socket sends what it is given and the master drops a runt as damaged. */
    CHECK(ap_metrics_response_build(frame, 38, master, child, 1, bsses, 1) == 0);
    CHECK(ap_metrics_response_build(frame, 60, master, child, 1, bsses, 1) == 60);
    /* Past the minimum the frame is exactly its own length: three BSSes is 22 + 3 * 13 + 3. */
    CHECK(ap_metrics_response_build(frame, 63, master, child, 1, bsses, 3) == 0);
    CHECK(ap_metrics_response_build(frame, 64, master, child, 1, bsses, 3) == 64);
    CHECK(ap_metrics_by_bss(frame, 64, read_back, CONTROLLER_AP_BSS_MAX) == 3);
    CHECK(read_back[1].stations == 65535 && !memcmp(read_back[1].bssid, bss_5g, 6));
    CHECK(read_back[2].stations == 0 && !memcmp(read_back[2].bssid, bss_other, 6));
    /* A BSS with nobody associated is a zero of its own, in the order the answer listed them. */
    CHECK(frame[61] == 0 && frame[62] == 0 && frame[63] == 0);
}

static void answers_the_table(void)
{
    uint8_t frame[CONTROLLER_1905_MAX_BYTES + 8], reply[512], dest[6];
    uint8_t asked[24], wide[12];
    struct ap_bss_metric out[CONTROLLER_AP_BSS_MAX];
    unsigned int counts[2];
    char text[512], path[400], child_text[18], bss_text[18];
    size_t bytes, used;

    memcpy(asked, bss_2g, 6);
    memcpy(wide, bss_2g, 6);
    memcpy(wide + 6, bss_5g, 6);
    mac_text(child_text, sizeof(child_text), child);

    /* A table that is not the collector's format answers nothing, and so does no table at all. */
    write_table("not a table at all\n");
    CHECK(agent_ap_metrics_answer(frame, ask(frame, asked, 1), reply, sizeof(reply), dest) == 0);
    write_table("epoch=\n02:00:00:00:19:b1 7\n");
    CHECK(agent_ap_metrics_answer(frame, ask(frame, asked, 1), reply, sizeof(reply), dest) == 0);
    CHECK(unlink(table) == 0);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, asked, 1), reply, sizeof(reply), dest) == 0);

    /* The BSSID the master named, with the count the collector measured on it. */
    counts[0] = 7;
    table_of(text, sizeof(text), 1, bss_2g, counts, 1);
    write_table(text);
    bytes = ask(frame, asked, 1);
    used = agent_ap_metrics_answer(frame, bytes, reply, sizeof(reply), dest);
    CHECK(used == 60);
    CHECK(!memcmp(dest, master, 6));
    CHECK(!memcmp(reply, master, 6) && !memcmp(reply + 6, child, 6));
    CHECK(reply[16] == 0x80 && reply[17] == (uint8_t)CONTROLLER_MSG_AP_METRICS_RESPONSE);
    CHECK(reply[18] == 0x41 && reply[19] == 0x00);
    CHECK(ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 1);
    CHECK(out[0].stations == 7 && !memcmp(out[0].bssid, bss_2g, 6));

    /* A BSSID the table does not have is left out rather than reported as zero: the page distinguishes "no data"
     * from "nobody associated", and a zero would erase the difference. A query that names only unknown BSSes gets
     * the same silence the vendor agent produces. */
    memcpy(asked, bss_other, 6);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, asked, 1), reply, sizeof(reply), dest) == 0);
    memcpy(asked, bss_other, 6);
    memcpy(asked + 6, bss_2g, 6);
    used = agent_ap_metrics_answer(frame, ask(frame, asked, 2), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 1);
    CHECK(!memcmp(out[0].bssid, bss_2g, 6));

    /* The same BSSID named twice is one BSS: the master sums the 0x94s of a frame, so a repeated entry would count
     * those clients twice. */
    memcpy(asked, bss_2g, 6);
    memcpy(asked + 6, bss_2g, 6);
    used = agent_ap_metrics_answer(frame, ask(frame, asked, 2), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 1);

    /* An empty list asks about every BSS this node runs. */
    used = agent_ap_metrics_answer(frame, ask(frame, wide, 0), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 1);

    /* Two BSSes, two answers, in the order the query named them - and a BSS with nobody associated is a zero the
     * collector measured, not an omission. */
    counts[0] = 7;
    counts[1] = 0;
    table_of(text, sizeof(text), 1, wide, counts, 2);
    write_table(text);
    memcpy(asked, bss_5g, 6);
    memcpy(asked + 6, bss_2g, 6);
    used = agent_ap_metrics_answer(frame, ask(frame, asked, 2), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 2);
    CHECK(!memcmp(out[0].bssid, bss_5g, 6) && out[0].stations == 0);
    CHECK(!memcmp(out[1].bssid, bss_2g, 6) && out[1].stations == 7);
    used = agent_ap_metrics_answer(frame, ask(frame, wide, 0), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 2);

    /* A table nobody refreshed is not a measurement any more, and neither is one from the future - the clock has
     * not settled since boot. */
    table_of(text, sizeof(text), 31, wide, counts, 2);
    write_table(text);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, wide, 2), reply, sizeof(reply), dest) == 0);
    table_of(text, sizeof(text), -5, wide, counts, 2);
    write_table(text);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, wide, 2), reply, sizeof(reply), dest) == 0);
    table_of(text, sizeof(text), 25, wide, counts, 2);
    write_table(text);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, wide, 2), reply, sizeof(reply), dest) == 60);

    /* A row that is damage is skipped; the rows around it still answer. */
    table_of(text, sizeof(text), 1, wide, counts, 1);
    strcat(text, "zz:zz:zz:zz:zz:zz 1\n02:00:00:00:19:b2 5\n");
    write_table(text);
    used = agent_ap_metrics_answer(frame, ask(frame, wide, 2), reply, sizeof(reply), dest);
    CHECK(used == 60 && ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 2);
    CHECK(out[0].stations == 7 && out[1].stations == 5);
    /* A count wider than the 16-bit field the answer carries is not half a number on the page. */
    table_of(text, sizeof(text), 1, wide, counts, 1);
    strcat(text, "02:00:00:00:19:b2 70000\n");
    write_table(text);
    memcpy(asked, bss_5g, 6);
    CHECK(agent_ap_metrics_answer(frame, ask(frame, asked, 1), reply, sizeof(reply), dest) == 0);

    /* A query sent to a group address is not addressed to this node's AL MAC: answering it would put that address
     * in the reply's source, which is the key the master files the counts under. */
    memcpy(asked, bss_2g, 6);
    bytes = query(frame, group, master, 8, 0, 0x80, asked, 1, 0);
    CHECK(agent_ap_metrics_answer(frame, bytes, reply, sizeof(reply), dest) == 0);

    /* The reply, read back by the master's own writer: the files behind the page's terminal column. */
    counts[0] = 4;
    counts[1] = 2;
    table_of(text, sizeof(text), 1, wide, counts, 2);
    write_table(text);
    bytes = ask(frame, wide, 2);
    used = agent_ap_metrics_answer(frame, bytes, reply, sizeof(reply), dest);
    CHECK(used == 60);
    CHECK(controller_peer_note(reply, used) == 3); /* the digest plus one file per BSS */
    snprintf(path, sizeof(path), "%s/%s.metrics", peers, child_text);
    text_of(path, text, sizeof(text));
    CHECK(!strcmp(text, "stations=6\nhops=1\n"));
    mac_text(bss_text, sizeof(bss_text), bss_2g);
    snprintf(path, sizeof(path), "%s/%s.%s.sta", peers, child_text, bss_text);
    text_of(path, text, sizeof(text));
    CHECK(!strcmp(text, "4\n"));
    mac_text(bss_text, sizeof(bss_text), bss_5g);
    snprintf(path, sizeof(path), "%s/%s.%s.sta", peers, child_text, bss_text);
    text_of(path, text, sizeof(text));
    CHECK(!strcmp(text, "2\n"));

    /* The same exchange with this repository's own builder at the other end. The controller asks with an empty list,
     * so the answer carries every BSS the collector measured, and the writer files each one - which is combination ③
     * (this controller and an OpenWrt child) end to end, missing only the wire between them. */
    counts[0] = 3;
    counts[1] = 1;
    table_of(text, sizeof(text), 1, wide, counts, 2);
    write_table(text);
    bytes = ap_metrics_query_build(frame, sizeof(frame), child, master, 0x4101);
    CHECK(bytes == 60);
    used = agent_ap_metrics_answer(frame, bytes, reply, sizeof(reply), dest);
    CHECK(used == 60);
    CHECK(!memcmp(dest, master, 6));         /* where to send it: back to the MAC that asked */
    CHECK(!memcmp(reply + 6, child, 6));     /* and the child's own AL MAC as the source, which is the key the files use */
    CHECK(ap_metrics_by_bss(reply, used, out, CONTROLLER_AP_BSS_MAX) == 2);
    CHECK(controller_peer_note(reply, used) == 3);
    snprintf(path, sizeof(path), "%s/%s.metrics", peers, child_text);
    text_of(path, text, sizeof(text));
    CHECK(!strcmp(text, "stations=4\nhops=1\n"));
    mac_text(bss_text, sizeof(bss_text), bss_2g);
    snprintf(path, sizeof(path), "%s/%s.%s.sta", peers, child_text, bss_text);
    text_of(path, text, sizeof(text));
    CHECK(!strcmp(text, "3\n"));
}

static void bounded_mutations(void)
{
    uint8_t frame[CONTROLLER_1905_MAX_BYTES + 8], reply[512 + 8], dest[6], asked[6];
    unsigned int counts[1];
    char text[256];
    unsigned int seed = 12345, i, damaged = 0;
    memcpy(asked, bss_2g, 6);
    counts[0] = 7;
    table_of(text, sizeof(text), 1, bss_2g, counts, 1);
    write_table(text);
    /* The responder reads the bytes it is handed and writes into a buffer the caller sizes: flip one bit of a real
     * query, cut it anywhere, and the answer either does not come or fits. */
    for (i = 0; i < 20000; i++) {
        size_t bytes = ask(frame, asked, 1), cut;
        seed = seed * 1664525u + 1013904223u;
        frame[seed % bytes] ^= (uint8_t)(1u << ((seed >> 24) & 7));
        seed = seed * 1664525u + 1013904223u;
        cut = seed % (bytes + 1);
        memset(frame + bytes, 0xa5, 8);
        memset(reply + 512, 0x5a, 8);
        bytes = agent_ap_metrics_answer(frame, cut, reply, 512, dest);
        if (bytes > 512 || reply[512] != 0x5a || frame[60] != 0xa5)
            damaged++;
    }
    CHECK(!damaged);
    printf("agent-ap-metrics-codec: %u assertions; 20000 bounded mutations passed\n", checks);
}

int main(void)
{
    DIR *dir;
    struct dirent *entry;
    snprintf(root, sizeof(root), "/tmp/zte-apsta-%ld-XXXXXX", (long)getpid());
    if (!mkdtemp(root)) {
        fprintf(stderr, "ap metrics stand-in: cannot make a temporary directory\n");
        return 1;
    }
    snprintf(table, sizeof(table), "%s/ap-sta", root);
    snprintf(peers, sizeof(peers), "%s/peers", root);
    if (mkdir(peers, 0755)) {
        fprintf(stderr, "ap metrics stand-in: cannot make the peers directory\n");
        return 1;
    }
    setenv("ZTE_MESH_AP_STA_FILE", table, 1);
    setenv("ZTE_MESH_PEERS_DIR", peers, 1);

    query_decode();
    response_build();
    answers_the_table();
    bounded_mutations();

    dir = opendir(peers);
    while (dir && (entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            char path[sizeof(peers) + sizeof(entry->d_name) + 2];
            snprintf(path, sizeof(path), "%s/%s", peers, entry->d_name);
            (void)unlink(path);
        }
    if (dir)
        closedir(dir);
    (void)unlink(table);
    (void)rmdir(peers);
    (void)rmdir(root);
    return 0;
}
