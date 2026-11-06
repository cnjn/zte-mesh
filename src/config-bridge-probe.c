/* Native AArch64 one-shot PIPE integration driver; public fixtures ONLY.
 * This is not an installed daemon, credential CLI, UCI reader or Controller.
 */
#define _GNU_SOURCE
#include "mesh-bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int scenario(const char *root, const char *launcher, unsigned int test)
{
    struct mb_profile p = { .request_id = test + 1, .security = MB_WPA2_PSK_AES, .enabled = 1 };
    struct mb_result result;
    uint8_t wire[MB_REQUEST_SIZE + 1], response[MB_RESPONSE_SIZE];
    int input[2], output[2], status, ok = 0;
    size_t length = MB_REQUEST_SIZE;
    unsigned int expected = MB_OK;
    pid_t child, waited;
    strcpy(p.ssid, "zte-bridge-public-lab");
    strcpy(p.key, "zte-bridge-not-a-real-key");
    p.ssid_length = strlen(p.ssid); p.key_length = strlen(p.key);
    p.radio = test == 1 ? 1 : 0;
    p.enabled = test == 1 ? 0 : 1;
    memset(wire, 0, sizeof(wire));
    if (mb_encode_request(&p, wire, MB_REQUEST_SIZE))
        return -1;
    switch (test) {
    case 0: case 1: break;
    case 2: wire[0] ^= 1; expected = MB_BAD_FRAME; break;
    case 3: wire[5] = 2; expected = MB_BAD_FRAME; break;
    case 4: wire[11]--; expected = MB_BAD_FRAME; break;
    case 5: wire[7] = 2; expected = MB_BAD_FRAME; break;
    case 6: wire[22] = 1; expected = MB_BAD_FRAME; break;
    case 7: wire[18] = 9; expected = MB_BAD_CONFIG; break;
    case 8: wire[16] = 2; expected = MB_BAD_CONFIG; break;
    case 9: wire[21] = 7; expected = MB_BAD_CONFIG; break;
    case 10: wire[20] = 32; expected = MB_BAD_CONFIG; break;
    case 11: wire[119] = 1; expected = MB_BAD_CONFIG; break;
    case 12: length--; expected = MB_TRANSPORT_ERROR; break;
    case 13: length++; expected = MB_TRANSPORT_ERROR; break;
    case 14: length = 0; expected = MB_TRANSPORT_ERROR; break;
    case 15: expected = MB_DB_ERROR; break;
    default: return -1;
    }
    if (pipe2(input, O_CLOEXEC) || pipe2(output, O_CLOEXEC))
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0)
            _exit(82);
        if (input[0] > 2) close(input[0]);
        if (input[1] > 2) close(input[1]);
        if (output[0] > 2) close(output[0]);
        if (output[1] > 2) close(output[1]);
        execl(launcher, launcher, root, "/lib/ld-linux.so.3", "--library-path", "/lib",
              "/bin/db-engine-probe", test == 15 ? "--bridge-rollback-test" : "--bridge-test", (char *)NULL);
        _exit(82);
    }
    close(input[0]); close(output[1]);
    /* Only the trailing-byte negative case intentionally exceeds wire size. */
    if (length && mb_write_message(input[1], wire, length > MB_REQUEST_SIZE ? MB_REQUEST_SIZE : length, 3000))
        goto cleanup;
    if (length > MB_REQUEST_SIZE && write(input[1], wire + MB_REQUEST_SIZE, 1) != 1)
        goto cleanup;
    close(input[1]); input[1] = -1;
    if (mb_read_message(output[0], response, sizeof(response), 20000) ||
        mb_decode_response(response, sizeof(response), &result))
        goto cleanup;
    if (result.status != expected ||
        (expected == MB_OK && (result.request_id != p.request_id || result.generation != 1 ||
                              result.flags != (MB_READBACK | MB_OTHER_ROW))) ||
        (expected != MB_OK && result.generation) ||
        (test == 15 && result.flags != MB_ROLLED_BACK))
        goto cleanup;
    ok = 1;
cleanup:
    if (input[1] >= 0)
        close(input[1]);
    close(output[0]);
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status))
        ok = 0;
    mb_wipe(&p, sizeof(p)); mb_wipe(wire, sizeof(wire)); mb_wipe(response, sizeof(response));
    if (!ok)
        fprintf(stderr, "ZTE_CONFIG_BRIDGE fail=scenario number=%u\n", test);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    char root[PATH_MAX], launcher[PATH_MAX];
    struct rlimit core = {0, 0};
    unsigned int i;
    size_t length;
    if (argc != 2 || !realpath(argv[1], root) || strncmp(root, "/tmp/zte-mesh-db.", 17) ||
        (length = strlen(root)) < 5 || length + 2 >= sizeof(launcher) || strcmp(root + length - 5, "/root"))
        return 64;
    if (setrlimit(RLIMIT_CORE, &core) || signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 82;
    memcpy(launcher, root, length - 5);
    strcpy(launcher + length - 5, "/ns-run");
    for (i = 0; i < 16; i++)
        if (scenario(root, launcher, i))
            return 83;
    printf("ZTE_CONFIG_BRIDGE PASS scenarios=16 native_arm64_to_arm32=1 typed_wire=1 profile_readback=1 other_row_preserved=1 rejected_before_db=1 rollback_verified=1 payload_logged=0 controller_ready=0\n");
    return 0;
}
