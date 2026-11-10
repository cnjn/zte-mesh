#!/bin/sh
# Install-and-verify one batch of zte-mesh packages on a lab machine (PLAN C10). Runs on the router under busybox
# ash; copy it to the router and run it there over ssh. Output is a list of `KEY=VALUE` evidence lines and a final
# PASS/FAIL, so one run can be pasted into a review as proof and diffed against the next batch.
#
#   sh verify-install.sh                 # read the machine back and run the three smokes
#   sh verify-install.sh /tmp/zte-apk    # install the apks found there (upgrade in place), restart, then the same
#
# Passing a directory means "activate this batch", so the script restarts rpcd and the service after installing:
# `apk add` only replaces files. Measured on owrt -- after replacing luci-app-zte-mesh the status call still had no
# `local` key (rpcd kept the pre-split plugin) and the attach instance still reported itself running with zmesh0 gone.
# Verifying without the restart would have graded the previous build, not this one.
#
# The three smokes are the three things that can be wrong after an install and that the page does not tell you:
#   1 service     -- the daemon is up, and the role it reports is the role the configuration asks for
#   2 instances   -- exactly the procd instances that role implies are running (no missing applier, no stray one)
#   3 profile     -- what the Wi-Fi applier did last time it ran, or (on a controller that touches no radio) that it
#                    correctly did nothing at all
# and when the run actually restarted the service, a fourth: how many times the applier wrote during that restart
# (a converged child must answer zero -- see `smoke_restart_quiet`). A fifth reads the hop count the daemon itself
# put into the database (`smoke_cur_hop`): the child's startup fixture writes -2 and never overwrites it, so on a
# child any other number is the vendor daemon's own update -- but only once the daemon has had the chance to learn it,
# so a witness younger than CUR_HOP_GRACE seconds is reported without being judged. A sixth checks that the one lab
# override of that column -- `MultiAPCfg 0 CurHop <n>` in /run/zte-mesh/db-profile, see PLAN's H-14 ⑦ -- is what the
# controller's own database row now carries (`smoke_cur_hop_override`); a machine with no such file judges nothing.
#
# After the smokes the script reads the machine back: the two strings that say this batch's writer is the one that is
# installed (`packed_copy`), the four that say the batch's shell files are (`package_scripts` -- a batch that changes
# only the init script and the applier leaves nothing in the rootfs), the three that say this machine has a launcher it
# can exec (`launcher_evidence` -- the package is architecture-independent now and the install-time board gate is gone,
# so the answer moved here), one more there that only reports whether the separate zte-mesh-launcher package is still
# on the box, the collector's own table (`ap_sta_evidence`), the
# peer files it has produced (`peer_files`) and whether the state document carries the per-BSS key (`state_nodes`).
# Those are evidence lines, and only the nine markers are counted as assertions -- an empty peer directory on a link
# whose peer reports no station count is not a fault of ours.
# Two more pairs turn PLAN's on-machine questions into lines you can read twice. `smoke_mesh_compat` judges the page
# option against the key the daemon reads (they answer opposite questions), and `smoke_backhaul` judges the four uci
# lines this package writes for a wireless backhaul while reporting what wpad and the radio made of them.
# `peer_evidence` counts the peer files and the per-BSS rows so H-12 and H-13 have a number beside the condition that
# makes it mean something, and `nodes_rows`/`nodes_online` are the pair to read with the child's cable in and out.
set -u

APK_DIR=${1:-}
UBUS=${UBUS:-ubus}
APK=${APK:-apk}
UCI=${UCI:-uci}
IW=${IW:-iw}
IP=${IP:-ip}
LOGREAD=${LOGREAD:-logread}
DATE=${DATE:-date}
INIT_D=${INIT_D:-/etc/init.d}
RESTART_WAIT=${RESTART_WAIT:-12}
# set RESTART_NOW=1 to ask for a restart even though nothing was installed. That is the drive for PLAN's H-14: the
# question "does a restart of a converged child write anything" cannot be answered by reading a machine that has
# already settled -- and on a real device this drops every Wi-Fi client for a few seconds.
RESTART_NOW=${RESTART_NOW:-0}
# a child's hop count lands about 27 s after the daemon starts (measured above); a witness younger than this is the
# daemon not having been told yet, so the number is reported and not judged
CUR_HOP_GRACE=${CUR_HOP_GRACE:-120}
# how long the Wi-Fi applier gets to land the plan before its result is graded (section 3 of INSTALL runs this
# right after an install, when the child is still waiting for the controller's profile)
SSID_WAIT=${SSID_WAIT:-60}
SSID_POLL=${SSID_POLL:-3}
# The peer records are rewritten by every frame that arrives, so one snapshot of the directory is one instant: the
# metrics line is the last frame's, and a frame that carries no station count erases it (measured 2026-11-05, eight
# reads on the lab link caught one `peer_reports_stations=no` while `peers_sta=2`). So re-sample -- but only while a
# pass shows no number anywhere, which is the "did this machine get any input at all" question. In tests the fixture
# never changes, so METRICS_READS=1 keeps a silent link from sleeping.
METRICS_READS=${METRICS_READS:-3}
METRICS_READ_GAP=${METRICS_READ_GAP:-5}
RUNDIR=${ZTE_MESH_RUNDIR:-/run/zte-mesh}
# the private root the runtime tree is shipped in (openwrt/zte-mesh/Makefile copies `root/` into the package itself,
# so `apk del` removes it again -- what survives is only the qemu-arm-static a machine needed and someone put here)
ROOTFS=${ZTE_MESH_ROOTFS:-/usr/lib/zte-mesh}
LIBEXEC=${ZTE_MESH_LIBEXEC:-/usr/libexec/zte-mesh}
PEERS=$RUNDIR/peers
TABLE=$RUNDIR/ap-sta
STATE=$RUNDIR/state.json
PLAN=$RUNDIR/wlan-plan
SYNC_STATUS=$RUNDIR/wlan-sync.status
PKG_STEMS='zte-mesh luci-app-zte-mesh luci-i18n-zte-mesh-zh-cn'

pass=0
fail=0
installed_any=
did_restart=
BEFORE_WRITES=
BEFORE_AIR=
S_ROLE=''
S_LOCAL=''
S_RUNNING=''
S_INSTALLED=''
S_ATTACHED=''

say() { printf '%s=%s\n' "$1" "$2"; }
check() {
	# check LABEL EXPECTED ACTUAL -- a counted assertion whose line is also the evidence
	if [ "$2" = "$3" ]; then
		pass=$((pass + 1))
		say "$1" "$3"
	else
		fail=$((fail + 1))
		printf 'FAIL %s expected=%s got=%s\n' "$1" "$2" "$3"
	fi
}

# --- install -------------------------------------------------------------------------------------------------------
# One `apk add` naming a package this machine cannot take does not half-succeed, it refuses the whole transaction
# (measured on the x86_64 VM, which never accepts the aarch64 launcher). So each file is its own call and a file that
# is simply absent is a skip, not a failure.
install_apks() {
	[ -n "$APK_DIR" ] || { say install skipped; return 0; }
	[ -d "$APK_DIR" ] || { fail=$((fail + 1)); printf 'FAIL apk_dir dir=%s\n' "$APK_DIR"; return 1; }
	for stem in $PKG_STEMS; do
		# [0-9] after the stem: an earlier batch left zte-mesh-launcher-*.apk in this same directory, and
		# `zte-mesh-*.apk` would pick it up as this batch's zte-mesh -- the loop would install the launcher file
		# twice while the runtime apk never lands. docs/INSTALL.md §2 is the rule this guards against.
		pkg=$(ls "$APK_DIR/$stem"-[0-9]*.apk 2>/dev/null | sort | tail -n 1)
		if [ -z "$pkg" ]; then
			say "install_$stem" skip
			continue
		fi
		out=$($APK add --allow-untrusted "$pkg" 2>&1)
		rc=$?
		if [ "$rc" = 0 ]; then
			installed_any=yes
			say "install_$stem" "$(basename "$pkg")"
		elif echo "$out" | grep -qi 'installable'; then
			# architecture mismatch is the expected answer on a machine that cannot carry this package
			say "install_$stem" uninstallable
		else
			fail=$((fail + 1))
			say "install_$stem" "rc=$rc"
			echo "$out" | tail -3
		fi
	done
}

restart_services() {
	if [ "$RESTART_NOW" != 1 ] && [ "$installed_any" != yes ]; then say restart not_needed; return 0; fi
	did_restart=yes
	# taken before the restart, because the window being graded is the restart itself: how many times the applier
	# wrote from here on, and whether the air already carried the plan when this began.
	BEFORE_WRITES=$(applier_writes)
	BEFORE_AIR=$(air_carries_plan)
	$INIT_D/rpcd restart >/dev/null 2>&1
	$INIT_D/zte-mesh restart >/dev/null 2>&1
	# The init brings the instances up asynchronously and attach needs a second to create and enslave zmesh0;
	# reading the machine back before that settles reports the old instance set.
	sleep "$RESTART_WAIT"
	[ "$RESTART_NOW" = 1 ] && [ "$installed_any" != yes ] && { say restart now; return 0; }
	say restart rpcd+zte-mesh
}

versions() {
	for stem in $PKG_STEMS; do
		# [0-9] after the stem: without it `zte-mesh-` also matches a zte-mesh-launcher- line left installed
		# from an earlier batch, and the version reported for zte-mesh would be that package's release.
		v=$($APK list --installed 2>/dev/null | sed -n "s|^$stem-\([0-9][^ ]*\) .*|\1|p" | head -1)
		say "version_$stem" "${v:-absent}"
	done
}

# --- reading the machine back --------------------------------------------------------------------------------------
json_get() {
	# json_get DOC KEY -- one sed over the lines, no jsonfilter: `ubus call` pretty-prints this document with one key
	# per line, the five keys below are flat, and the first line matching wins (the status call prints its own keys
	# before the nested `state`, so the outer `running` is always the one that matches).
	printf '%s' "$1" | sed -n "s/.*\"$2\": *\"\{0,1\}\([a-z0-9_-]*\)\"\{0,1\}.*/\1/p" | head -1
}

kv_get() {
	# kv_get FILE KEY -- /run/zte-mesh/wlan-sync.status is plain key=value lines on purpose (see wlan-sync's
	# write_status), so parsing it must not pretend it is JSON.
	[ -s "$1" ] && sed -n "s/^$2=//p" "$1" | head -1
}

instances_running() {
	# `ubus call service list` pretty-prints one document; the instance names sit at three tabs and their fields at
	# four, so depth is what separates an instance's own "running" from the nested blocks around it.
	$UBUS call service list '{"name":"zte-mesh"}' 2>/dev/null | awk '
		/^[\t]+"[a-z]/ {
			depth = 0
			while (substr($0, depth + 1, 1) == "\t") depth++
			key = $1
			gsub(/["{:]/, "", key)
			if (depth == 3) { inst = key; seen[inst] = "stopped" }
			else if (depth == 4 && key == "running" && $0 ~ /true/) seen[inst] = "running"
		}
		END { for (i in seen) printf "%s=%s ", i, seen[i] }
	'
}

smoke_service() {
	status=$($UBUS call luci.zte-mesh status 2>/dev/null)
	if [ -z "$status" ]; then
		fail=$((fail + 1))
		say smoke_service no-status
		return 1
	fi
	S_INSTALLED=$(json_get "$status" installed)
	S_RUNNING=$(json_get "$status" running)
	S_ATTACHED=$(json_get "$status" attached)
	S_ROLE=$(json_get "$status" role)
	S_LOCAL=$(json_get "$status" local)
	check service_installed true "$S_INSTALLED"
	check service_running true "$S_RUNNING"
	say service_role "$S_ROLE"
	# The shipped status call always prints `local` (luci.zte-mesh's status branch). An empty answer therefore is not
	# a configuration state: the running rpcd still has the plugin it loaded before this install, so the page reads the
	# old document even though the file on disk is new. `apk add` replaces the file, it does not reload rpcd.
	[ -n "$S_LOCAL" ] || S_LOCAL=absent
	if [ "$S_LOCAL" = absent ]; then
		fail=$((fail + 1))
		printf 'FAIL service_local reported=absent hint=%s\n' "rpcd still has the pre-install plugin: /etc/init.d/rpcd restart"
	else
		say service_local "$S_LOCAL"
	fi
	say service_attached "$S_ATTACHED"
	say uci_mesh_role "$($UCI -q get zte_mesh.main.mesh_role 2>/dev/null || echo unset)"
	[ -n "$S_ROLE" ] || { fail=$((fail + 1)); say service_role empty; return 1; }
	return 0
}

smoke_instances() {
	all=$(instances_running)
	say instances "${all% }"
	# controller and attach belong to every role; the applier and the search loop do not, and which ones do is the
	# same judgement the init used when it opened the instances.
	for want in controller attach; do
		got=absent
		for pair in $all; do
			[ "${pair%%=*}" = "$want" ] && got=${pair#*=}
		done
		check "instance_$want" running "$got"
	done
	if [ "$S_ROLE" = master ] && [ "$S_LOCAL" != true ]; then
		# a controller that leaves its own radios alone must have no applier; one that shows up means the restart
		# signature now claims an instance the role does not run
		check instance_wlan-sync absent "$(pair_value "$all" wlan-sync)"
	else
		check instance_wlan-sync running "$(pair_value "$all" wlan-sync)"
	fi
	# The search loop is the controller half of the enrolment handshake; a child has nothing to search for.
	if [ "$S_ROLE" = master ]; then
		check instance_search running "$(pair_value "$all" search)"
	else
		check instance_search absent "$(pair_value "$all" search)"
	fi
}

pair_value() {
	# pair_value LIST KEY
	for pair in $1; do
		[ "${pair%%=*}" = "$2" ] && { printf '%s' "${pair#*=}"; return 0; }
	done
	printf absent
}

# The main BSS of a band is applied onto the router's own section (wlan-sync's ap_iface_of picks default_* and
# writes ssid/key there), so on a slave the visible result is not a `zte_*` section -- it is the SSID of the
# interface that was already there. Reading only zte_* names would report `none` on a machine where the applier
# did exactly the right thing.
# ssid_where SSID -> the wireless sections that carry this SSID, space separated.
# The same SSID may legitimately exist on more than one section, so the evidence is the sections that have it.
# `uci show` prints values in single quotes (wireless.default_radio2.ssid='FRESHR10'), so the quotes have to be
# optional -- matching only the bare form calls a healthy applier a missing SSID. Measured on ax3600, where
# FRESHR10 and FRESHR10-5G were both live and the first version of this smoke reported absent.
# `|` as the sed delimiter and every sed-special character escaped: an SSID is user data and can contain
# `/`, `.` or `+`, which would otherwise break the expression or match something else.
ssid_where() {
	$UCI show wireless 2>/dev/null |
		sed -n "s|^wireless\.\([^.=]*\)\.ssid='\{0,1\}$(printf '%s' "$1" | sed 's/[][\\.*^$/|]/\\&/g')'\{0,1\}$|\1|p" |
		tr '\n' ' '
}

smoke_ssid() {
	# The plan and the wireless config are two files written by two processes, and a restart rewrites both:
	# measured on ax3600, the plan had no rows at t=0 (the applier had not run yet), at t=2s its rows were there
	# but wireless still carried the previous profile, and at t=4s the applier had landed the new one. Asserting
	# once, right after the restart this script itself asked for, fails a healthy slave -- and a reader following
	# INSTALL section 3 would take that as a broken install. So wait for the pair to settle, then grade once.
	waited=0
	settled=no
	pending=
	rows=0
	while :; do
		rows=$(sed -n 's/^rows=\([0-9]*\)$/\1/p' "$PLAN" 2>/dev/null | head -1)
		[ -n "$rows" ] || rows=0
		pending=
		i=0
		while [ "$i" -lt "$rows" ]; do
			enabled=$(sed -n "s/^$i\.enabled=//p" "$PLAN" | head -1)
			ssid=$(sed -n "s/^$i\.ssid=//p" "$PLAN" | head -1)
			if [ "$enabled" = 1 ] && [ -n "$ssid" ]; then
				[ -n "$(ssid_where "$ssid")" ] || pending="$pending $i"
			fi
			i=$((i + 1))
		done
		if [ "$rows" -gt 0 ] && [ -z "$pending" ]; then
			settled=yes
			break
		fi
		[ "$waited" -lt "$SSID_WAIT" ] || break
		sleep "$SSID_POLL"
		waited=$((waited + SSID_POLL))
	done
	say ssid_wait "$waited"
	[ -s "$PLAN" ] || { say profile_ssid no-plan; return 0; }
	[ "$rows" -gt 0 ] || { say profile_ssid no-rows; return 0; }
	[ "$settled" = yes ] || say ssid_pending "${pending# }"
	i=0
	while [ "$i" -lt "$rows" ]; do
		enabled=$(sed -n "s/^$i\.enabled=//p" "$PLAN" | head -1)
		ssid=$(sed -n "s/^$i\.ssid=//p" "$PLAN" | head -1)
		if [ "$enabled" = 1 ] && [ -n "$ssid" ]; then
			where=$(ssid_where "$ssid")
			where=${where% }
			if [ -n "$where" ]; then
				pass=$((pass + 1))
				say "plan_ssid_$i" "$ssid@$where"
			else
				fail=$((fail + 1))
				printf 'FAIL plan_ssid_%s expected=%s got=absent\n' "$i" "$ssid"
			fi
		fi
		i=$((i + 1))
	done
}

smoke_profile() {
	say plan_exists "$([ -s "$PLAN" ] && echo yes || echo no)"
	ours=$($UCI show wireless 2>/dev/null | sed -n 's/^wireless\.\(zte_[^.]*\)=wifi-iface$/\1/p' | tr '\n' ' ')
	ours=${ours% }
	if [ "$S_ROLE" = master ] && [ "$S_LOCAL" != true ]; then
		# nothing may reach this router's radios: an applier result or one of our sections is the failure
		check profile_no_status_file absent "$([ -e "$SYNC_STATUS" ] && echo present || echo absent)"
		check profile_sections unset "${ours:-unset}"
		return 0
	fi
	if [ ! -s "$SYNC_STATUS" ]; then
		fail=$((fail + 1))
		say smoke_profile no-status-file
		return 1
	fi
	say sync_result "$(kv_get "$SYNC_STATUS" result)"
	say sync_changed "$(kv_get "$SYNC_STATUS" changed)"
	say sync_bands "$(kv_get "$SYNC_STATUS" bands)"
	say sync_message "$(kv_get "$SYNC_STATUS" message)"
	check sync_result ok "$(kv_get "$SYNC_STATUS" result)"
	say profile_sections "${ours:-none}"
	return 0
}

smoke_log() {
	# Not a smoke of its own, one number: it catches the shape where everything above passes because the service
	# restarted out from under the check. The trace socket retry pair is excluded on purpose -- ns-run binds and
	# rebinds /var/tmp/zte-meshctl0._snd and logs the EADDRINUSE/ECONNREFUSED on the way (dozens of lines per hour on
	# the lab controller), so counting it would make the number meaningless. What is left is what nobody has explained.
	err=$($LOGREAD 2>/dev/null | grep -i zte-mesh | grep -viE 'ZTE_CONTROLLER_TRACE unix_bind' |
		grep -ciE 'error|failed|errno=[1-9]|crash loop')
	say log_error_lines "${err:-0}"
}

# --- is the hop count real on this machine? (B16) --------------------------------------------------------------------
# The slave startup path deliberately writes CurHop=-2, which is the registry's `not attached`, and nothing else in
# our code writes that column for a child -- so any other number in the supervisor's own cfg line can only have come
# from the daemon (slave_init, or the controller's hop TLV). On a controller the 2 is ours (it is what the master
# path writes), so there the same line is evidence about the database, not a verdict about the agent.
cur_hop_line() {
	# the newest witness line, epoch included: the supervisor stamps the moment it read the columns, so the age of the
	# evidence is readable instead of assumed. Measured on ax3600 during the r18 install round (13:26:51 `cur_hop=-2`,
	# 13:27:18 `cur_hop=2`): a child sits at the not-attached baseline for about half a minute after the daemon starts,
	# and a read-only run that lands in that half minute is the "问得太早" mistake again.
	$LOGREAD 2>/dev/null | grep 'ZTE_MESH_SERVICE cfg' | tail -1
}

field() { printf '%s' "$1" | sed -n "s/.*$2=\([-0-9][0-9]*\).*/\1/p"; }

smoke_cur_hop() {
	line=$(cur_hop_line)
	[ -n "$line" ] || { say cur_hop no-cfg-line; return 0; }
	hop=$(field "$line" cur_hop)
	max=$(field "$line" max_hop)
	[ -n "$max" ] && say max_hop "$max"
	stamp=$(field "$line" epoch)
	now=$($DATE +%s 2>/dev/null)
	age=
	[ -n "$stamp" ] && [ -n "$now" ] && age=$((now - stamp))
	[ -n "$age" ] && say cur_hop_age "$age"
	judge=yes
	# Four shapes where the number is evidence and not a verdict: a controller writes this column itself, so only a
	# child's value says anything about the agent; -98 is the supervisor's own `could not read the column`, which is
	# not a hop count; a restart replaces the supervisor, which writes its baseline snapshot seconds to minutes after
	# a fresh start -- grading the window that run just opened is the same mistake the SSID smoke had to fix.
	[ "$S_ROLE" != slave ] && judge=no
	[ "$did_restart" = yes ] && judge=no
	case "$hop" in -98) judge=no ;; esac
	# and the fourth: a witness younger than the settling window is a daemon that has not been told yet, not a child
	# that never learned. `cur_hop_age` says how young, so a reader can see the same line judged a minute later.
	[ -n "$age" ] && [ "$age" -lt "$CUR_HOP_GRACE" ] && judge=no
	if [ "$judge" != yes ]; then say cur_hop "$hop"; return 0; fi
	if [ "$hop" = -2 ]; then
		fail=$((fail + 1))
		printf 'FAIL cur_hop expected=updated got=-2\n'
	else
		pass=$((pass + 1))
		say cur_hop "$hop"
	fi
}

# --- is the lab CurHop override live? (B16b, PLAN H-14 ⑦ 后半) -------------------------------------------------------
# The question left over there is which number a child's hop column carries: the value the controller advertises in
# its 1905 vendor TLV, or the child's own MaxHop. As long as the controller writes CurHop=2 and a child's MaxHop is
# also 2 those are the same number, so the only way to separate them is to make the controller say something else.
# The switch is the startup profile that already exists: one line `MultiAPCfg 0 CurHop 5` in
# /run/zte-mesh/db-profile. That is a file under /run, not an option in any package -- no page, no UCI file and no
# apk can set it, so it cannot reach a user's machine, and the master startup path writes no CurHop at all (only the
# slave path does, as -2), so the line survives a restart. Proving it is live matters before reading the child: a
# controller that silently rejected the field would leave the child at 2 and the observation would look like an
# answer.
db_profile_field() {
	# the last line the daemon would have applied: profile_apply walks the file and writes each match, so a repeated
	# field is won by the later line. Empty output covers both "no file" and "no such line".
	[ -r "$RUNDIR/db-profile" ] || return 0
	awk -v want="$1" '$1 == "MultiAPCfg" && $3 == want { value = $4 } END { if (value != "") print value }' "$RUNDIR/db-profile"
}

smoke_cur_hop_override() {
	want=$(db_profile_field CurHop)
	[ -n "$want" ] || { say db_profile_cur_hop absent; return 0; }
	say db_profile_cur_hop "$want"
	# A child's number is the daemon's answer to the air (that is `smoke_cur_hop`), not this write -- and its startup
	# path puts -2 back over the profile, so on that side there is nothing here to verify.
	[ "$S_ROLE" = master ] || { say master_cur_hop not-master; return 0; }
	line=$(cur_hop_line)
	[ -n "$line" ] || { say master_cur_hop no-cfg-line; return 0; }
	got=$(field "$line" cur_hop)
	[ -n "$got" ] || { say master_cur_hop no-cfg-line; return 0; }
	case "$got" in -98) say master_cur_hop not-read; return 0 ;; esac
	# the same settling window as the child's: a snapshot taken before the daemon finished starting says nothing
	# about whether the override reached the database.
	stamp=$(field "$line" epoch)
	now=$($DATE +%s 2>/dev/null)
	[ -n "$stamp" ] && [ -n "$now" ] && [ $((now - stamp)) -lt "$CUR_HOP_GRACE" ] && { say master_cur_hop "$got"; return 0; }
	check master_cur_hop "$want" "$got"
}

# --- did this restart write anything? (B8b, PLAN H-14) ----------------------------------------------------------------
# A child's restart is supposed to find the air already carrying what the plan asks for, and then write nothing.
# Reading `/etc/config/wireless` cannot show the difference: the old seed renamed the network and the controller's M2
# renamed it back, so the file ends up identical either way -- while every client in the house dropped twice. The
# applier's own line is the only witness, and the window is the restart this script asked for.
applier_writes() {
	$LOGREAD 2>/dev/null | grep -c 'applied; wireless committed'
}

logread_readable() {
	# a missing logread counts as zero lines, and zero minus zero is a quiet restart that never happened
	$LOGREAD >/dev/null 2>&1
}

air_carries_plan() {
	# air_carries_plan -- `yes` when every enabled row of the plan is already a name in the wireless config, before
	# the restart. That condition is what turns a write from a convergence into a fault: the applier had nothing to land.
	[ -s "$PLAN" ] || { printf no-plan; return 0; }
	rows=$(sed -n 's/^rows=\([0-9]*\)$/\1/p' "$PLAN" | head -1)
	case "$rows" in ''|*[!0-9]*) printf no-rows; return 0 ;; esac
	[ "$rows" -gt 0 ] || { printf no-rows; return 0; }
	missing=
	i=0
	while [ "$i" -lt "$rows" ]; do
		enabled=$(sed -n "s/^$i\.enabled=//p" "$PLAN" | head -1)
		ssid=$(sed -n "s/^$i\.ssid=//p" "$PLAN" | head -1)
		if [ "$enabled" = 1 ] && [ -n "$ssid" ]; then
			[ -n "$(ssid_where "$ssid")" ] || missing="$missing $i"
		fi
		i=$((i + 1))
	done
	[ -z "$missing" ] && printf yes || printf 'rows-unset:%s' "${missing# }"
}

smoke_restart_quiet() {
	[ "$did_restart" = yes ] || { say applier_writes no-restart; return 0; }
	if [ "$S_ROLE" = master ] && [ "$S_LOCAL" != true ]; then
		# no applier runs on this role: the restart cannot have written a wireless config it never touches
		say applier_writes no-applier
		return 0
	fi
	logread_readable || { say applier_writes no-logread; return 0; }
	after=$(applier_writes)
	if [ "${after:-0}" -lt "${BEFORE_WRITES:-0}" ]; then
		# the ring rolled over inside the window: the delta of a truncated window is not a number
		say applier_writes wrapped
		return 0
	fi
	writes=$(( ${after:-0} - ${BEFORE_WRITES:-0} ))
	say air_matched_plan "$BEFORE_AIR"
	if [ "$BEFORE_AIR" = yes ]; then
		check applier_writes 0 "$writes"
	else
		# the air did not carry the plan yet, so this restart had work to do -- say how much, judge nothing
		say applier_writes "$writes"
	fi
}

# --- the MESH compatibility switch as the machine holds it (A12b) ---------------------------------------------------
# The page option and the key the daemon reads answer opposite questions: `mesh_compatible=1` (MESH 兼容 on, accept
# any child router) must reach /run/zte-mesh/config as `mesh.v2=0`. Both halves are on the machine, so the pair is
# judged here: an install that put the new page over the old writer -- or the old conffile over the new writer --
# would otherwise show up as a switch that quietly does nothing.
# The retired `mesh_v2` is read by nobody. A machine whose /etc/config/zte-mesh still carries it, and has no
# mesh_compatible, gets no mesh.v2 line at all and the daemon keeps its own default (compatible) -- which is what the
# old key's default meant, so the behaviour does not move; the line `mesh_v2_legacy_uci` says that case happened.
uci_get() { $UCI -q get "$1" 2>/dev/null || :; }

smoke_mesh_compat() {
	page=$(uci_get zte_mesh.main.mesh_compatible)
	legacy=$(uci_get zte_mesh.main.mesh_v2)
	say mesh_compat_uci "${page:-unset}"
	say mesh_v2_legacy_uci "${legacy:-unset}"
	if [ ! -r "$RUNDIR/config" ]; then
		# the service file is what the init script writes on a start: no file says the daemon is not running here,
		# and then there is nothing to compare the page value against.
		say mesh_v2_line no-config
		return 0
	fi
	line=$(sed -n 's/^mesh\.v2=//p' "$RUNDIR/config" | tail -1)
	say mesh_v2_line "${line:-absent}"
	case "$page" in
	0) check mesh_compat_inverted 1 "${line:-absent}" ;;
	1) check mesh_compat_inverted 0 "${line:-absent}" ;;
	'') [ -z "$line" ] || check mesh_compat_inverted absent "$line" ;;
	*) say mesh_compat_inverted "page value is not 0 or 1: $page" ;;
	esac
}

# --- what one peer actually reported (PLAN H-12 and H-13) ------------------------------------------------------------
# H-12 is whether a ZTE child router sends the Device Inventory TLV (0xd4) at all. That TLV carries three strings --
# serial, software, hardware -- and only the middle one is ever written to a file: the serial belongs to the device,
# so `peers_software` from `peer_files` plus `peer_software_bytes` here are the whole answer. They answer it only
# beside a live peer: `peers_seen=0` means this machine never heard from one, and zero files says nothing about 0xd4.
# H-13 is whether the station count arrives split per BSS. `<MAC>.<BSSID>.sta` is the record, `sta_file_sum` is what
# those records add to, and `bss_detail_rows` is the page rows built from the same files (write_state sums them, so
# the two agree by construction and neither is a verdict on the other). Empty means the frames never split -- the
# finding PLAN wants, not a fault of ours, so nothing here is counted as an assertion.
# `nodes_rows` and `nodes_online` are the controller's own list of its children: read them twice, with the child's
# cable in and out, and that pair is PLAN H-01's "拔线后还在" -- the decision belongs to the second run, not the script.
peer_evidence() {
	seen=0
	sta_sum=0
	soft_bytes=0
	if [ -d "$PEERS" ]; then
		seen=$(ls "$PEERS" 2>/dev/null | cut -d. -f1 | sort -u | grep -c ':')
		for f in $PEERS/*.sta; do
			[ -e "$f" ] || continue
			v=$(cat "$f" 2>/dev/null)
			case "$v" in ''|*[!0-9]*) ;; *) sta_sum=$((sta_sum + v)) ;; esac
		done
		for f in $PEERS/*.software; do
			[ -e "$f" ] || continue
			n=$(wc -c < "$f" 2>/dev/null)
			case "$n" in ''|*[!0-9]*) ;; *) soft_bytes=$((soft_bytes + n)) ;; esac
		done
	fi
	say peers_seen "$seen"
	say sta_file_sum "$sta_sum"
	say peer_software_bytes "$soft_bytes"
	if [ -r "$STATE" ]; then
		say bss_detail_rows "$(grep -o '"bssid":"' "$STATE" | wc -l)"
		say nodes_rows "$(grep -o '"zte":' "$STATE" | wc -l)"
		say nodes_online "$(grep -o '"online":true' "$STATE" | wc -l)"
	else
		say bss_detail_rows no-state
		say nodes_rows no-state
		say nodes_online no-state
	fi
}

# --- the wireless backhaul as the child holds it (PLAN H-01) ---------------------------------------------------------
# The four uci lines are this package's own writer, so on a slave asked for `backhaul=multi_ap` they are judged:
# without `wds` netifd refuses the bridge (BRIDGE_NOT_ALLOWED) and 4addr never opens, and without `multi_ap 1` the
# supplicant will not associate to a backhaul BSS at all (PLAN H-01's known trap).
# A section only exists once the controller has sent a backhaul row, so `backhaul_section=none` on a child that has
# not been given one is reported, not failed.
# Everything below that line belongs to wpad, the radio and the far end, so it is read and not judged -- including
# the two 4addr greps: which command on which build actually names that flag is exactly what the on-machine window is
# for, and a missing string here is a measurement about the tool, not a broken applier.
smoke_backhaul() {
	want=$(uci_get zte_mesh.main.backhaul)
	bands=$($UCI -q show wireless 2>/dev/null | sed -n 's/^wireless\.zte_backhaul_sta_\([a-z0-9]*\)=.*/\1/p')
	say backhaul_asked "${want:-unset}"
	if [ -z "$bands" ]; then
		say backhaul_section none
		return 0
	fi
	for b in $bands; do
		sec=zte_backhaul_sta_$b
		ifn=$(uci_get "wireless.$sec.ifname")
		[ -n "$ifn" ] || ifn=$sec
		say "backhaul_section_$b" "$sec"
		say "backhaul_device_$b" "$(uci_get "wireless.$sec.device")"
		say "backhaul_network_$b" "$(uci_get "wireless.$sec.network")"
		say "backhaul_disabled_$b" "$(uci_get "wireless.$sec.disabled")"
		if [ "$S_ROLE" = slave ] && [ "$want" = multi_ap ]; then
			check "backhaul_mode_$b" sta "$(uci_get "wireless.$sec.mode")"
			check "backhaul_multi_ap_$b" 1 "$(uci_get "wireless.$sec.multi_ap")"
			check "backhaul_wds_$b" 1 "$(uci_get "wireless.$sec.wds")"
		fi
		iwinfo=$($IW dev "$ifn" info 2>/dev/null)
		if [ -z "$iwinfo" ]; then
			say "backhaul_iface_$b" missing
			continue
		fi
		say "backhaul_iface_$b" up
		say "backhaul_assoc_$b" "$(printf '%s\n' "$iwinfo" | grep -qi '^[[:space:]]*ssid ' && echo yes || echo no)"
		say "backhaul_4addr_iw_$b" "$(printf '%s\n' "$iwinfo" | grep -qi '4addr' && echo yes || echo no)"
		say "backhaul_4addr_ip_$b" "$($IP -d link show "$ifn" 2>/dev/null | grep -qi '4addr' && echo yes || echo no)"
		say "backhaul_brport_$b" "$([ -e "/sys/class/net/$ifn/brport" ] && echo yes || echo no)"
		say "backhaul_station_$b" "$($IW dev "$ifn" station dump 2>/dev/null | grep -qi 'Authorized' && echo authorized || echo none)"
	done
}

# --- reading the installed copy back -------------------------------------------------------------------------------
marker_present() {
	# marker_present FILE NEEDLE -- a string, not a hash: `apk` strips the ELF sections, so the device copy cannot
	# be compared byte-for-byte with the archive (the precedent is ZTE_TRACE_POLL and log_budget= from the r16 round).
	# What it does prove is that this batch's writer is on the machine: a service that sums per-BSS files against a
	# library that never writes them falls back to the digest every second and the page keeps the old behaviour.
	# `-e` because a needle can start with a dash: the launcher's contract word is --launcher-check.
	if [ ! -r "$1" ]; then printf unreadable; elif grep -aqF -e "$2" "$1"; then printf yes; else printf no; fi
}

packed_copy() {
	trace=$(marker_present "$ROOTFS/root/lib/libzte_controller_trace.so" '.%02x:%02x:%02x:%02x:%02x:%02x.sta')
	probe=$(marker_present "$ROOTFS/root/bin/db-engine-probe" 'bss_detail')
	if [ "$trace" = unreadable ] || [ "$probe" = unreadable ]; then
		# no rootfs to read: a machine where the runtime tarball was never unpacked is not a stale one
		say trace_per_bss_name "$trace"
		say probe_bss_detail "$probe"
		return 0
	fi
	check trace_per_bss_name yes "$trace"
	check probe_bss_detail yes "$probe"
}

package_scripts() {
	# A batch that changes only the package's own shell files leaves nothing in the rootfs tree: the r18 child borrows
	# its seed from the air (`live_ap_iface` in the init script) and its applier compares flags by meaning (`set_flag`
	# in wlan-sync). Measured on both lab machines one batch earlier: r17's installed copies answer zero hits for
	# either string, so this read-back grades the batch rather than the machine.
	init=$(marker_present "$INIT_D/zte-mesh" 'live_ap_iface')
	sync=$(marker_present "$LIBEXEC/wlan-sync" 'set_flag')
	# A12b renamed the page option and inverted it, so the init script is the file that changed. This marker says the
	# reader on the machine is the one that inverts; `smoke_mesh_compat` is the pair that shows it working.
	compat=$(marker_present "$INIT_D/zte-mesh" 'mesh_compatible')
	# r19 batch: the collector learned one switch. Reading it back matters because the pause is the only lever that
	# stops the station counts without touching the daemon -- a machine still holding the old script answers `no`.
	collect=$(marker_present "$LIBEXEC/sta-collect" 'collect-pause')
	if [ "$init" = unreadable ] || [ "$sync" = unreadable ] || [ "$compat" = unreadable ] ||
		[ "$collect" = unreadable ]; then
		say init_seed_from_air "$init"
		say sync_flag_by_meaning "$sync"
		say init_reads_mesh_compatible "$compat"
		say collect_pause_in_collector "$collect"
		return 0
	fi
	check init_seed_from_air yes "$init"
	check sync_flag_by_meaning yes "$sync"
	check init_reads_mesh_compatible yes "$compat"
	check collect_pause_in_collector yes "$collect"
}

launcher_evidence() {
	# The package is one now: the runtime tarball and, under $LIBEXEC/arch/<cpu>/, one build per CPU, both from
	# zte-mesh. A machine that still lists zte-mesh-launcher as installed therefore holds two packages claiming the
	# same files, and which one owns them after an upgrade is the apk solver's answer, not something this script can
	# settle -- the line exists to make the question visible in the same read-back that checks the entry script.
	legacy=$($APK list --installed 2>/dev/null | sed -n 's|^zte-mesh-launcher-\([0-9][^ ]*\) .*|\1|p' | head -1)
	say legacy_launcher_package "${legacy:-absent}"
	# The launcher is arch:all now: one apk, one build per CPU under $LIBEXEC/arch/<cpu>/, and this entry
	# script installed twice -- as ns-run and as mesh-search -- to pick the one matching `uname -m`. The
	# install-time gate that used to refuse a wrong board (`DEPENDS:=@(TARGET_zte||TARGET_qualcommax)`) is
	# gone, so this is the line that says the machine has a launcher it can actually exec. Asking the entry
	# script rather than recomputing its mapping here is deliberate: the read-back then proves the code that
	# the service runs, not a copy of its logic.
	entry=$(marker_present "$LIBEXEC/ns-run" '--launcher-check')
	if [ "$entry" != yes ]; then
		# Either no launcher on this machine, or the one from before the entry script -- a hand-built
		# binary in $LIBEXEC is an environment fact, not a stale batch.
		say launcher_dispatch "$entry"
		say launcher_arch not-installed
		return 0
	fi
	check launcher_dispatch yes "$entry"
	# The package lays the same script down under the second name the service runs. A tree that carries only
	# ns-run starts the controller and leaves the search instance dead, so the half install is worth its own line.
	search=$(marker_present "$LIBEXEC/mesh-search" '--launcher-check')
	check launcher_second_name yes "$search"
	resolved=$("$LIBEXEC/ns-run" --launcher-check 2>&1)
	rc=$?
	say launcher_arch "$(printf '%s\n' "$resolved" | head -n 1)"
	if [ "$rc" = 0 ]; then
		check launcher_arch_present yes yes
	else
		check launcher_arch_present yes no
	fi
}

# split per BSS, so `peers_sta=0` beside `peer_reports_stations=no` means the peer never reported a number at all
# (measured on the lab link, where the only metrics digest is `hops=1`) and says nothing about the decoder. Read
# PLAN.md H-13 before treating an empty count as a failure.
# The last two lines make that pair readable in one run instead of eight. `peer_reads` says how many snapshots this
# answer needed -- more than one means the first showed no number anywhere -- and `peer_input` is the switch H-13 and
# H-15 actually ask about: did any station number reach this controller, from the per-BSS records or from the digest.
# `peer_reports_stations` keeps its old, narrower meaning: it is the snapshot that ended the sampling, and a stand-in
# that answers per BSS without a digest TLV can hold `peers_sta=2` beside `no`.
peer_files() {
	check=1
	sta=0
	soft=0
	report=no
	while :; do
		sta=0
		soft=0
		report=no
		if [ -d "$PEERS" ]; then
			for f in $PEERS/*.sta; do
				[ -e "$f" ] && sta=$((sta + 1))
			done
			for f in $PEERS/*.software; do
				[ -e "$f" ] && soft=$((soft + 1))
			done
			for f in $PEERS/*.metrics; do
				[ -e "$f" ] && grep -q '^stations=' "$f" && report=yes
			done
		fi
		# a number from either record ends the sampling; so does the last read this run is allowed
		[ "$sta" -gt 0 ] && break
		[ "$report" = yes ] && break
		[ "$check" -lt "$METRICS_READS" ] || break
		sleep "$METRICS_READ_GAP"
		check=$((check + 1))
	done
	say peers_sta "$sta"
	say peers_software "$soft"
	say peer_reports_stations "$report"
	say peer_reads "$check"
	if [ "$sta" -gt 0 ] || [ "$report" = yes ]; then say peer_input yes; else say peer_input no; fi
}

# --- the table the collector keeps (PLAN H-15) -------------------------------------------------------------------------
# /run/zte-mesh/ap-sta is the AP Metric answer's whole input: one `epoch=` line the collector stamps on each pass,
# then one `<bssid> <count>` row per access point it found. Both numbers are read and neither is judged: a controller
# does not run the collector, so `absent` is that role's shape, and a child with nothing associated legitimately holds
# rows of zeroes. The age is the one to read after pausing the collector -- the `collect-pause` flag stops the writes
# and the table then ages out of the responder's freshness window, so a growing number means the collector is not
# updating, which is the half of H-15 ⑤ that used to need a procd edit.
ap_sta_evidence() {
	if [ ! -r "$TABLE" ]; then
		say ap_sta_rows absent
		say ap_sta_age absent
		return 0
	fi
	say ap_sta_rows "$(sed -n '/^[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:.* [0-9][0-9]*$/p' "$TABLE" | wc -l)"
	epoch=$(sed -n 's/^epoch=\([0-9][0-9]*\)$/\1/p' "$TABLE" | head -1)
	now=$($DATE +%s 2>/dev/null)
	if [ -n "$epoch" ] && [ -n "$now" ]; then
		say ap_sta_age "$((now - epoch))"
	else
		say ap_sta_age unreadable
	fi
}

state_nodes() {
	# only a master with node rows has to print the per-BSS key; a child's state has no nodes by design
	[ -r "$STATE" ] || { say state_bss_detail no-state; return 0; }
	got=absent
	grep -q '"bss_detail"' "$STATE" && got=present
	if grep -q '"nodes":\[{' "$STATE"; then
		check state_bss_detail present "$got"
	else
		say state_bss_detail "$got"
	fi
}

install_apks
restart_services
versions
if smoke_service && smoke_instances && smoke_profile; then :; fi
# only where an applier is supposed to run: a controller that leaves its own radios alone has no plan to honour
if [ "$S_ROLE" = master ] && [ "$S_LOCAL" != true ]; then :; else smoke_ssid; fi
smoke_restart_quiet
smoke_log
smoke_cur_hop
smoke_cur_hop_override
smoke_mesh_compat
smoke_backhaul
packed_copy
package_scripts
launcher_evidence
ap_sta_evidence
peer_files
peer_evidence
state_nodes

say pass "$pass"
say fail "$fail"
[ "$fail" = 0 ] && say RESULT PASS || say RESULT FAIL
[ "$fail" = 0 ]
