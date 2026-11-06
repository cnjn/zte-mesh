/* Probe: how does qemu-user handle shmat() address requests?
 *
 * The factory DB engine attaches its segment at a fixed guest address and dies on
 * the returned pointer when the attach fails (docs/research/AGENT_PROBE.md). This
 * probe isolates the qemu behaviour so a workaround can be chosen deliberately.
 *
 *   arm-linux-gnueabi-gcc -march=armv7-a -mfloat-abi=soft -static shm-fixed.c -o shm-fixed
 *   qemu-arm-static ./shm-fixed
 */
#include <errno.h>
#include <stdio.h>
#include <sys/shm.h>

static void attempt(const char *label, void *wanted, int flags)
{
    int id = shmget(0x7a11, 65536, IPC_CREAT | 0600);
    if (id < 0) {
        printf("%-22s shmget failed errno=%d\n", label, errno);
        return;
    }
    errno = 0;
    void *got = shmat(id, wanted, flags);
    printf("%-22s wanted=%p got=%p errno=%d\n", label, wanted, got, errno);
    if (got != (void *)-1)
        shmdt(got);
    shmctl(id, IPC_RMID, NULL);
}

int main(void)
{
    attempt("NULL", NULL, 0);
    attempt("fixed 0x60000000", (void *)0x60000000UL, 0);
    attempt("fixed | SHM_RND", (void *)0x60000000UL, SHM_RND);
    attempt("fixed | SHM_REMAP", (void *)0x60000000UL, SHM_REMAP);
    attempt("fixed 0x50000000", (void *)0x50000000UL, 0);
    return 0;
}
