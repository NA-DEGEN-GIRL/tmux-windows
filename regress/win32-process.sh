#!/bin/sh

# Windows child processes: the last output and exit status of a pane, the
# environment and working directory children get, which handles they
# inherit, Ctrl-C, many concurrent and short-lived children (exits must
# all be seen and the server must stay idle) and killing whole process
# trees with a pane, while a server started from a pane survives.
# Must be run on Windows (Git Bash).

CMD_EXE=$(cygpath -w "$(command -v cmd.exe)")
PS_EXE=$(command -v powershell.exe)
WPS_EXE=$(cygpath -w "$PS_EXE")
PING_EXE=$(cygpath -w "$(command -v ping.exe)")

PATH=/bin:/usr/bin
TERM=screen

BASH_EXE=$(cygpath -w /usr/bin/bash.exe)

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-process}
TMUX="$TEST_TMUX -L$LABEL"
TMUX2="$TEST_TMUX -L$LABEL-2"
$TMUX kill-server 2>/dev/null
$TMUX2 kill-server 2>/dev/null

OUT=$(mktemp -d)
trap "$TMUX kill-server 2>/dev/null; $TMUX2 kill-server 2>/dev/null; rm -rf $OUT" 0 1 15
WOUT=$(cygpath -w "$OUT")
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

# Count running processes called $1 whose command line contains $2.
count_procs() {
	ps "@(Get-CimInstance Win32_Process -Filter \"Name='$1'\" |
	    Where-Object { \$_.CommandLine -like '*$2*' }).Count"
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

# Wait until a command prints the expected value; print the last value.
wait_for() {
	n=0
	while [ $n -lt ${3:-60} ]; do
		R=$(eval "$1")
		[ "$R" = "$2" ] && break
		sleep 0.5
		n=$((n + 1))
	done
	echo "$R"
}

# Print the first line a pane showed, history included.
first_line() {
	$TMUX capture-pane -p -J -S - -t "$1" | tr -d '\r' | grep -v '^$' | head -1
}

# Milliseconds of CPU time used by a process.
cpu_ms() {
	ps "[int](Get-Process -Id $1).TotalProcessorTime.TotalMilliseconds"
}

# Milliseconds since the epoch.
now_ms() {
	echo $(($(date +%s%N) / 1000000))
}

$TMUX -fNUL start-server \; set -g default-shell "$CMD_EXE" \; \
    set -g remain-on-exit on \; new -d -s s -x 100 -y 30 </dev/null ||
	{ echo "FAIL: cannot start server"; exit 1; }
SERVER_PID=$($TMUX display -p '#{pid}' | tr -d '\r')

# 1. The last output of a pane that exits at once is not lost and the exit
# status is right. The dead message scrolls the screen, so look in the
# history too.
for i in 1 2 3 4 5 6 7 8; do
	$TMUX new-window -d -n d$i "echo done-marker-$i& exit 3"
done
ok=1
for i in 1 2 3 4 5 6 7 8; do
	R=$(wait_dead d$i)
	T=$($TMUX capture-pane -p -S - -t d$i | tr -d '\r' |
	    grep -c "^done-marker-$i")
	if [ "$R" != "1 3 " ] || [ "$T" != 1 ]; then
		ok=0
		fail "remain-on-exit output: d$i: '$R' $T"
	fi
done
[ $ok = 1 ] && pass "remain-on-exit keeps last output and status"

# 2. Ctrl-C interrupts a program started directly in a pane: the server
# ignores Ctrl-C itself and that must not be inherited.
$TMUX new-window -d -n cc "\"$PING_EXE\" -n 60 127.0.0.1"
sleep 2
[ "$($TMUX display -p -t cc '#{pane_dead}' | tr -d '\r')" = 0 ] ||
	fail "ping is not running"
$TMUX send-keys -t cc C-c
R=$(wait_dead cc)
case "$R" in
"1 "*) pass "Ctrl-C interrupts a pane command" ;;
*) fail "Ctrl-C interrupts a pane command: '$R'" ;;
esac

# 3. The session environment reaches jobs and panes, hidden variables do
# not, and Path is there once. run-shell -t shows its output in the pane,
# so use if-shell to test with a target.
$TMUX new -d -s env -e TFOO=bar </dev/null
$TMUX set-environment -t env TBAZ qux
$TMUX set-environment -t env -h THID secret
$TMUX set-environment -g TGLOB gval
$TMUX if-shell -t env \
    'if "%TBAZ%%TFOO%%TGLOB%"=="quxbargval" (exit 0) else (exit 1)' \
    'set -g @envjob yes' 'set -g @envjob no'
[ "$($TMUX show -gv @envjob | tr -d '\r')" = yes ] &&
	pass "jobs get the session environment" ||
	fail "jobs get the session environment"
$TMUX if-shell -t env 'if "%THID%"=="secret" (exit 0) else (exit 1)' \
    'set -g @envhid yes' 'set -g @envhid no'
[ "$($TMUX show -gv @envhid | tr -d '\r')" = no ] &&
	pass "jobs do not get hidden variables" ||
	fail "jobs do not get hidden variables"
R=$($TMUX run-shell "\"$(echo "$WTMUX" | sed 's/#/##/g')\" display -p from-job" |
    tr -d '\r')
[ "$R" = from-job ] && pass "tmux can be run from a job" ||
	fail "tmux can be run from a job: $R"
$TMUX new-window -d -t env: -n envw "set > \"$WOUT\\env.txt\""
wait_dead env:envw >/dev/null
tr -d '\r' <$OUT/env.txt >$OUT/env
if grep -qx 'TFOO=bar' $OUT/env && grep -qx 'TBAZ=qux' $OUT/env &&
    grep -qx 'TGLOB=gval' $OUT/env && grep -q '^TMUX_PANE=%' $OUT/env &&
    ! grep -q '^THID=' $OUT/env; then
	pass "pane environment"
else
	fail "pane environment: $(grep '^T' $OUT/env)"
fi
N=$(grep -ic '^path=' $OUT/env)
[ "$N" = 1 ] && pass "one Path in the environment" ||
	fail "one Path in the environment: $N"

# 4. Working directories: a missing one falls back to home, MSYS and
# forward slash paths work.
HOME_W=$(cygpath -w "$USERPROFILE")
MSYS_NO_PATHCONV=1 $TMUX new-window -d -n cw1 -c 'C:\no\such\directory' 'cd'
MSYS_NO_PATHCONV=1 $TMUX new-window -d -n cw2 -c '/c/Windows' 'cd'
MSYS_NO_PATHCONV=1 $TMUX new-window -d -n cw3 -c 'C:/Windows/System32' 'cd'
for w in cw1 cw2 cw3; do
	wait_dead $w >/dev/null
done
R=$(first_line cw1)
[ "$R" = "$HOME_W" ] && pass "missing directory falls back to home" ||
	fail "missing directory falls back to home: '$R' not '$HOME_W'"
R=$(first_line cw2)
[ "$R" = 'C:\Windows' ] && pass "MSYS directory" ||
	fail "MSYS directory: '$R'"
R=$(first_line cw3)
[ "$R" = 'C:\Windows\System32' ] && pass "forward slash directory" ||
	fail "forward slash directory: '$R'"
# A directory longer than MAX_PATH is used by its short name if it has one,
# otherwise home.
D=$OUT
for i in 1 2 3 4 5 6 7 8 9 10; do
	D=$D/abcdefghijklmnopqrstuvwxyz0123
done
mkdir -p $D
WD=$(cygpath -w $D)
$TMUX new-window -d -n cw4 -c "$WD" 'cd' ||
	fail "long directory: new-window failed"
wait_dead cw4 >/dev/null
R=$(first_line cw4)
case "$R" in
*\\ABCDEF~1|"$HOME_W") pass "long directory" ;;
*) fail "long directory: '$R'" ;;
esac
R=$(MSYS_NO_PATHCONV=1 $TMUX run-shell -c 'C:\no\such\directory' 'cd' |
    tr -d '\r')
[ "$R" = "$HOME_W" ] && pass "job directory falls back to home" ||
	fail "job directory falls back to home: '$R'"

# 5. Children inherit only their standard handles, none of tmux's sockets
# (client connections, listeners, other panes).
cat >$OUT/sockets.ps1 <<'EOF'
$code = 'using System; using System.Runtime.InteropServices; public static class H { [DllImport("ws2_32.dll")] public static extern int getsockopt(IntPtr s, int l, int o, byte[] v, ref int n); [DllImport("ws2_32.dll")] public static extern int WSAStartup(ushort v, byte[] d); }'
Add-Type $code
[void][H]::WSAStartup(0x202, (New-Object byte[] 512))
$c = 0
for ($h = 4; $h -lt 16384; $h += 4) {
	$b = New-Object byte[] 4; $n = 4
	if ([H]::getsockopt([IntPtr]$h, 0xffff, 0x1008, $b, [ref]$n) -eq 0) { $c++ }
}
"sockets=$c"
EOF
SOCKPS="\"$WPS_EXE\" -NoProfile -ExecutionPolicy Bypass -File \"$WOUT\\sockets.ps1\""
R=$($TMUX run-shell "$SOCKPS" | tr -d '\r')
[ "$R" = sockets=0 ] && pass "job inherits no sockets" ||
	fail "job inherits no sockets: $R"
$TMUX new-window -d -n sock "$SOCKPS > \"$WOUT\\sock-pane.txt\""
wait_dead sock >/dev/null
R=$(tr -d '\r' <$OUT/sock-pane.txt)
[ "$R" = sockets=0 ] && pass "pane inherits no sockets" ||
	fail "pane inherits no sockets: $R"

# 6. 100 short-lived windows one after another: every exit is seen.
$TMUX new -d -s seq </dev/null
i=0
while [ $i -lt 100 ]; do
	$TMUX new-window -d -t seq: 'exit 4'
	i=$((i + 1))
done
R=$(wait_for "\$TMUX list-panes -s -t seq -F '#{pane_dead}#{pane_dead_status}' |
    tr -d '\r' | grep -c '^14\$'" 100)
[ "$R" = 100 ] && pass "100 sequential exits seen" ||
	fail "100 sequential exits seen: $R"
$TMUX kill-session -t seq

# 7. 70 panes alive at once (more than WaitForMultipleObjects can watch),
# then all exit: every exit is seen. Each waits for a file to appear.
WAITCMD="for /l %i in (1,1,300) do @(if exist \"$WOUT\\go\" exit 5) & \"$PING_EXE\" -n 2 127.0.0.1 >NUL"
$TMUX new -d -s many "$WAITCMD" </dev/null
i=1
while [ $i -lt 70 ]; do
	$TMUX new-window -d -t many: "$WAITCMD"
	i=$((i + 1))
done
sleep 1
R=$($TMUX list-panes -s -t many -F '#{pane_dead}' | tr -d '\r' | grep -c '^0$')
[ "$R" = 70 ] && pass "70 concurrent panes running" ||
	fail "70 concurrent panes running: $R"
touch $OUT/go
R=$(wait_for "\$TMUX list-panes -s -t many -F '#{pane_dead}#{pane_dead_status}' |
    tr -d '\r' | grep -c '^15\$'" 70 90)
[ "$R" = 70 ] && pass "70 concurrent exits seen" ||
	fail "70 concurrent exits seen: $R"
$TMUX kill-session -t many

# 8. The server is idle afterwards.
sleep 1
A=$(cpu_ms $SERVER_PID)
sleep 5
B=$(cpu_ms $SERVER_PID)
[ $((B - A)) -lt 250 ] && pass "server idle ($((B - A)) ms CPU in 5 s)" ||
	fail "server idle: $((B - A)) ms CPU in 5 s"

# 9. Killing a pane kills its whole process tree.
TAG=$((7000 + $$ % 1000))
$TMUX new-window -d -n tree -- "$BASH_EXE" -c "sleep ${TAG}1 & sleep ${TAG}2"
R=$(wait_for "count_procs sleep.exe ' ${TAG}'" 2 20)
[ "$R" = 2 ] || fail "process tree did not start: $R"
$TMUX kill-pane -t tree
R=$(wait_for "count_procs sleep.exe ' ${TAG}'" 0 20)
[ "$R" = 0 ] && pass "kill-pane kills the process tree" ||
	fail "kill-pane kills the process tree: $R left"

# 10. respawn-pane -k does the same and the new command runs.
$TMUX new-window -d -n resp -- "$BASH_EXE" -c "sleep ${TAG}3 & sleep ${TAG}4"
R=$(wait_for "count_procs sleep.exe ' ${TAG}'" 2 20)
$TMUX respawn-pane -k -t resp 'echo respawned& exit 6'
R=$(wait_for "count_procs sleep.exe ' ${TAG}'" 0 20)
[ "$R" = 0 ] && pass "respawn-pane -k kills the process tree" ||
	fail "respawn-pane -k kills the process tree: $R left"
R=$(wait_dead resp)
T=$($TMUX capture-pane -p -S - -t resp | tr -d '\r' | grep -c '^respawned')
if [ "$R" = "1 6 " ] && [ "$T" = 1 ]; then
	pass "respawn-pane -k runs the new command"
else
	fail "respawn-pane -k runs the new command: '$R' $T"
fi

# 11. Killing a session of 20 busy panes is quick.
$TMUX new -d -s twenty "\"$PING_EXE\" -n 600 127.0.0.1 >NUL" </dev/null
i=1
while [ $i -lt 20 ]; do
	$TMUX new-window -d -t twenty: "\"$PING_EXE\" -n 600 127.0.0.1 >NUL"
	i=$((i + 1))
done
sleep 1
A=$(now_ms)
$TMUX kill-session -t twenty
B=$(now_ms)
[ $((B - A)) -lt 2000 ] && pass "kill-session of 20 panes ($((B - A)) ms)" ||
	fail "kill-session of 20 panes took $((B - A)) ms"

# 12. A server started from inside a pane is not killed with the pane.
$TMUX new-window -d -n inner \
    "\"$WTMUX\" -L $LABEL-2 -fNUL new -d -s x & \"$PING_EXE\" -n 600 127.0.0.1 >NUL"
R=$(wait_for "\$TMUX2 has -t x 2>/dev/null && echo yes" yes 20)
[ "$R" = yes ] || fail "server in pane did not start"
$TMUX kill-pane -t inner
sleep 2
$TMUX2 has -t x 2>/dev/null && pass "server started in a pane survives it" ||
	fail "server started in a pane survives it"
$TMUX2 kill-server 2>/dev/null

$TMUX has -t s || fail "server is not running at the end"
$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo "ALL PROCESS TESTS PASSED"
	exit 0
fi
echo "SOME PROCESS TESTS FAILED"
exit 1
