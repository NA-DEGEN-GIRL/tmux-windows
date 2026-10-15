#!/bin/sh

# Windows popups (PTY jobs on a pseudo console): display-popup -E closes
# when its command exits, repeated popups do not leak console hosts,
# Ctrl-C reaches the popup's command and popups are resized with the
# client. A client of the inner server under test runs in a pane of an
# outer server, whose capture-pane shows what the client drew.
# Must be run on Windows (Git Bash).

CMD_EXE=$(cygpath -w "$(command -v cmd.exe)")
PS_EXE=$(command -v powershell.exe)
WPS_EXE=$(cygpath -w "$PS_EXE")
PING_EXE=$(cygpath -w "$(command -v ping.exe)")

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-popup}
IN="$TEST_TMUX -L$LABEL-in"
OUTER="$TEST_TMUX -L$LABEL-out"
$IN kill-server 2>/dev/null
$OUTER kill-server 2>/dev/null
trap "$OUTER kill-server 2>/dev/null; $IN kill-server 2>/dev/null" 0 1 15
WTMUX=$(cygpath -w "$TEST_TMUX")

FAIL=0
fail() {
	echo "FAIL: $*"
	FAIL=1
}
pass() {
	echo "PASS: $*"
}

# Run PowerShell code and print its output.
ps() {
	"$PS_EXE" -NoProfile -NonInteractive -Command "$1" | tr -d '\r'
}

# Wait until a command prints the expected value; print the last value.
wait_for() {
	n=0
	while [ $n -lt ${3:-40} ]; do
		R=$(eval "$1")
		[ "$R" = "$2" ] && break
		sleep 0.5
		n=$((n + 1))
	done
	echo "$R"
}

# Lines of popup border the inner client shows.
border() {
	$OUTER capture-pane -p -t o | tr -d '\r' | grep -c '│'
}

# Console hosts the inner server has started.
conhosts() {
	ps "@(Get-CimInstance Win32_Process -Filter 'ParentProcessId=$IN_PID' |
	    Where-Object Name -eq 'conhost.exe').Count"
}

$IN -fNUL start-server \; set -g default-shell "$CMD_EXE" \; \
    new -d -s s -x 100 -y 30 </dev/null ||
	{ echo "FAIL: cannot start inner server"; exit 1; }
IN_PID=$($IN display -p '#{pid}' | tr -d '\r')
$OUTER -fNUL new -d -s o -x 120 -y 40 -- "$WTMUX" -L $LABEL-in attach -t s \
    </dev/null || { echo "FAIL: cannot start outer server"; exit 1; }
R=$(wait_for "\$IN list-clients | wc -l" 1)
[ "$R" = 1 ] || { echo "FAIL: inner client did not attach"; exit 1; }
sleep 1
BASE=$(conhosts)

# 1. display-popup -E closes when the command exits, every time, and the
# pseudo consoles go with the popups.
$IN bind -n F9 display-popup -E "echo popup-hi& \"$PING_EXE\" -n 2 127.0.0.1 >NUL"
i=0
while [ $i -lt 8 ]; do
	$OUTER send-keys -t o F9
	R=$(wait_for "\$OUTER capture-pane -p -t o | grep -c popup-hi" 1 20)
	[ "$R" = 1 ] || break
	R=$(wait_for border 0 20)
	[ "$R" = 0 ] || break
	i=$((i + 1))
done
[ $i = 8 ] && pass "display-popup -E closes itself" ||
	fail "display-popup -E closes itself: still open after $i"
R=$(wait_for conhosts "$BASE" 20)
[ "$R" = "$BASE" ] && pass "popups do not leak console hosts" ||
	fail "popups do not leak console hosts: $R not $BASE"

# 2. Ctrl-C reaches the command in a popup.
$IN bind -n F8 display-popup -E "\"$PING_EXE\" -n 60 127.0.0.1"
$OUTER send-keys -t o F8
sleep 2
[ "$(border)" -gt 0 ] || fail "popup with ping did not open"
$OUTER send-keys -t o C-c
R=$(wait_for border 0 20)
[ "$R" = 0 ] && pass "Ctrl-C in a popup" || fail "Ctrl-C in a popup"

# 3. A popup is resized when the client is.
$IN bind -n F7 display-popup -E -w 100 -h 12 "\"$WPS_EXE\" -NoProfile -Command \"while (1) { 'W=' + \$Host.UI.RawUI.WindowSize.Width; Start-Sleep -Milliseconds 300 }\""
$OUTER send-keys -t o F7
width() {
	$OUTER capture-pane -p -t o | tr -d '\r' | grep -o 'W=[0-9]*' | tail -1
}
R=$(wait_for width W=98 30)
[ "$R" = W=98 ] || fail "popup width: $R"
$OUTER resize-window -t o -x 60 -y 40
R=$(wait_for width W=58 30)
[ "$R" = W=58 ] && pass "popup resized with the client" ||
	fail "popup resized with the client: $R"
$OUTER send-keys -t o C-c
wait_for border 0 20 >/dev/null

$IN has -t s || fail "inner server is not running at the end"

if [ $FAIL -eq 0 ]; then
	echo "ALL POPUP TESTS PASSED"
	exit 0
fi
echo "SOME POPUP TESTS FAILED"
exit 1
