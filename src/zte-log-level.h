/* One switch for the lines that are only useful while chasing a fault.
 *
 * The device log ring holds about a thousand lines, and a healthy mesh asks every child
 * for its station counts every ten seconds forever. At info those successes are not news;
 * they only push the user's own Wi-Fi, DHCP and LuCI lines out of the ring. The same goes
 * for every received 1905 frame. So the success path of the poll loops is a debug line:
 * set `ZTE_MESH_LOG_LEVEL=debug` (uci `zte_mesh.main.log_level='debug'`, the init script
 * exports it) to see them, and leave it unset on a normal installation.
 *
 * Failures are not behind this switch: a query that got no answer, a push that returned
 * an error and every state change still reach info.
 */
#ifndef ZTE_LOG_LEVEL_H
#define ZTE_LOG_LEVEL_H

#include <stdlib.h>
#include <string.h>

/* Read once per process: the level does not change while a daemon runs, and these lines
 * sit on paths that are taken every few seconds. */
static inline int zte_log_debug(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *level = getenv("ZTE_MESH_LOG_LEVEL");
        cached = level && (!strcmp(level, "debug") || !strcmp(level, "7") || !strcmp(level, "8"));
    }
    return cached;
}

#endif
