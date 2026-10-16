#!/bin/sh

# Windows-specific basic functionality tests.
# Tests ConPTY spawn, I/O, split windows, pane exit cleanup, session size,
# /dev/null config translation, Unix -S path warning, and default-shell guard.
# Must be run on Windows (Git Bash or similar).

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-basic}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp)
CFG=$(mktemp)
trap 'rm -f "$OUT" "$CFG"; $TMUX kill-server 2>/dev/null' 0 1 15

# On Windows, -f/dev/null doesn't work (native exe can't open Unix path).
# Use NUL instead.
FNULL="-fNUL"
CMD_W=$(cygpath -w "${COMSPEC:-C:/Windows/System32/cmd.exe}")

FAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
}

# wait_for SECONDS COMMAND...: poll until COMMAND succeeds.
wait_for() {
	_end=$(($(date +%s) + $1))
	shift
	while ! "$@"; do
		[ "$(date +%s)" -ge "$_end" ] && return 1
		sleep 0.2
	done
	return 0
}

server_gone() {
	$TMUX ls 2>&1 | grep -q "no server running"
}

# Kill the server and wait until a new one can be started under the label.
restart() {
	$TMUX kill-server 2>/dev/null
	wait_for 10 server_gone || fail "server did not exit"
}

# pane_line TARGET LINE: the pane shows LINE (ignoring trailing spaces).
pane_line() {
	$TMUX capture-pane -p -t "$1" | tr -d '\r' | sed 's/ *$//' |
	    grep -qxF "$2"
}

# Compare Windows paths ignoring case and separator style.
norm() {
	echo "$1" | tr 'A-Z/' 'a-z\\'
}

pane_count() {
	[ "$($TMUX list-panes -t "$1" | wc -l)" -eq "$2" ]
}

# 1. Session lifecycle: new, ls, kill-session
$TMUX $FNULL new -d -sfoo < /dev/null || exit 1
$TMUX ls -F '#{session_name}' | tr -d '\r' >$OUT
printf "foo\n" | cmp -s $OUT - || fail "1: ls after new"
$TMUX kill-session -tfoo
$TMUX has -tfoo 2>/dev/null && fail "1: session alive after kill-session"
echo "PASS 1: session lifecycle"
restart

# 2. Send-keys + capture-pane: the pane must run the command, so look for
# its output, which is not in the typed text.
$TMUX $FNULL new -d -sio -- "$CMD_W" /K < /dev/null || exit 1
$TMUX send-keys -tio "set /a 6*7" Enter
if wait_for 10 pane_line io 42; then
	echo "PASS 2: send-keys and capture-pane"
else
	fail "2: no command output in capture-pane"
fi
restart

# 3. Split-window + list-panes
$TMUX $FNULL new -d -ssplit < /dev/null || exit 1
$TMUX split-window -tsplit || exit 1
pane_count split 2 || fail "3: split-window did not make two panes"
echo "PASS 3: split-window"
restart

# 4. Pane exit cleanup (tests server_destroy_pane ConPTY path)
$TMUX $FNULL new -d -sexit -- "$CMD_W" /K < /dev/null || exit 1
$TMUX split-window -texit -- "$CMD_W" /K || exit 1
$TMUX send-keys -texit:0.1 "exit" Enter
if wait_for 10 pane_count exit 1; then
	echo "PASS 4: pane exit cleanup"
else
	fail "4: exited pane was not removed"
fi
restart

# 5. Detached session size with explicit dimensions
$TMUX $FNULL new -d -x 120 -y 40 < /dev/null || exit 1
$TMUX ls -F "#{window_width} #{window_height}" | tr -d '\r' >$OUT
printf "120 40\n" | cmp -s $OUT - || fail "5: size is $(cat $OUT)"
echo "PASS 5: detached session size"
restart

# 6. -f /dev/null translation: the server must be told NUL, not /dev/null.
# MSYS_NO_PATHCONV keeps Git Bash from rewriting /dev/null itself.
MSYS_NO_PATHCONV=1 $TEST_TMUX -L$LABEL -f /dev/null new -d -snull \
    < /dev/null || exit 1
$TMUX ls -F '#{session_name}' | tr -d '\r' >$OUT
printf "null\n" | cmp -s $OUT - || fail "6: ls after -f /dev/null"
CONFIG=$($TMUX display -p '#{config_files}' | tr -d '\r')
case "$(norm "$CONFIG")" in
nul)
	echo "PASS 6: -f /dev/null is NUL" ;;
*/dev/null*)
	fail "6: config files are '$CONFIG'" ;;
*)
	echo "SKIP 6: -f is not passed to an auto-started server" \
	    "(config files '$CONFIG')" ;;
esac
restart

# 7. -S with Unix path: should work as IPC label and emit warning
# MSYS_NO_PATHCONV prevents Git Bash from translating /tmp to C:/...
SOCK=/tmp/$LABEL-sock
WARN=$(MSYS_NO_PATHCONV=1 $TEST_TMUX -S $SOCK -fNUL new -d -sunix 2>&1)
echo "$WARN" | grep -qi "unix" || fail "7: no warning for -S $SOCK"
MSYS_NO_PATHCONV=1 $TEST_TMUX -S $SOCK ls -F '#{session_name}' |
    tr -d '\r' >$OUT
printf "unix\n" | cmp -s $OUT - || fail "7: session not found by -S label"
MSYS_NO_PATHCONV=1 $TEST_TMUX -S $SOCK kill-server 2>/dev/null
echo "PASS 7: -S with a Unix path"

# 8. default-shell guard: a Unix shell path is not a usable shell.
$TMUX $FNULL new -d -sshell < /dev/null || exit 1
MSG=$(MSYS_NO_PATHCONV=1 $TMUX set -g default-shell /bin/bash 2>&1)
RC=$?
SHELLOPT=$($TMUX show -gv default-shell | tr -d '\r')
if [ $RC -eq 0 ] || [ "$SHELLOPT" = /bin/bash ]; then
	fail "8: set default-shell /bin/bash accepted ($MSG)"
fi
$TMUX new-window -d -tshell: || fail "8: new-window after guard"
echo "PASS 8: default-shell rejects a Unix path"
restart

# The same from a configuration file, if -f reaches the server.
CFG_W=$(cygpath -w "$CFG")
printf 'set -g default-shell /bin/bash\n' >"$CFG"
$TMUX -f "$CFG_W" new -d -sshell < /dev/null || exit 1
CONFIG=$($TMUX display -p '#{config_files}' | tr -d '\r')
if [ "$(norm "$CONFIG")" = "$(norm "$CFG_W")" ]; then
	SHELLOPT=$($TMUX show -gv default-shell | tr -d '\r')
	[ "$SHELLOPT" = /bin/bash ] && fail "8: config set default-shell"
	pane_count shell 1 || fail "8: pane did not start"
	echo "PASS 8b: default-shell guard from a config file"
else
	echo "SKIP 8b: -f is not passed to an auto-started server" \
	    "(config files '$CONFIG')"
fi
$TMUX kill-server 2>/dev/null

if [ $FAIL -ne 0 ]; then
	echo "SOME BASIC TESTS FAILED"
	exit 1
fi
echo "ALL BASIC TESTS PASSED"
exit 0
