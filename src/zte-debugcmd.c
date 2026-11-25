/* Client for the factory daemon's out-of-band debug channel, the same datagram the
 * factory `sendcmd` tool sends (decoded from its SendDebugMsg/SendToApp): an AF_UNIX
 * datagram to /var/tmp/<process>_asy, 0x224 bytes:
 *   +0x00 "sendcmd" (sender name)   +0x24 target name   +0x48 u32 0x11102   +0x4c u16 0x1c4
 *   +0x60 payload: u32 argc, then argc 64-byte NUL-padded argument strings.
 * Runs inside the sandbox root only (the socket lives in the daemon's /var/tmp).
 * usage: zte-debugcmd <app.process.task> [arg...]   e.g. multiapd.map.master steerpolicy ... */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PACKET 0x224
#define PAYLOAD 0x1c4
#define MAX_ARGS 6

int main(int argc, char **argv)
{
    unsigned char packet[PACKET];
    struct sockaddr_un to = {0}, from = {0};
    char *dot, target_copy[64], self_path[108];
    int fd, i, idle = 0;
    uint32_t count;
    uint16_t length = PAYLOAD;
    uint32_t magic = 0x11102;
    if (argc < 3 || argc - 2 > MAX_ARGS || strlen(argv[1]) >= sizeof(target_copy)) {
        fprintf(stderr, "usage: zte-debugcmd <app.process.task> <cmd> [up to 5 args]\n");
        return 64;
    }
    snprintf(target_copy, sizeof(target_copy), "%s", argv[1]);
    dot = strrchr(target_copy, '.');
    if (!dot)
        return 64;
    *dot = 0; /* process = everything before the last dot, like sendcmd */
    memset(packet, 0, sizeof(packet));
    memcpy(packet, "zte-debugcmd", 12); /* reply address: the daemon answers to /var/tmp/<this name> */
    snprintf((char *)packet + 0x24, 0x24, "%s", argv[1]);
    memcpy(packet + 0x48, &magic, 4);
    memcpy(packet + 0x4c, &length, 2);
    count = (uint32_t)(argc - 2);
    memcpy(packet + 0x60, &count, 4);
    for (i = 0; i < argc - 2; i++) {
        if (strlen(argv[i + 2]) >= 64)
            return 64;
        strcpy((char *)packet + 0x64 + i * 64, argv[i + 2]);
    }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return 2;
    from.sun_family = AF_UNIX;
    snprintf(self_path, sizeof(self_path), "/var/tmp/zte-debugcmd");
    snprintf(from.sun_path, sizeof(from.sun_path), "%s", self_path);
    (void)unlink(self_path);
    if (bind(fd, (struct sockaddr *)&from, sizeof(from)))
        return 3;
    to.sun_family = AF_UNIX;
    snprintf(to.sun_path, sizeof(to.sun_path), "/var/tmp/%s_asy", target_copy);
    if (sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        perror("sendto");
        (void)unlink(self_path);
        return 4;
    }
    while (idle < 2) { /* print every reply datagram until the daemon goes quiet */
        fd_set set;
        struct timeval wait = {1, 0};
        unsigned char reply[2048];
        ssize_t n;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, NULL, NULL, &wait) <= 0) {
            idle++;
            continue;
        }
        n = recv(fd, reply, sizeof(reply), 0);
        if (n <= 0)
            break;
        idle = 0;
        for (i = 0; i < n; i++)
            if ((reply[i] >= 0x20 && reply[i] < 0x7f) || reply[i] == '\n' || reply[i] == '\t')
                putchar(reply[i]);
        fflush(stdout);
    }
    (void)unlink(self_path);
    putchar('\n');
    return 0;
}
