#!/bin/sh
# Push a handful of edited files to the lab VM with LF line endings.
#
#   sh tools/lab/sync.sh src/controller-packet.h src/controller-packet.c src/controller-wsc.c
#
# tools/lab/push.py needs paramiko; this needs only the OpenSSH client that already
# has a key for the VM. Paths are relative to the repository root and are recreated
# under $LAB_DIR (default ~/zte-mesh), so the VM's out/ tree survives.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LAB_HOST=${LAB_HOST:-10.23.90.131}
LAB_USER=${LAB_USER:?set LAB_USER}
LAB_DIR=${LAB_DIR:-\~/zte-mesh}
[ $# -gt 0 ] || { echo "usage: $0 FILE..." >&2; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

for file in "$@"; do
	mkdir -p "$TMP/$(dirname "$file")"
	# A Windows checkout stores CRLF; the compiler does not care, the shell scripts do.
	tr -d '\r' < "$REPO/$file" > "$TMP/$file"
done

tar -C "$TMP" -cf - . | ssh -o BatchMode=yes "$LAB_USER@$LAB_HOST" \
	"tar -C $LAB_DIR -xf - && echo synced: $*"
