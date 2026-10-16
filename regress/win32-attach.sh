#!/bin/sh

# Windows attached clients, driven through a client of an INNER server
# running in a pane of an OUTER server (so the inner client has a real
# console): unique client names, suspend-client, lock, detach-client -P,
# restoring the console on detach, the nested session guard and control
# mode.
# Must be run on Windows (Git Bash or similar).

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-attach}
IN="$TEST_TMUX -L$LABEL-in"
OUT="$TEST_TMUX -L$LABEL-out"
WTMUX=$(cygpath -w "$TEST_TMUX")
SYS=$(cygpath -w "${SYSTEMROOT:-C:\\Windows}")\\System32
$IN kill-server 2>/dev/null
$OUT kill-server 2>/dev/null

DIR=$(mktemp -d)
WDIR=$(cygpath -w "$DIR")
trap "$IN kill-server 2>/dev/null; $OUT kill-server 2>/dev/null; rm -rf $DIR" \
    0 1 15

FAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
}
pass() {
	echo "PASS: $1"
}

# Poll until a command succeeds (up to about 10 seconds).
until_ok() {
	n=0
	until "$@"; do
		[ $n -ge 100 ] && return 1
		sleep 0.1
		n=$((n + 1))
	done
	return 0
}

# Inner client names, one per line, sorted.
clients() {
	timeout 10 $IN list-clients -F '#{client_name}' </dev/null |
	    tr -d '\r' | sort
}
nclients() {
	[ "$(clients | grep -c .)" -eq "$1" ]
}

# Start an outer session whose pane runs a cmd.exe command line, normally
# one that attaches an inner client.
outer() {
	env -u SHELL timeout 20 $OUT -f "$WDIR\\outer.conf" new -d -s "$1" \
	    -x 120 -y 40 -e TMUX= "$2" </dev/null
}
outer_has() {
	timeout 10 $OUT capture-pane -p -t "$1" </dev/null | grep -q "$2"
}
outer_fmt() {
	timeout 10 $OUT display -p -t "$1" "$2" </dev/null | tr -d '\r'
}
outer_is() {
	[ "$(outer_fmt "$1" "$2")" = "$3" ]
}
windows() {
	[ "$(timeout 10 $IN display -p -t s '#{session_windows}' </dev/null |
	    tr -d '\r')" = "$1" ]
}
inner_has() {
	timeout 10 $IN capture-pane -p -t s </dev/null | grep -q "$1"
}

printf 'set -g remain-on-exit on\n' >$DIR/outer.conf
timeout 20 $IN -f NUL new -d -s s -x 100 -y 30 </dev/null || {
	echo "FAIL: inner new-session"
	exit 1
}
ATTACH="$WTMUX -L$LABEL-in attach -t s"

# 1. Two attached clients have different names and can be told apart.
outer o1 "$ATTACH"
until_ok nclients 1
C1=$(clients)
outer o2 "$ATTACH"
if until_ok nclients 2; then
	C2=$(clients | grep -v "^$C1\$")
	if [ -n "$C2" ] && [ "$C1" != "$C2" ]; then
		timeout 10 $IN detach-client -t "$C1" </dev/null
		if until_ok nclients 1 && [ "$(clients)" = "$C2" ]; then
			pass "client names are unique ($C1, $C2)"
		else
			fail "detach-client -t $C1 left '$(clients)'"
		fi
	else
		fail "two clients share the name $C1"
	fi
else
	fail "inner clients did not attach: '$(clients)'"
fi
C=$(clients)

# 2. suspend-client (C-b C-z) does not leave the client frozen.
timeout 10 $OUT send-keys -t o2 C-b C-z </dev/null
sleep 1
F=$(timeout 10 $IN list-clients -F '#{client_flags}' </dev/null)
timeout 10 $OUT send-keys -t o2 C-b c </dev/null
if until_ok windows 2; then
	pass "suspend-client does not freeze the client"
else
	fail "client frozen after suspend-client (flags $F)"
fi

# 3. lock-client hands the console to lock-command, then gives it back.
PS="$SYS\\WindowsPowerShell\\v1.0\\powershell.exe"
LOCKED="$WDIR\\locked.txt"
LOCKCMD="Read-Host | Set-Content -Encoding ascii '$LOCKED'"
timeout 10 $IN set -g lock-command "$PS -NoProfile -Command \"$LOCKCMD\"" \
    </dev/null
timeout 10 $IN lock-client -t "$C" </dev/null
sleep 1
timeout 10 $OUT send-keys -t o2 'lock-input-word' Enter </dev/null
if until_ok test -s $DIR/locked.txt &&
    grep -q '^lock-input-word' $DIR/locked.txt; then
	timeout 10 $OUT send-keys -t o2 C-b c </dev/null
	if until_ok windows 3; then
		pass "lock-command gets the console and the client resumes"
	else
		fail "client did not resume after lock-command"
	fi
else
	fail "lock-command did not get the input: $(cat $DIR/locked.txt 2>&1)"
fi
timeout 10 $IN set -gu lock-command </dev/null

# 4. Detaching restores the console: modes the inner tmux turned on are
# turned off again, the message is shown and the code page is put back.
timeout 10 $IN set -g mouse on </dev/null
outer o3 "$SYS\\chcp.com 437 >nul & $ATTACH & $SYS\\chcp.com & $SYS\\PING.EXE -n 60 127.0.0.1 >nul"
until_ok nclients 2
C3=$(clients | grep -v "^$C\$")
until_ok outer_is o3 '#{alternate_on}#{mouse_any_flag}' 11
timeout 10 $IN detach-client -t "$C3" </dev/null
if until_ok outer_has o3 ': *437'; then
	V=$(outer_fmt o3 '#{alternate_on}#{mouse_any_flag}')
	if [ "$V" = "00" ] && outer_has o3 '\[detached'; then
		pass "detach restores the console"
	else
		fail "after detach: alternate/mouse '$V', $(timeout 10 \
		    $OUT capture-pane -p -t o3 </dev/null | grep . | head -3)"
	fi
else
	fail "code page not restored: $(timeout 10 $OUT capture-pane -p \
	    -t o3 </dev/null | grep . | tail -3)"
fi
timeout 10 $IN set -g mouse off </dev/null

# 5. detach-client -P ends the shell the client was started from.
outer o4 "$ATTACH & echo shell-still-alive & $SYS\\PING.EXE -n 60 127.0.0.1 >nul"
until_ok nclients 2
C4=$(clients | grep -v "^$C\$")
timeout 10 $IN detach-client -P -t "$C4" </dev/null
if until_ok outer_is o4 '#{pane_dead}' 1; then
	pass "detach-client -P ends the parent shell"
else
	fail "parent shell survived detach-client -P"
fi

# 5b. ... but never a parent that is not on the client's console: here the
# client is run directly (argv form) by the outer server, which hosts its
# console and must survive.
env -u SHELL timeout 20 $OUT -f "$WDIR\\outer.conf" new -d -s o6 -x 120 \
    -y 40 -e TMUX= -- "$WTMUX" -L$LABEL-in attach -t s </dev/null
OUTER_PID=$(outer_fmt o6 '#{pid}')
until_ok nclients 2
C6=$(clients | grep -v "^$C\$")
PANE_PID=$(outer_fmt o6 '#{pane_pid}')
C6_PID=$(timeout 10 $IN display -p -c "$C6" '#{client_pid}' </dev/null |
    tr -d '\r')
timeout 10 $IN detach-client -P -t "$C6" </dev/null
if [ -z "$C6_PID" ] || [ "$C6_PID" != "$PANE_PID" ]; then
	fail "client $C6_PID is not the pane process $PANE_PID"
elif until_ok nclients 1 &&
    until_ok outer_is o6 '#{pane_dead}' 1 &&
    timeout 10 $OUT has-session -t o1 </dev/null &&
    [ "$(outer_fmt o1 '#{pid}')" = "$OUTER_PID" ]; then
	pass "detach-client -P spares a parent off the console"
else
	fail "detach-client -P with the server as parent: clients\
 '$(clients | tr '\n' ' ')', outer\
 $(timeout 10 $OUT list-sessions </dev/null 2>&1 | head -1)"
fi

# 6. Attaching from inside one of the server's own panes is refused.
timeout 10 $IN send-keys -t s "'$TEST_TMUX' -L$LABEL-in attach -t s \
    >$DIR/nested.out 2>&1; echo nested-done-marker" Enter </dev/null
if until_ok inner_has '^nested-done-marker' &&
    grep -q 'sessions should be nested with care' $DIR/nested.out; then
	pass "nested attach is refused"
else
	fail "nested attach: '$(cat $DIR/nested.out 2>&1)'"
	timeout 10 $IN send-keys -t s C-b d </dev/null
fi

# 7. Control mode from pipes: commands in, notifications out, exits at EOF.
printf 'display -p control-marker\n' |
    timeout 20 $IN -C attach -t s >$DIR/ctl.out 2>&1
R=$?
if [ $R -eq 0 ] && grep -q '^%begin' $DIR/ctl.out &&
    grep -q '^control-marker' $DIR/ctl.out && grep -q '^%end' $DIR/ctl.out &&
    grep -q '^%exit' $DIR/ctl.out; then
	pass "control mode over pipes"
else
	fail "control mode: exit $R, $(head -5 $DIR/ctl.out)"
fi

# 7b. Control mode commands ending in CRLF, as PowerShell and other Windows
# programs write them to a pipe, also when the CR and LF arrive separately.
timeout 10 $IN set -g @ctl_crlf crlf-value </dev/null
{
	printf 'display -p crlf-marker\r\nshow -gv @ctl_crlf\r\n'
	printf 'display -p split-marker\r'
	sleep 1
	printf '\n'
} | timeout 20 $IN -C attach -t s >$DIR/ctl.out 2>&1
if grep -qx 'crlf-marker' $DIR/ctl.out && grep -qx 'crlf-value' $DIR/ctl.out &&
    grep -qx 'split-marker' $DIR/ctl.out && ! grep -q '%error' $DIR/ctl.out
then
	pass "control mode commands ending in CRLF"
else
	fail "control mode with CRLF: $(od -c $DIR/ctl.out | head -8)"
fi

# 8. Control mode from a console.
outer o5 "$WTMUX -L$LABEL-in -C attach -t s"
until_ok outer_has o5 '^%' >/dev/null
timeout 10 $OUT send-keys -t o5 'display -p console-control-marker' Enter \
    </dev/null
if until_ok outer_has o5 '^console-control-marker'; then
	pass "control mode on a console"
else
	fail "control mode on a console: $(timeout 10 $OUT capture-pane -p \
	    -t o5 </dev/null | grep . | head -3)"
fi

timeout 10 $IN has-session -t s </dev/null || fail "inner server died"
timeout 10 $OUT has-session </dev/null || fail "outer server died"
$IN kill-server 2>/dev/null
$OUT kill-server 2>/dev/null
[ $FAIL -eq 0 ] && echo "ALL ATTACH TESTS PASSED"
exit $FAIL
