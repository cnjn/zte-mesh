#!/bin/sh
# Host guard on the role decision in the main loop of src/controller-service.c.
#
# That loop cannot be run here: it supervises the factory daemon inside the private sandbox and is only ever built
# into the ARM32 db-engine-probe. What can be checked is the shape of the decision, which is where the defect was.
# `slave` used to be computed once before the loop and never again, so a hot edit of zte_mesh.main.mesh_role wrote
# the factory's role triple, replaced the daemon -- and left the loop branching on the role this process started
# with. A router switched to slave went on running controller features against agents it no longer had and never
# polled the M2 rows into a Wi-Fi plan; switched back it did the opposite.
#
# So this pins the order (recompute after the config is applied, before the branch that reads it) and the two
# one-shots that have to restart with the role. The behaviour itself is an on-machine item: docs/PLAN.md section 5.
#
#   sh tests/test_service_role.sh [SOURCE]
SRC=${1:-$(dirname "$0")/../src/controller-service.c}
[ -r "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }

pass=0
fail=0
check() { # NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else
		echo "FAIL $1: expected '$2', got '$3'"
		fail=$((fail + 1))
	fi
}
line_of() { grep -nF -- "$1" "$SRC" | head -n 1 | cut -d: -f1; }
has() { [ -n "$(line_of "$1")" ] && echo yes || echo no; }

# The decision is read back from the config the loop just applied, not from a variable set before the loop.
check "the loop recomputes the role from the config" yes \
	"$(has 'int now_slave = config.role_set && config.role == CONTROLLER_ROLE_SLAVE;')"

# Order is the whole point: a recompute before apply_new_config would read the previous pass's role, and one after
# the branch would leave that pass running the wrong half.
APPLY=$(line_of 'apply_new_config(&config, &stamp, &state);')
RECOMPUTE=$(line_of 'int now_slave = config.role_set')
BRANCH=$(line_of 'if (!slave) {')
check "the config is applied before the role is read" yes "$([ -n "$APPLY" ] && [ -n "$RECOMPUTE" ] && [ "$APPLY" -lt "$RECOMPUTE" ] && echo yes || echo no)"
check "the role is read before the branch that uses it" yes "$([ -n "$RECOMPUTE" ] && [ -n "$BRANCH" ] && [ "$RECOMPUTE" -lt "$BRANCH" ] && echo yes || echo no)"

# A change restarts the role's own startup behaviour. The election trigger is what starts EasyMesh on a child, so
# a child that kept a spent attempt counter would never join; and both gates are measured from the daemon's start,
# so a switch an hour into the run must not fire them against an acd task that is not up yet.
BLOCK=$(awk '/if \(now_slave != slave\) \{/,/^        \}/' "$SRC")
for needle in 'slave = now_slave;' 'renew_sent = 0;' 'election_attempts = 0;' 'role_started = now.tv_sec;'; do
	case "$BLOCK" in *"$needle"*) pass=$((pass + 1)) ;; *) fail=$((fail + 1)); echo "FAIL role change does not reset: $needle" ;; esac
done

# The gates follow the role's clock, not the service's. A cold start is unchanged: role_started begins at state.started.
check "the renew gate uses the role's clock" yes "$(has 'if (!renew_sent && now.tv_sec - role_started >= 5) {')"
check "the election gate uses the role's clock" yes \
	"$(has 'if (slave && election_attempts < 3 && now.tv_sec - role_started >= 8 + election_attempts * 12) {')"
check "a cold start keeps the service's own clock" yes "$(has 'long role_started = state.started;')"
check "no gate is left on the service clock" 0 "$(grep -c 'now.tv_sec - state.started' "$SRC")"

echo "service role test: $pass passed, $fail failed"
[ "$fail" = 0 ]