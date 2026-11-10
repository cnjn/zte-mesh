#ifndef ZTE_CONTROLLER_PEER_H
#define ZTE_CONTROLLER_PEER_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "controller-1905.h" /* CONTROLLER_AP_BSS_MAX, the same cap the writer will not exceed */

/* What one received 1905 frame leaves on disk for the topology page.
 *
 * The controller's database does not hold the indicator light, the number of associated clients, how many BSS a
 * child reports as up, or whether it reached us over a direct link or a relay. All of it rides in frames the daemon
 * receives anyway: a HigherLayerResponse carries the light in vendor sub-TLV 0x21 {AL MAC[6], status} (the master
 * reads it in _topoLightStatusHandle), an AP Metrics Response lists the station count of each BSS, and a Topology
 * Response lists the child's BSS interfaces. The software version rides the AP Capability Report, in the vendor's
 * own TLV 0xd4; the master's daemon extracts it into its own device table and prints it on its CLI, but never
 * writes it into the peer rows, so a page that reads the database alone shows it empty. The answers to a client
 * action - a BTM report (0x8015) and a Steering Completed (0x8017) - name the *client*, and are filed under that
 * MAC rather than under the agent that sent them, because that is the key the action table uses.
 *
 * The latest values go to `<peers dir>/<MAC>.<kind>`, written atomically, and the service reads them for state.json.
 * A station count is the one value written twice over: the digest `<MAC>.metrics` holds the total of the last frame
 * that carried one, and each BSS of that frame also gets its own `<MAC>.<BSSID>.sta`, which is the only form that
 * survives a child answering per radio - the service prefers the sum of those files.
 * The directory is /run/zte-mesh/peers unless ZTE_MESH_PEERS_DIR says otherwise - that is the one thing here a host
 * test can point at a temporary directory, which is what makes "two children at once" checkable without two radios.
 *
 * Decoding lives in controller-1905.c and controller-steer.c; this file only decides what to write and writes it.
 */

/* The number of files this frame produced - counting the per-BSS station files too, so a frame that reports three
 * BSSes is four. 0 = the frame says nothing the page shows (a fragment, another vendor's frame, the daemon's status
 * chatter, or a MAC beyond the flood budget below). A complete, unfragmented 1905 CMDU is the only thing looked at.
 *
 * The budget is per process, i.e. per daemon lifetime: anybody on the LAN can send a 1905 frame with any AL MAC, so
 * at most 32 distinct MACs are recorded. Without that a forged flood could fill the tmpfs with digest files. */
int controller_peer_note(const uint8_t *frame, size_t bytes);

/* One BSS of one node, as its own file names it: the BSSID in the spelling the file name uses, the count of the
 * last frame that named it, and how many seconds ago that frame is. */
struct peer_bss_entry {
    char bssid[18];
    unsigned int stations;
    long age;
};

/* What the per-BSS files of one node add up to. `stations` is the sum over the live entries, `entries` how many
 * BSS reported, `age` the oldest entry still counted - the number's weakest link, which is what the page shows as
 * the freshness of a total assembled from several frames. `detail`, in BSSID order, is those entries themselves,
 * so the page can say which radio is busy and not only how many clients there are in total: `listed` is how many
 * of the live entries fit there, and a node with more BSSes than CONTROLLER_AP_BSS_MAX still keeps the full sum. */
struct peer_bss_load {
    unsigned int stations;
    unsigned int entries;
    long age;
    /* The live entries themselves, in BSSID order, so the same directory reads out the same JSON every second.
     * `entries` is how many were live and `listed` how many of those are here: a node with more BSSes than
     * CONTROLLER_AP_BSS_MAX keeps its full total and shows fewer rows, which is the only way the sum below stays
     * the sum the page prints. */
    unsigned int listed;
    struct peer_bss_entry detail[CONTROLLER_AP_BSS_MAX];
};

/* 1 = at least one `<MAC>.<BSSID>.sta` is there and younger than max_age seconds (0 = no limit), 0 = this node has
 * no per-BSS entries at all, which is not the same as its stations being zero.
 *
 * A frame that lists its BSSes lets the reader count them one by one and keep each one's own age: the digest's
 * single "stations=" line only ever holds the last frame that arrived, so a child that answers one frame per radio
 * would alternate between its radios' counts instead of showing both. */
int controller_peer_bss_load(const char *mac, time_t max_age, struct peer_bss_load *load);

#endif
