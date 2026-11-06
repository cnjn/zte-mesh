#!/bin/sh
# Build the lab artifacts on a non-Debian host.
#
# The audited build runs in debian:bookworm-slim (see Dockerfile), where gcc does
# not define _FORTIFY_SOURCE. Ubuntu's gcc enables it by default, which turns
# "ignored return value of write()" into an error under -Werror and changes code
# generation of memcpy/memset calls in ways the ARM32 ABI audit never saw.
# Disabling it explicitly keeps the Ubuntu build equivalent to the audited one
# instead of patching every call site for a host-only concern.
#
#   sh tools/lab/build.sh                  # all database
#   sh tools/lab/build.sh test-host        # host tests
#   sh tools/lab/build.sh controller       # controller shims + layout evidence
set -e
cd "$(dirname "$0")/../.."

HOSTCC=${HOSTCC:-cc}
ARMCC=${ARMCC:-arm-linux-gnueabi-gcc}
ROUTERCC=${ROUTERCC:-aarch64-linux-gnu-gcc}
NO_FORTIFY="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"

exec make -j"$(nproc)" \
	HOSTCC="$HOSTCC $NO_FORTIFY" \
	ARMCC="$ARMCC $NO_FORTIFY" \
	ROUTERCC="$ROUTERCC $NO_FORTIFY" \
	"$@"
