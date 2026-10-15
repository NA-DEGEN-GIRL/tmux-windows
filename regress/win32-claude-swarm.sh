#!/bin/sh

# Claude Code agent-team swarm simulation test.
# Exercises the exact tmux command sequences that Claude Code emits when
# running split-pane agent teams: rapid multi-split, parallel send-keys,
# capture-pane round-trips, pane targeting, and teardown under load, and
# the sequence an agent driver uses to hand a prompt to a session.
# Must be run on Windows (Git Bash or similar).
#
# Usage:
#   TEST_TMUX=./build/win32/tmux.exe bash regress/win32-claude-swarm.sh

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-claude-swarm}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp)
DIR=$(mktemp -d)
trap 'rm -f "$OUT"; rm -rf "$DIR"; $TMUX kill-server 2>/dev/null' 0 1 15

FNULL="-fNUL"
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

server_gone() {
	$TMUX ls 2>&1 | grep -q "no server running"
}

restart() {
	$TMUX kill-server 2>/dev/null
	wait_for 10 server_gone || fail "server did not exit"
}

# pane_line TARGET LINE: the pane shows LINE as a whole line. Commands are
# echoed after a prompt, so this only matches their output.
pane_line() {
	$TMUX capture-pane -p -t "$1" | tr -d '\r' | sed 's/ *$//' |
	    grep -qxF "$2"
}

# --- Test 1: Version gate ---
# Claude Code runs `tmux -V` at startup to detect availability.
VER=$($TEST_TMUX -V 2>&1 | tr -d '\r')
case "$VER" in
	tmux\ *) ;;
	*) fail "tmux -V returned unexpected: $VER"; exit 1 ;;
esac
echo "PASS 1: tmux -V => $VER"

# --- Test 2: has-session on non-existent session ---
# Claude Code checks session existence before creating.
$TMUX has-session -t swarm 2>/dev/null
if [ $? -eq 0 ]; then
	fail "has-session should fail for non-existent session"
	exit 1
fi
echo "PASS 2: has-session correctly reports missing session"

# Run the full swarm lifecycle CYCLES times to catch handle leaks.
CYCLES=3
CYCLE=0
while [ $CYCLE -lt $CYCLES ]; do
	CYCLE=$((CYCLE + 1))
	echo "--- Cycle $CYCLE/$CYCLES ---"

	# --- Test 3: Session create with explicit dimensions ---
	$TMUX $FNULL new-session -d -s swarm -x 120 -y 40 -- "$CMD_W" /K \
	    < /dev/null || {
		fail "cycle $CYCLE: new-session failed"; exit 1
	}

	# --- Test 4: Rapid multi-split (1 horizontal + 2 vertical) ---
	# Claude Code splits once per teammate. Typical team = 3-5 teammates.
	$TMUX split-window -h -t swarm -- "$CMD_W" /K ||
	    { fail "cycle $CYCLE: split-window -h failed"; exit 1; }
	$TMUX split-window -v -t swarm -- "$CMD_W" /K ||
	    { fail "cycle $CYCLE: split-window -v (1) failed"; exit 1; }
	$TMUX split-window -v -t swarm:0.0 -- "$CMD_W" /K ||
	    { fail "cycle $CYCLE: split-window -v (2) failed"; exit 1; }

	# Verify 4 panes exist.
	COUNT=$($TMUX list-panes -t swarm | wc -l)
	[ "$COUNT" -eq 4 ] || { fail "cycle $CYCLE: expected 4 panes, got $COUNT"; exit 1; }
	echo "  PASS 4.$CYCLE: 4 panes created"

	# --- Test 5: Pane targeting with session:window.pane format ---
	# Claude Code addresses each teammate pane individually.
	PANE=0
	while [ $PANE -lt 4 ]; do
		$TMUX send-keys -t "swarm:0.$PANE" "echo SWARM_PANE_${PANE}_OK" Enter || {
			fail "cycle $CYCLE: send-keys to pane $PANE failed"; exit 1
		}
		PANE=$((PANE + 1))
	done

	# --- Test 6: Capture-pane round-trip for each pane ---
	# Each pane must have run its command: look for the output line, not
	# the typed command.
	PANE=0
	while [ $PANE -lt 4 ]; do
		wait_for 15 pane_line "swarm:0.$PANE" "SWARM_PANE_${PANE}_OK" || {
			fail "cycle $CYCLE: capture-pane $PANE missing command output"
			echo "  pane $PANE content:"
			$TMUX capture-pane -t "swarm:0.$PANE" -p | tr -d '\r' | grep . | tail -5
			exit 1
		}
		PANE=$((PANE + 1))
	done
	echo "  PASS 6.$CYCLE: all 4 panes echo round-trip OK"

	# --- Test 7: list-panes format string (Claude Code reads pane metadata) ---
	$TMUX list-panes -t swarm \
		-F '#{pane_index} #{pane_width} #{pane_height} #{pane_active}' \
		| tr -d '\r' >$OUT
	FCOUNT=$(wc -l < $OUT)
	ACTIVE=$(awk '$4 == 1' $OUT | wc -l)
	AREA=$(awk '{s += $2 * $3} END {print s}' $OUT)
	if [ "$FCOUNT" -ne 4 ] || [ "$ACTIVE" -ne 1 ] ||
	    [ "$AREA" -gt $((120 * 40)) ] || [ "$AREA" -lt $((118 * 38)) ]; then
		fail "cycle $CYCLE: list-panes format output: $(cat $OUT)"
		exit 1
	fi
	echo "  PASS 7.$CYCLE: list-panes format output OK"

	# --- Test 8: Rapid teardown while panes are active ---
	$TMUX kill-session -t swarm || { fail "cycle $CYCLE: kill-session failed"; exit 1; }

	# Verify session is gone.
	$TMUX has-session -t swarm 2>/dev/null
	if [ $? -eq 0 ]; then
		fail "cycle $CYCLE: session still exists after kill-session"
		exit 1
	fi
	echo "  PASS 8.$CYCLE: session torn down cleanly"

	restart
done

# --- Test 9: Multi-session (simulates multiple teams) ---
# Claude Code could run multiple teams or restart teams.
echo "--- Multi-session test ---"
$TMUX $FNULL new-session -d -s team-a -x 100 -y 30 -- "$CMD_W" /K < /dev/null || { fail "new-session team-a"; exit 1; }
$TMUX $FNULL new-session -d -s team-b -x 100 -y 30 -- "$CMD_W" /K < /dev/null || { fail "new-session team-b"; exit 1; }

$TMUX split-window -h -t team-a || { fail "split team-a"; exit 1; }
$TMUX split-window -h -t team-b || { fail "split team-b"; exit 1; }

# Verify both sessions have 2 panes each.
CA=$($TMUX list-panes -t team-a | wc -l)
CB=$($TMUX list-panes -t team-b | wc -l)
[ "$CA" -eq 2 ] && [ "$CB" -eq 2 ] || { fail "multi-session: team-a=$CA team-b=$CB panes"; exit 1; }

# Cross-session send-keys + capture: each team sees only its own output.
$TMUX send-keys -t team-a:0.0 "echo TEAM_A" Enter
$TMUX send-keys -t team-b:0.0 "echo TEAM_B" Enter
wait_for 15 pane_line team-a:0.0 TEAM_A || { fail "capture team-a"; exit 1; }
wait_for 15 pane_line team-b:0.0 TEAM_B || { fail "capture team-b"; exit 1; }
pane_line team-a:0.0 TEAM_B && { fail "team-b output in team-a"; exit 1; }

$TMUX kill-session -t team-a
$TMUX kill-session -t team-b
restart
echo "PASS 9: multi-session OK"

# --- Test 10: Rapid create-destroy cycle (handle leak detection) ---
echo "--- Rapid create-destroy test ---"
RAPID=0
while [ $RAPID -lt 5 ]; do
	$TMUX $FNULL new-session -d -s "rapid$RAPID" -x 80 -y 24 < /dev/null || {
		fail "rapid cycle $RAPID: new-session failed"; exit 1
	}
	$TMUX split-window -h -t "rapid$RAPID" || {
		fail "rapid cycle $RAPID: split-window failed"; exit 1
	}
	$TMUX kill-session -t "rapid$RAPID" || {
		fail "rapid cycle $RAPID: kill-session failed"; exit 1
	}
	RAPID=$((RAPID + 1))
done
restart
echo "PASS 10: 5 rapid create-destroy cycles OK"

# --- Test 11: Hand a prompt to an agent session ---
# An agent driver starts the agent in a working directory, loads a prompt
# file into a buffer, pastes it (bracketed) and presses Enter, reads the
# pane, and cleans up. A bash script stands in for the agent: it prints
# the bytes of the line it reads and its working directory.
echo "--- Agent driver test ---"
WORK="$DIR/work dir"
mkdir -p "$WORK"
cat >"$DIR/agent.sh" <<'EOF'
printf 'CWD %s\n' "$(pwd -W)"
printf 'READY\n'
IFS= read -r line
printf 'GOT %s\n' "$(printf '%s' "$line" | tr -d '\033' | od -An -tx1 | tr -d ' \n')"
sleep 60
EOF
printf 'prompt with "quotes" and 한글' >"$DIR/prompt.md"
EXPECT=$(printf 'prompt with "quotes" and 한글' | od -An -tx1 | tr -d ' \n')
$TMUX $FNULL new-session -d -x 220 -y 50 -s agent -c "$(cygpath -w "$WORK")" \
    -- "$BASH_W" "$(cygpath -w "$DIR/agent.sh")" </dev/null ||
    { fail "11: new-session"; exit 1; }
wait_for 15 pane_line agent READY || fail "11: agent did not start"
if timeout 15 $TMUX load-buffer -b prompt "$(cygpath -w "$DIR/prompt.md")" &&
    timeout 15 $TMUX paste-buffer -p -b prompt -t agent &&
    timeout 15 $TMUX send-keys -t agent Enter; then
	# Bracketed paste markers (ESC [200~ / ESC [201~) arrive only if the
	# agent asked for them; the script strips ESC, so allow either.
	got_prompt() {
		$TMUX capture-pane -p -J -t agent -S -4000 | tr -d '\r' |
		    grep '^GOT ' | sed 's/^GOT //; s/^5b3230307e//; s/5b3230317e$//' |
		    grep -qxF "$EXPECT"
	}
	if wait_for 15 got_prompt; then
		echo "PASS 11a: prompt pasted intact"
	else
		fail "11a: prompt not received intact"
		$TMUX capture-pane -p -J -t agent | tr -d '\r' | grep .
	fi
	timeout 15 $TMUX delete-buffer -b prompt || fail "11: delete-buffer"
	$TMUX list-buffers -F '#{buffer_name}' | tr -d '\r' | grep -qx prompt &&
	    fail "11: buffer not deleted"
else
	fail "11: load-buffer, paste-buffer or send-keys failed or hung"
fi
CWD=$($TMUX capture-pane -p -J -t agent | tr -d '\r' | sed -n 's/^CWD //p')
[ "$(echo "$CWD" | tr 'A-Z\\' 'a-z/')" = \
    "$(cygpath -m "$WORK" | tr 'A-Z' 'a-z')" ] ||
    fail "11: agent ran in '$CWD', not '$(cygpath -m "$WORK")'"
$TMUX kill-session -t agent || fail "11: kill-session"
$TMUX has-session -t agent 2>/dev/null && fail "11: session still exists"
[ $FAIL -eq 0 ] && echo "PASS 11: agent driver sequence"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME TESTS FAILED"
	exit 1
fi
