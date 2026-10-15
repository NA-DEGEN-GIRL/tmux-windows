#!/bin/sh

# Windows jobs and command lines: run-shell, if-shell, #(), pipe-pane and
# copy-pipe (child stdio bridged to sockets), how shell commands and
# argument vectors become command lines for cmd.exe, Git Bash and
# PowerShell, .cmd shims, exit status mapping and tmux -c.
# Must be run on Windows (Git Bash).

# Programs used below, found before PATH is reset.
CMD_EXE=$(cygpath -w "$(command -v cmd.exe)")
PWSH_EXE=$(command -v pwsh.exe 2>/dev/null)
[ -n "$PWSH_EXE" ] && PWSH_EXE=$(cygpath -w "$PWSH_EXE")
NODE_EXE=$(command -v node.exe 2>/dev/null)
[ -n "$NODE_EXE" ] && NODE_EXE=$(cygpath -w "$NODE_EXE")

PATH=/bin:/usr/bin
TERM=screen

BASH_EXE=$(cygpath -w /usr/bin/bash.exe)
PRINTF_EXE=$(cygpath -w /usr/bin/printf.exe)

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -L${TMUX_TEST_LABEL:-jobs}"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp -d)
trap "$TMUX kill-server 2>/dev/null; rm -rf $OUT" 0 1 15
WOUT=$(cygpath -w "$OUT")

FAIL=0
fail() {
	echo "FAIL: $*"
	FAIL=1
}
pass() {
	echo "PASS: $*"
}

# Start a server with the given default shell. Not with -f: options must
# be set before the first pane is created.
start_server() {
	$TMUX kill-server 2>/dev/null
	$TMUX -fNUL start-server \; set -g default-shell "$1" \; \
	    set -g remain-on-exit on \; new -d -s s -x 120 -y 30 </dev/null ||
	    { echo "FAIL: cannot start server with $1"; exit 1; }
}

# Wait for a pane to die, then print its exit status and signal.
wait_dead() {
	n=0
	while [ $n -lt 60 ]; do
		[ "$($TMUX display -p -t "$1" '#{pane_dead}' | tr -d '\r')" = 1 ] &&
			break
		sleep 0.5
		n=$((n + 1))
	done
	$TMUX display -p -t "$1" '#{pane_dead} #{pane_dead_status} #{pane_dead_signal}' |
	    tr -d '\r'
}

# Print everything a pane showed.
pane_text() {
	$TMUX capture-pane -p -J -S - -t "$1" | tr -d '\r'
}

# Wait for a file to contain a string.
wait_file() {
	n=0
	while [ $n -lt 40 ]; do
		grep -q "$2" "$1" 2>/dev/null && return 0
		sleep 0.5
		n=$((n + 1))
	done
	return 1
}

start_server "$CMD_EXE"

# 1. run-shell: output and exit status.
R=$($TMUX run-shell 'echo run-ok' | tr -d '\r')
[ "$R" = "run-ok" ] && pass "run-shell output" ||
	fail "run-shell output: '$R'"
$TMUX run-shell 'exit 3' >$OUT/rs 2>&1
RC=$?
if [ $RC -eq 3 ] && grep -q "returned 3" $OUT/rs; then
	pass "run-shell exit status"
else
	fail "run-shell exit status: $RC $(cat $OUT/rs)"
fi

# 2. run-shell -b.
$TMUX run-shell -b "echo bg-ok> \"$WOUT\\bg.txt\""
wait_file $OUT/bg.txt bg-ok && pass "run-shell -b" || fail "run-shell -b"

# 3. if-shell (shell form) with cmd.exe, Git Bash and PowerShell as the
# default shell.
for SH in "$CMD_EXE" "$BASH_EXE" "$PWSH_EXE"; do
	[ -z "$SH" ] && continue
	start_server "$SH"
	$TMUX if-shell 'exit 0' 'set -g @t yes' 'set -g @t no'
	T=$($TMUX show -gv @t | tr -d '\r')
	$TMUX if-shell 'exit 1' 'set -g @f yes' 'set -g @f no'
	F=$($TMUX show -gv @f | tr -d '\r')
	if [ "$T" = yes ] && [ "$F" = no ]; then
		pass "if-shell with $SH"
	else
		fail "if-shell with $SH: true=$T false=$F"
	fi
done
start_server "$CMD_EXE"

# 4. #() in a format: the job runs in the background and its output is
# used by the next expansion. Starting a process must not push the
# expansion over its time limit.
R=
n=0
while [ $n -lt 5 ]; do
	R=$($TMUX display -p 'A:#(echo fmt-ok)' \; run -d 1.2 \; \
	    display -p 'B:#(echo fmt-ok)' | tr -d '\r' | grep '^B:')
	[ "$R" = "B:fmt-ok" ] && break
	n=$((n + 1))
done
[ "$R" = "B:fmt-ok" ] && pass "#() expands" || fail "#() expands: '$R'"
R=$($TMUX display -p 'X#(echo a)Y' \; run -d 1.2 \; \
    display -p 'X#(echo a)Y#(echo b)Z' \; run -d 1.2 \; \
    display -p 'X#(echo a)Y#(echo b)Z' | tr -d '\r' | tail -1)
[ "$R" = "XaYbZ" ] && pass "several #()" || fail "several #(): '$R'"

# 5. pipe-pane: the command's stdin is the pane output.
$TMUX pipe-pane -t s -o "cat >> \"$WOUT\\pipe.txt\""
$TMUX send-keys -t s 'echo pipe-marker' Enter
wait_file $OUT/pipe.txt pipe-marker && pass "pipe-pane captures output" ||
	fail "pipe-pane captures output"
$TMUX pipe-pane -t s
$TMUX has -t s || fail "server died after pipe-pane"

# 6. copy-pipe: the selection goes to the command's stdin, then EOF.
$TMUX send-keys -t s 'echo COPY-ME-123' Enter
sleep 1
$TMUX copy-mode -t s \; send -t s -X search-backward 'COPY-ME-123' \; \
    send -t s -X select-line \; \
    send -t s -X copy-pipe-and-cancel "cat > \"$WOUT\\copy.txt\""
wait_file $OUT/copy.txt COPY-ME-123 && pass "copy-pipe" || fail "copy-pipe"

# 7. Git Bash as default shell: commands are run with -c and quoted so that
# bash sees them unchanged.
start_server "$BASH_EXE"
$TMUX new-window -d -n b1 'echo a b'
$TMUX new-window -d -n b2 'printf "[%s]" "x y" "a\\b" "*" '"'"'c d'"'"'; echo'
$TMUX new-window -d -n b3 'exit 7'
wait_dead b1 >/dev/null
pane_text b1 | grep -qx 'a b' && pass "bash: echo a b" ||
	fail "bash: echo a b: $(pane_text b1 | head -3)"
wait_dead b2 >/dev/null
pane_text b2 | grep -qxF '[x y][a\b][*][c d]' && pass "bash: quoting" ||
	fail "bash: quoting: $(pane_text b2 | head -3)"
R=$(wait_dead b3)
[ "$R" = "1 7 " ] && pass "bash: exit status" || fail "bash: exit status: $R"

# 8. PowerShell as default shell.
if [ -n "$PWSH_EXE" ]; then
	start_server "$PWSH_EXE"
	$TMUX new-window -d -n p1 'Write-Output "pw-$(1+2)"; exit 5'
	R=$(wait_dead p1)
	if [ "$R" = "1 5 " ] && pane_text p1 | grep -qx 'pw-3'; then
		pass "pwsh: command and exit status"
	else
		fail "pwsh: $R $(pane_text p1 | head -3)"
	fi
fi

# 9. More than one argument is an argument vector: each argument must
# arrive as one argument.
start_server "$CMD_EXE"
$TMUX new-window -d -n a1 -- "$PRINTF_EXE" '[%s]\n' 'arg with space' 'q"q' '*' ''
wait_dead a1 >/dev/null
pane_text a1 >$OUT/a1
if grep -qxF '[arg with space]' $OUT/a1 && grep -qxF '[q"q]' $OUT/a1 &&
    grep -qxF '[*]' $OUT/a1 && grep -qxF '[]' $OUT/a1; then
	pass "argv: MSYS program"
else
	fail "argv: MSYS program: $(cat $OUT/a1)"
fi
if [ -n "$NODE_EXE" ]; then
	$TMUX new-window -d -n a2 -- "$NODE_EXE" -e \
	    'console.log(JSON.stringify(process.argv.slice(1)))' \
	    'arg with space' 'a\b' 'x\' 'q"q' '*'
	wait_dead a2 >/dev/null
	pane_text a2 | grep -qxF '["arg with space","a\\b","x\\","q\"q","*"]' &&
		pass "argv: native program" ||
		fail "argv: native program: $(pane_text a2 | head -3)"
fi

# 10. A .cmd shim (like npm's) in argument form, found in the PATH.
mkdir -p $OUT/shim
printf '@echo off\r\necho SHIM[%%1][%%2][%%3]\r\n' >$OUT/shim/tmuxtestshim.cmd
PATH="$OUT/shim:$PATH" $TMUX new-window -d -n c1 -- tmuxtestshim 'a b' 100% 'x&y'
wait_dead c1 >/dev/null
pane_text c1 | grep -qxF 'SHIM["a b"]["100%"]["x&y"]' && pass ".cmd shim" ||
	fail ".cmd shim: $(pane_text c1 | head -3)"

# 11. Exit status mapping: 256 is not success and a crash is a signal.
$TMUX new-window -d -n e1 'exit 256'
$TMUX new-window -d -n e2 'exit -1073741819'
R=$(wait_dead e1)
[ "$R" = "1 255 " ] && pass "exit 256 is failure" || fail "exit 256: $R"
R=$(wait_dead e2)
[ "$R" = "1  11" ] && pass "access violation is SIGSEGV" ||
	fail "access violation: $R"
$TMUX if-shell 'exit 256' 'set -g @z yes' 'set -g @z no'
R=$($TMUX show -gv @z | tr -d '\r')
[ "$R" = no ] && pass "if-shell exit 256 is false" ||
	fail "if-shell exit 256: $R"

# 12. tmux -c runs the command with the default shell and exits with its
# status.
R=$($TMUX -c 'echo exec-ok' </dev/null | tr -d '\r')
[ "$R" = "exec-ok" ] && pass "tmux -c output" || fail "tmux -c output: '$R'"
$TMUX -c 'exit 5' </dev/null
RC=$?
[ $RC -eq 5 ] && pass "tmux -c exit status" || fail "tmux -c exit status: $RC"

$TMUX has -t s || fail "server is not running at the end"
$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo "ALL JOB TESTS PASSED"
	exit 0
fi
echo "SOME JOB TESTS FAILED"
exit 1
