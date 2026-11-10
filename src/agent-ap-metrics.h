#ifndef ZTE_AGENT_AP_METRICS_H
#define ZTE_AGENT_AP_METRICS_H
#include <stddef.h>
#include <stdint.h>

/* The station-count stand-in of the child (agent) role.
 *
 * A master asks for station counts with an AP Metric Query (CMDU 0x800b, TLV 0x93) and shows the numbers of the
 * answer (CMDU 0x800c, one AP Metrics TLV 0x94 per BSS) on its topology page. The vendor agent in this sandbox
 * receives the query and answers nothing: its 0x94 is filled from cspd and the driver layer, and neither the
 * feeder nor /sys exists in the private root - ns-run gives the daemon one bind-mounted directory and
 * CAP_NET_RAW. So the answer cannot come out of the daemon. It comes from the counts measured outside, which
 * openwrt/zte-mesh/files/sta-collect writes to /run/zte-mesh/ap-sta on the child host, and from this unit, which
 * turns a row of that table into the 0x94 for the BSSID the master named.
 *
 * No socket here: the caller - the daemon's receive hook in libzte_controller_trace.so - sends the bytes back on
 * the interface the query arrived on. The table is a file and the answer is a frame, so both are checkable on a
 * host without a namespace. ZTE_MESH_AP_STA_FILE points the table somewhere else, which is how a test hands over
 * a table of its own.
 */

/* The answer to one received frame, or 0 to stay silent as the vendor agent does: not a query, no table, a table
 * nobody refreshed in the last two collector intervals, or no BSSID in the query that this node reports.
 *
 * `reply` gets the 0x800c and `dest` the MAC to send it to - the query's own source, which is how the answer
 * reaches whoever asked rather than a group address. The reply's source is the query's destination, this node's AL
 * MAC: that field is the key the master's receive hook files the station counts under, so a reply from any other
 * MAC lands on the wrong node.
 *
 * A BSSID the query names and the table does not is left out of the answer entirely, not reported as zero - the
 * master's page distinguishes "no data" from "nobody associated", and a zero would erase that. */
size_t agent_ap_metrics_answer(const uint8_t *frame, size_t bytes, uint8_t *reply, size_t capacity, uint8_t dest[6]);

#endif
