/* One switch for the lines that are only useful while chasing a fault.
 *
 * The device log ring holds about a thousand lines, and a healthy mesh asks every child
 * for its station counts every ten seconds forever. At info those successes are not news;
 * they only push the user's own Wi-Fi, DHCP and LuCI lines out of the ring. The same goes
 * for every received 1905 frame. So the success path of the poll loops is a debug line:
 * switch it on from the page (uci `zte_mesh.main.log_level='debug'`) and leave it at info
 * on a normal installation.
 *
 * The switch is a flag file in the exchange directory, not an environment variable: ns-run
 * clears the environment before the daemon starts, so a variable the init script exports
 * never reaches the daemon or the preload libraries (libzte_controller_trace.so reads
 * trace-all the same way). The environment variable is still honoured when no flag file
 * exists, which is how a lab run that does not go through the init script turns this on.
 *
 * The file is re-read at most once a second: a syscall per line would be disproportionate
 * for lines that sit on a ten-second path, and one second is short enough that a switch
 * saved on the page is in effect by the time the reader opens the log.
 *
 * Failures are not behind this switch: a query that got no answer, a push that returned
 * an error and every state change still reach info.
 */
#ifndef ZTE_LOG_LEVEL_H
#define ZTE_LOG_LEVEL_H

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ZTE_LOG_FLAG "/run/zte-mesh/log-debug"

static inline int zte_log_debug(void)
{
    static int cached = -1;
    static time_t checked;
    time_t now = time(NULL);

    if (cached < 0 || now != checked) {
        const char *level;

        checked = now;
        if (access(ZTE_LOG_FLAG, F_OK) == 0) {
            cached = 1;
        } else {
            level = getenv("ZTE_MESH_LOG_LEVEL");
            cached = level && (!strcmp(level, "debug") || !strcmp(level, "7") || !strcmp(level, "8"));
        }
    }
    return cached;
}

#endif
