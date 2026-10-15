#!/bin/sh

# Windows port of upstream has-session-return.sh.
# Validates exit codes for has-session command.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -L${TMUX_TEST_LABEL:-regress-has-session}"
$TMUX kill-server 2>/dev/null

FNULL="-fNUL"
trap '$TMUX kill-server 2>/dev/null' 0 1 15

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

# has-session with no server should fail
$TMUX $FNULL has -tfoo </dev/null 2>/dev/null && exit 1

# start server, has-session for non-existent session should fail
$TMUX $FNULL new -d </dev/null || exit 1
$TMUX has -tfoo 2>/dev/null && exit 1

# create named session, has-session should succeed
$TMUX kill-server 2>/dev/null
wait_for 10 server_gone || exit 1
$TMUX $FNULL new -dsfoo </dev/null || exit 1
$TMUX has -tfoo </dev/null 2>/dev/null || exit 1

# exact match and a prefix of the name
$TMUX has -t=foo </dev/null 2>/dev/null || exit 1
$TMUX has -t=fo </dev/null 2>/dev/null && exit 1

# after kill-session it should fail again
$TMUX new -dsbar </dev/null || exit 1
$TMUX kill-session -tfoo || exit 1
$TMUX has -tfoo </dev/null 2>/dev/null && exit 1

$TMUX kill-server 2>/dev/null

echo "ALL HAS-SESSION TESTS PASSED"
exit 0
