#!/bin/sh

# Unicode command-line arguments on Windows.
#
# Without the UTF-8 code page from the application manifest, argv, the
# environment and the narrow Win32 APIs use the ANSI code page (CP949 on a
# Korean system), so Korean session names, option values, -c paths and -e
# values were corrupted, and a tmux.exe in a directory whose name is outside
# the ANSI code page could not start its server.

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)

# A native Python, found before PATH is reset (optional).
PYTHON=
for i in python3 python py; do
	P=$(command -v $i 2>/dev/null) || continue
	case "$P" in
	*/WindowsApps/*) continue ;;	# Store installer stub
	esac
	"$P" -c 'import sys' >/dev/null 2>&1 && PYTHON=$P && break
done

PATH=/bin:/usr/bin
TERM=screen
LABEL=${TMUX_TEST_LABEL:-unicode}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp -d)
COPY="$OUT/tmux 한글 café😀"
trap '$TMUX kill-server 2>/dev/null; "$COPY/tmux.exe" -L$LABEL-copy kill-server 2>/dev/null; rm -rf "$OUT"' 0 1 15

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

# Hex bytes of a string.
hex() {
	printf '%s' "$1" | od -An -tx1 | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'
}

# The line read.sh prints for a string.
dump() {
	printf '%s %s' "$1" "$(printf '%s' "$2" | od -An -tx1)" | sed 's/ *$//'
}

# pane_line TARGET LINE: the pane shows LINE (ignoring trailing spaces).
pane_line() {
	$TMUX capture-pane -p -t "$1" | tr -d '\r' | sed 's/ *$//' |
	    grep -qxF "$2"
}

BASH_W=$(cygpath -w /usr/bin/bash.exe)
KOREAN='한글테스트'

$TMUX -fNUL new -d -s main -x 100 -y 30 </dev/null || exit 1

# --- Test 1: option values keep their UTF-8 bytes ---
$TMUX set -g @k "$KOREAN" || fail "set -g @k"
N=$($TMUX display -p '#{n:@k}' | tr -d '\r')
V=$($TMUX display -p '#{@k}' | tr -d '\r')
if [ "$N" = 15 ] && [ "$V" = "$KOREAN" ]; then
	echo "PASS 1: option value (#{n:@k} = $N)"
else
	fail "option value: length $N, bytes $(hex "$V")"
fi

# --- Test 2: session and window names ---
SNAME='세션😀'
WNAME='창 이름'
if $TMUX new -d -s "$SNAME" -n "$WNAME" -x 80 -y 24 </dev/null; then
	GOT=$($TMUX ls -F '#{session_name}|#{window_name}' |
	    tr -d '\r' | grep -F "$SNAME|")
	if [ "$GOT" = "$SNAME|$WNAME" ] && $TMUX has -t "=$SNAME" 2>/dev/null
	then
		echo "PASS 2: session and window names"
	else
		fail "session and window names: got $(hex "$GOT")"
	fi
else
	fail "new-session -s $SNAME"
fi

# --- Test 3: send-keys -l delivers Korean byte-exact to a pane ---
# The pane runs a bash script that reads one line and prints its bytes and
# the bytes of an environment variable set with -e.
cat >"$OUT/read.sh" <<'EOF'
IFS= read -r line
printf 'LINE %s\n' "$(printf '%s' "$line" | od -An -tx1)"
printf 'ENV %s\n' "$(printf '%s' "$TMUXTEST_VALUE" | od -An -tx1)"
sleep 60
EOF
READER=$(cygpath -w "$OUT/read.sh")
ENVVALUE='값 😀'
$TMUX new-window -d -t main: -n reader -e "TMUXTEST_VALUE=$ENVVALUE" -- \
    "$BASH_W" "$READER" || fail "new-window reader"
sleep 1
SENT='한글 입력'
$TMUX send-keys -t main:reader -l "$SENT"
$TMUX send-keys -t main:reader Enter
if wait_for 10 pane_line main:reader "$(dump LINE "$SENT")"; then
	echo "PASS 3: send-keys -l Korean"
else
	fail "send-keys -l Korean: expected '$(dump LINE "$SENT")'"
	$TMUX capture-pane -p -t main:reader | tr -d '\r' | grep .
fi

# --- Test 4: -e value reaches the pane environment ---
if wait_for 10 pane_line main:reader "$(dump ENV "$ENVVALUE")"; then
	echo "PASS 4: -e value"
else
	fail "-e value: expected '$(dump ENV "$ENVVALUE")'"
fi

# --- Test 3b: characters outside the BMP reach a native program ---
# ConPTY turns them into a surrogate pair of console input records. The
# MSYS2 runtime (bash) does not join such pairs, so use a native reader.
if [ -n "$PYTHON" ]; then
	cat >"$OUT/read.py" <<'EOF'
import sys, time
line = sys.stdin.buffer.readline().rstrip(b'\r\n')
print('LINE ' + ' '.join('%02x' % b for b in line), flush=True)
time.sleep(60)
EOF
	$TMUX new-window -d -t main: -n emoji -- "$(cygpath -w "$PYTHON")" \
	    "$(cygpath -w "$OUT/read.py")" || fail "new-window emoji"
	sleep 1
	SENT='a 한글 😀 b'
	$TMUX send-keys -t main:emoji -l "$SENT"
	$TMUX send-keys -t main:emoji Enter
	if wait_for 10 pane_line main:emoji "LINE $(hex "$SENT")"; then
		echo "PASS 3b: send-keys -l outside the BMP"
	else
		fail "send-keys -l outside the BMP: expected 'LINE $(hex "$SENT")'"
		$TMUX capture-pane -p -t main:emoji | tr -d '\r' | grep .
	fi
else
	echo "SKIP 3b: python not found"
fi

# --- Test 5: -c with a Korean directory ---
DIR="$OUT/한글 폴더"
mkdir -p "$DIR"
DIR_W=$(cygpath -w "$DIR")
$TMUX new-window -d -t main: -n dir -c "$DIR_W" -- cmd.exe /K ||
    fail "new-window -c"
check_dir() {
	[ "$($TMUX display -p -t main:dir '#{pane_current_path}' |
	    tr -d '\r')" = "$DIR_W" ]
}
if wait_for 10 check_dir; then
	echo "PASS 5: -c Korean directory"
else
	fail "-c Korean directory: got '$($TMUX display -p -t main:dir \
	    '#{pane_current_path}' | tr -d '\r')' expected '$DIR_W'"
fi

# --- Test 6: tmux.exe in a directory outside the ANSI code page ---
# The client finds itself with GetModuleFileNameA and starts the server with
# CreateProcessA; both need the UTF-8 code page.
mkdir -p "$COPY"
cp "$TEST_TMUX" "$COPY/tmux.exe" 2>/dev/null ||
    cp "$TEST_TMUX.exe" "$COPY/tmux.exe"
if "$COPY/tmux.exe" -L$LABEL-copy -fNUL new -d -s copy </dev/null &&
    "$COPY/tmux.exe" -L$LABEL-copy has -t copy 2>/dev/null; then
	echo "PASS 6: server started from a Unicode directory"
else
	fail "server started from a Unicode directory"
fi
"$COPY/tmux.exe" -L$LABEL-copy kill-server 2>/dev/null

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL UNICODE TESTS PASSED"
	exit 0
fi
echo ""
echo "SOME UNICODE TESTS FAILED"
exit 1
