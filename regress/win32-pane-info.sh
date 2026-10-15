#!/bin/sh

# pane_current_command, pane_current_path and automatic-rename on Windows.
#
# There is no terminal foreground process group, so tmux looks for the
# processes attached to the pane's pseudoconsole. MSYS2 programs (Git Bash)
# fork and exec through processes that exit, so the command a shell runs has
# a parent that no longer exists; it must still be found. Programs that are
# not shells keep the foreground while they run tools of their own.

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)

# Optional programs, found before PATH is reset.
NODE=$(command -v node 2>/dev/null)
PWSH=$(command -v pwsh 2>/dev/null || command -v powershell 2>/dev/null)

PATH=/bin:/usr/bin
TERM=screen

LABEL=${TMUX_TEST_LABEL:-pane-info}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp -d)
trap '$TMUX kill-server 2>/dev/null; rm -rf "$OUT"' 0 1 15

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

# fmt TARGET FORMAT: expand a format for a pane.
fmt() {
	$TMUX display -p -t "$1" "$2" | tr -d '\r'
}

# is TARGET FORMAT VALUE: the format expands to VALUE (ignoring case).
is() {
	[ "$(fmt "$1" "$2" | tr 'A-Z' 'a-z')" = "$(echo "$3" | tr 'A-Z' 'a-z')" ]
}

# expect SECONDS TARGET FORMAT VALUE DESCRIPTION
expect() {
	if wait_for "$1" is "$2" "$3" "$4"; then
		echo "PASS: $5"
	else
		fail "$5: $3 is '$(fmt "$2" "$3")', expected '$4'"
	fi
}

# never SECONDS TARGET FORMAT VALUE DESCRIPTION: the format stays VALUE.
never_changes() {
	_end=$(($(date +%s) + $1))
	while [ "$(date +%s)" -lt "$_end" ]; do
		if ! is "$2" "$3" "$4"; then
			fail "$5: $3 became '$(fmt "$2" "$3")'"
			return
		fi
		sleep 0.3
	done
	echo "PASS: $5"
}

BASH_W=$(cygpath -w /usr/bin/bash.exe)
DIR="$OUT/dir with space 한글"
mkdir -p "$DIR"
DIR_W=$(cygpath -w "$DIR")

$TMUX -fNUL new -d -s info -x 120 -y 30 -- "$BASH_W" -l </dev/null || exit 1
$TMUX set -g remain-on-exit on

# --- Git Bash ---
expect 10 info:0 '#{pane_current_command}' bash "bash pane is bash"
$TMUX send-keys -t info:0 'sleep 60' Enter
expect 15 info:0 '#{pane_current_command}' sleep "bash running sleep"
# automatic-rename must notice even though sleep prints nothing.
expect 15 info:0 '#{window_name}' sleep "automatic-rename to sleep"
$TMUX send-keys -t info:0 C-c
expect 15 info:0 '#{pane_current_command}' bash "bash after sleep"
expect 15 info:0 '#{window_name}' bash "automatic-rename back to bash"

$TMUX send-keys -t info:0 "cd '$DIR'" Enter
expect 10 info:0 '#{pane_current_path}' "$DIR_W" "bash cwd after cd"

# --- cmd.exe ---
$TMUX new-window -d -t info:1 -- cmd.exe /K
expect 10 info:1 '#{pane_current_command}' cmd "cmd pane is cmd"
$TMUX send-keys -t info:1 "cd /d \"$DIR_W\"" Enter
expect 10 info:1 '#{pane_current_path}' "$DIR_W" "cmd cwd after cd"
# PATH is /bin:/usr/bin here, so name ping by its full path.
$TMUX send-keys -t info:1 '"%SystemRoot%\System32\ping.exe" -n 60 127.0.0.1' \
    Enter
expect 10 info:1 '#{pane_current_command}' ping "cmd running ping"
$TMUX kill-window -t info:1

# --- PowerShell ---
if [ -n "$PWSH" ]; then
	PSNAME=$(basename "$PWSH" .exe)
	$TMUX new-window -d -t info:2 -- "$(cygpath -w "$PWSH")" -NoLogo \
	    -NoProfile
	expect 15 info:2 '#{pane_current_command}' "$PSNAME" \
	    "PowerShell pane is $PSNAME"
	# Set-Location does not change the process directory unless the
	# profile mirrors it (README_WIN32.MD).
	$TMUX send-keys -t info:2 '$ExecutionContext.InvokeCommand.LocationChangedAction = { if ($PWD.Provider.Name -eq "FileSystem") { [Environment]::CurrentDirectory = $PWD.ProviderPath } }' Enter
	$TMUX send-keys -t info:2 "Set-Location -LiteralPath '$DIR_W'" Enter
	expect 15 info:2 '#{pane_current_path}' "$DIR_W" \
	    "PowerShell cwd with LocationChangedAction"
	$TMUX send-keys -t info:2 'Start-Sleep 60' Enter
	sleep 2
	expect 5 info:2 '#{pane_current_command}' "$PSNAME" \
	    "PowerShell running Start-Sleep"
	$TMUX kill-window -t info:2
else
	echo "SKIP: PowerShell not found"
fi

# --- A program that is not a shell keeps the foreground ---
# vim runs ":!" commands through a shell that forks and execs; the pane is
# still running vim.
if [ -x /usr/bin/vim ]; then
	$TMUX new-window -d -t info:3 -- "$BASH_W" -l
	expect 10 info:3 '#{pane_current_command}' bash "second bash pane"
	$TMUX send-keys -t info:3 'vim -u NONE -N' Enter
	expect 15 info:3 '#{pane_current_command}' vim "bash running vim"
	$TMUX send-keys -t info:3 ':!sleep 5; sleep 5' Enter
	sleep 1
	never_changes 6 info:3 '#{pane_current_command}' vim \
	    "vim running a shell command"
	$TMUX kill-window -t info:3
else
	echo "SKIP: vim not found"
fi

# An agent such as Claude Code starts Git Bash for its tools.
if [ -n "$NODE" ]; then
	cat >"$OUT/agent.js" <<'EOF'
require('child_process').execFileSync(process.argv[2],
    ['-c', 'sleep 4; sleep 4'], { stdio: 'inherit' });
setTimeout(function () {}, 60000);
EOF
	$TMUX new-window -d -t info:4 -- "$(cygpath -w "$NODE")" \
	    "$(cygpath -w "$OUT/agent.js")" "$BASH_W"
	expect 10 info:4 '#{pane_current_command}' node "node pane is node"
	never_changes 7 info:4 '#{pane_current_command}' node \
	    "node running bash commands"
	$TMUX kill-window -t info:4
else
	echo "SKIP: node not found"
fi

# --- Window names from Windows command lines ---
# A dead pane falls back to its command; a new window is first named after
# its command or shell. Paths may contain spaces and be quoted.
cp /c/Windows/System32/hostname.exe "$DIR/host name.exe" 2>/dev/null ||
    cp "$(cygpath -u "$SYSTEMROOT")/System32/hostname.exe" "$DIR/host name.exe"
HOST_W=$(cygpath -w "$DIR/host name.exe")
$TMUX set -g default-shell "$(cygpath -w "$COMSPEC")"

$TMUX new-window -d -t info:5 "$HOST_W"
expect 10 info:5 '#{pane_dead}' 1 "unquoted path command exited"
expect 1 info:5 '#{pane_current_command}' "host name" \
    "unquoted path with spaces"

$TMUX new-window -d -t info:6 "\"$HOST_W\" extra arguments"
expect 10 info:6 '#{pane_dead}' 1 "quoted path command exited"
expect 1 info:6 '#{pane_current_command}' "host name" \
    "quoted path with spaces and arguments"

$TMUX set -g default-shell "$HOST_W"
$TMUX set -wg automatic-rename off
$TMUX new-window -d -t info:7
expect 1 info:7 '#{window_name}' "host name" "window named after shell path"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL PANE INFO TESTS PASSED"
	exit 0
fi
echo ""
echo "SOME PANE INFO TESTS FAILED"
exit 1
