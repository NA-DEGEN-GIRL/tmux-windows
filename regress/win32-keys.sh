#!/bin/sh

# Windows key handling regression tests.
# Tests that send-keys with named keys produces expected effects.
# Cannot use the upstream cat -tv approach (no stty on Windows), so we
# verify key behavior through observable pane output. Every check looks for
# output that does not appear in the typed text, so a key that is lost or
# mistranslated makes the check fail.
#
# Upstream had 4 backspace/key-mapping fixes in Dec 2024 - Feb 2025:
#   6d792e4, 2a5eba7, 5c3cf2f, eece415

PATH=/bin:/usr/bin
TERM=screen

# Arguments such as "C:\Windows /?" must reach tmux unchanged.
MSYS_NO_PATHCONV=1
export MSYS_NO_PATHCONV

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-keys}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

FNULL="-fNUL"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; $TMUX kill-server 2>/dev/null' 0 1 15
FAIL=0
CMD_W=$(cygpath -w "${COMSPEC:-C:/Windows/System32/cmd.exe}")
BASH_W=$(cygpath -w /usr/bin/bash.exe)

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

# lines TARGET LINE: count pane lines equal to LINE (ignoring trailing
# spaces).
lines() {
	$TMUX capture-pane -p -t "$1" -S - | tr -d '\r' | sed 's/ *$//' |
	    grep -cxF "$2"
}

# shows TARGET LINE: the pane shows LINE.
shows() {
	[ "$(lines "$1" "$2")" -gt 0 ]
}

# check NUMBER DESCRIPTION TARGET LINE: wait for LINE in the pane.
check() {
	if wait_for 10 shows "$3" "$4"; then
		echo "PASS $1: $2"
	else
		fail "$1: $2 (no line '$4')"
		$TMUX capture-pane -p -t "$3" | tr -d '\r' | grep . | tail -5
	fi
}

fmt() {
	$TMUX display -p -t "$1" "$2" | tr -d '\r'
}

is() {
	[ "$(fmt "$1" "$2")" = "$3" ]
}

# cmd.exe in a directory with a file for Tab completion; bash for readline
# editing keys and Ctrl-C.
printf 'TAB_COMPLETED\r\n' >"$OUT/tabtarget_7f3a.txt"
$TMUX $FNULL new -d -skeys -x 120 -y 24 -c "$(cygpath -w "$OUT")" -- \
    "$CMD_W" /K < /dev/null || exit 1
$TMUX new-window -d -tkeys:1 -- "$BASH_W" --norc --noprofile -i ||
    exit 1
$TMUX send-keys -tkeys:1 "PS1='$ '" Enter

# --- Test 1: Enter key executes commands ---
$TMUX send-keys -tkeys:0 "set /a 40+2" Enter
check 1 "Enter key" keys:0 42

# --- Test 2: Literal characters arrive correctly ---
$TMUX send-keys -tkeys:0 "echo abcXYZ019" Enter
check 2 "Literal characters" keys:0 abcXYZ019

# --- Test 3: Space key ---
$TMUX send-keys -tkeys:0 "echo" Space "SPACE_OK" Enter
check 3 "Space key" keys:0 SPACE_OK

# --- Test 4: Tab key (cmd.exe file name completion) ---
$TMUX send-keys -tkeys:0 "type tabtarget_7f" Tab Enter
check 4 "Tab key" keys:0 TAB_COMPLETED

# --- Test 5: Escape key clears the cmd.exe input line ---
$TMUX send-keys -tkeys:0 "echo WRONG_"
sleep 0.5
$TMUX send-keys -tkeys:0 Escape
sleep 0.5
$TMUX send-keys -tkeys:0 "echo ESC_OK" Enter
check 5 "Escape key" keys:0 ESC_OK

# --- Test 6: Ctrl-C interrupts a running command ---
$TMUX send-keys -tkeys:1 "sleep 60; echo SLEEP_DONE" Enter
sleep 2
$TMUX send-keys -tkeys:1 C-c
# The console discards typeahead when it delivers Ctrl-C, so a command typed
# straight after can be lost on a slow machine: retype it until it runs. If
# Ctrl-C did not interrupt sleep 60, nothing runs in time and this fails.
n=0
while [ $n -lt 3 ]; do
	sleep 1
	$TMUX send-keys -tkeys:1 'echo $((6*7))' Enter
	wait_for 5 shows keys:1 42 && break
	n=$((n + 1))
done
check 6 "Ctrl-C interrupt" keys:1 42
[ "$(lines keys:1 SLEEP_DONE)" -eq 0 ] ||
    fail "6: the interrupted command list went on"

# --- Test 7: Arrow keys move the copy mode cursor ---
for i in 1 2 3 4 5; do
	$TMUX send-keys -tkeys:0 "echo line$i" Enter
done
wait_for 10 shows keys:0 line5
$TMUX copy-mode -tkeys:0
Y0=$(fmt keys:0 '#{copy_cursor_y}')
$TMUX send-keys -tkeys:0 Up Up
Y1=$(fmt keys:0 '#{copy_cursor_y}')
$TMUX send-keys -tkeys:0 Down
Y2=$(fmt keys:0 '#{copy_cursor_y}')
$TMUX send-keys -tkeys:0 Right Right Right
X1=$(fmt keys:0 '#{copy_cursor_x}')
$TMUX send-keys -tkeys:0 Left
X2=$(fmt keys:0 '#{copy_cursor_x}')
if [ "$Y1" -eq $((Y0 - 2)) ] && [ "$Y2" -eq $((Y0 - 1)) ] &&
    [ "$X2" -eq $((X1 - 1)) ]; then
	echo "PASS 7: Arrow keys in copy mode"
else
	fail "7: copy mode cursor y $Y0 -> $Y1 -> $Y2, x $X1 -> $X2"
fi

# --- Test 8: Home/End in copy mode ---
$TMUX send-keys -tkeys:0 End
XE=$(fmt keys:0 '#{copy_cursor_x}')
$TMUX send-keys -tkeys:0 Home
XH=$(fmt keys:0 '#{copy_cursor_x}')
if [ "$XH" -eq 0 ] && [ "$XE" -gt 0 ]; then
	echo "PASS 8: Home/End in copy mode"
else
	fail "8: Home/End moved the cursor to $XH/$XE"
fi
$TMUX send-keys -tkeys:0 -X cancel
is keys:0 '#{pane_in_mode}' 0 || fail "8: copy mode not left"

# --- Test 9: PgUp/PgDn in copy mode ---
I=1
while [ $I -le 40 ]; do
	$TMUX send-keys -tkeys:0 "echo scroll_line_$I" Enter
	I=$((I + 1))
done
wait_for 15 shows keys:0 scroll_line_40
$TMUX copy-mode -tkeys:0
$TMUX send-keys -tkeys:0 PPage
S1=$(fmt keys:0 '#{scroll_position}')
$TMUX send-keys -tkeys:0 NPage
S2=$(fmt keys:0 '#{scroll_position}')
if [ "$S1" -gt 0 ] && [ "$S2" -lt "$S1" ]; then
	echo "PASS 9: PgUp/PgDn in copy mode"
else
	fail "9: scroll position after PgUp $S1, after PgDn $S2"
fi
$TMUX send-keys -tkeys:0 -X cancel

# --- Test 10: Function keys (F3 repeats the last cmd.exe command) ---
$TMUX send-keys -tkeys:0 "echo FKEY_OK" Enter
wait_for 10 shows keys:0 FKEY_OK
$TMUX send-keys -tkeys:0 F3 Enter
fkey_twice() {
	[ "$(lines keys:0 FKEY_OK)" -ge 2 ]
}
if wait_for 10 fkey_twice; then
	echo "PASS 10: Function keys"
else
	fail "10: F3 did not repeat the last command"
fi

# --- Test 11: BSpace (backspace) deletes characters ---
$TMUX send-keys -tkeys:0 "echo BSPXXX" BSpace BSpace BSpace "ACE_OK" Enter
check 11 "Backspace" keys:0 BSPACE_OK

# --- Test 12: Modifier keys edit the readline line ---
$TMUX send-keys -tkeys:1 "ho MOD_OK" C-a "ec" C-e Enter
check 12 "C-a and C-e" keys:1 MOD_OK
$TMUX send-keys -tkeys:1 "echo NOT_RUN" C-a C-k "echo KILL_OK" Enter
check 12 "C-k" keys:1 KILL_OK
[ "$(lines keys:1 NOT_RUN)" -eq 0 ] || fail "12: C-k did not kill the line"

# --- Test 13: Mouse wheel key names are recognized ---
for key in WheelUpPane WheelDownPane WheelUpStatus WheelDownStatus; do
	$TMUX bind-key -n "$key" display-message "WHEEL_OK" || {
		fail "Mouse wheel key not recognized: $key"
	}
	$TMUX list-keys -T root "$key" | grep -q WHEEL_OK ||
	    fail "13: $key binding not listed"
done
echo "PASS 13: Mouse wheel key names"

# --- Test 14: ASCII punctuation does not collide with special keys ---
$TMUX send-keys -tkeys:0 "echo PUNCT_A/B C:\\Windows /? https://example.test/path" Enter
check 14 "ASCII punctuation" keys:0 \
    "PUNCT_A/B C:\\Windows /? https://example.test/path"

# --- Test 15: packed UTF-8 keys retain their metadata ---
$TMUX send-keys -tkeys:0 "echo UTF8_" 0xACC4 0xC18D "_" 0xC9C4 0xD589 Enter
expected=$(printf 'UTF8_\352\263\204\354\206\215_\354\247\204\355\226\211')
check 15 "UTF-8 key metadata" keys:0 "$expected"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL KEY TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME KEY TESTS FAILED"
	exit 1
fi
