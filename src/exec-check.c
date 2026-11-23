/* Run loader-only checks without the shell's misleading ENOEXEC fallback. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    int error;
    if (argc < 2) {
        fprintf(stderr, "usage: exec-check EXECUTABLE [ARGS...]\n");
        return 64;
    }
    /* A bounded loader/probe run, even when the device has no timeout applet. */
    alarm(10);
    execve(argv[1], argv + 1, environ);
    error = errno;
    fprintf(stderr, "ZTE_MESH_EXEC execve_errno=%d error=%s\n", error, strerror(error));
    return error == ENOEXEC ? 126 : 127;
}
