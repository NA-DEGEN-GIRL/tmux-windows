#!/bin/sh

# Layout verification tests for Windows.
# Verifies pane dimensions after split operations — not just count.
# Catches upstream bug class e149d29 (uneven space distribution).

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-layout}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

FNULL="-fNUL"
OUT=$(mktemp)
trap 'rm -f "$OUT"; $TMUX kill-server 2>/dev/null' 0 1 15
FAIL=0
TFAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
	TFAIL=1
}

# pass NUMBER DESCRIPTION: report a test that had no failures.
pass() {
	[ $TFAIL -eq 0 ] && echo "PASS $1: $2"
	TFAIL=0
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

panes() {
	$TMUX list-panes -tlayout -F "$1" | tr -d '\r' >$OUT
}

# spread FILE: print "min max" of the numbers in FILE.
spread() {
	sort -n "$1" | sed -n '1p;$p' | tr '\n' ' ' | sed 's/ $//'
}

# --- Test 1: Single pane dimensions match session size ---
$TMUX $FNULL new -d -slayout -x 120 -y 40 < /dev/null || exit 1
panes '#{pane_width} #{pane_height}'
read W H <$OUT
# Detached sessions on Windows have no client, so no status line.
[ "$W" -eq 120 ] || fail "1: single pane width $W != 120"
[ "$H" -eq 40 ] || fail "1: single pane height $H != 40"
pass 1 "single pane 120x40"

# --- Test 2: Horizontal split produces two panes with equal width ---
$TMUX split-window -h -tlayout || exit 1
panes '#{pane_width}'
W1=$(sed -n '1p' $OUT)
W2=$(sed -n '2p' $OUT)
TOTAL=$((W1 + W2 + 1))
[ "$TOTAL" -eq 120 ] || fail "2: h-split widths $W1+$W2+1 = $TOTAL != 120"
DIFF=$((W1 - W2))
[ "$DIFF" -ge -1 ] && [ "$DIFF" -le 1 ] || fail "2: h-split uneven: $W1 vs $W2"
pass 2 "horizontal split ($W1 + $W2 + 1 = $TOTAL)"
restart

# --- Test 3: Vertical split produces two panes with equal height ---
$TMUX $FNULL new -d -slayout -x 80 -y 40 < /dev/null || exit 1
$TMUX split-window -v -tlayout || exit 1
panes '#{pane_height}'
H1=$(sed -n '1p' $OUT)
H2=$(sed -n '2p' $OUT)
TOTAL=$((H1 + H2 + 1))
[ "$TOTAL" -eq 40 ] || fail "3: v-split heights $H1+$H2+1 = $TOTAL != 40"
DIFF=$((H1 - H2))
[ "$DIFF" -ge -1 ] && [ "$DIFF" -le 1 ] || fail "3: v-split uneven: $H1 vs $H2"
pass 3 "vertical split ($H1 + $H2 + 1 = $TOTAL)"
restart

# --- Test 4: Four panes in two columns of two ---
$TMUX $FNULL new -d -slayout -x 100 -y 40 < /dev/null || exit 1
$TMUX split-window -h -tlayout || exit 1
$TMUX split-window -v -tlayout:0.0 || exit 1
$TMUX split-window -v -tlayout:0.2 || exit 1

panes '#{pane_left} #{pane_top} #{pane_width} #{pane_height}'
COUNT=$(wc -l < $OUT)
[ "$COUNT" -eq 4 ] || { fail "4: got $COUNT panes"; exit 1; }
# Two columns: the left one at x 0, the right one after a divider; each
# column is two panes and a divider tall and the columns fill the width.
LEFTS=$(awk '{print $1}' $OUT | sort -nu | tr '\n' ' ')
set -- $LEFTS
if [ $# -ne 2 ] || [ "$1" -ne 0 ]; then
	fail "4: pane columns start at $LEFTS"
else
	XR=$2
	for X in 0 $XR; do
		CW=$(awk -v x=$X '$1 == x {print $3}' $OUT | sort -u)
		CH=$(awk -v x=$X '$1 == x {s += $4} END {print s + 1}' $OUT)
		[ "$(echo "$CW" | wc -l)" -eq 1 ] ||
		    fail "4: column at $X has widths $(echo $CW)"
		[ "$CH" -eq 40 ] || fail "4: column at $X is $CH rows, not 40"
	done
	WL=$(awk '$1 == 0 {print $3; exit}' $OUT)
	WR=$(awk -v x=$XR '$1 == x {print $3; exit}' $OUT)
	[ "$XR" -eq $((WL + 1)) ] || fail "4: right column at $XR, not $((WL + 1))"
	[ $((WL + 1 + WR)) -eq 100 ] ||
	    fail "4: column widths $WL+1+$WR != 100"
fi
pass 4 "four-pane layout"

# --- Test 5: select-layout tiled ---
$TMUX select-layout -tlayout tiled || exit 1
panes '#{pane_width}'
[ "$(wc -l < $OUT)" -eq 4 ] || { fail "5: got $(wc -l < $OUT) panes"; exit 1; }
set -- $(spread $OUT)
[ $(($2 - $1)) -le 1 ] || fail "5: tiled width spread $1-$2"
WS="$1-$2"
panes '#{pane_height}'
set -- $(spread $OUT)
[ $(($2 - $1)) -le 1 ] || fail "5: tiled height spread $1-$2"
pass 5 "tiled layout (w:$WS h:$1-$2)"

# --- Test 6: select-layout even-horizontal ---
$TMUX select-layout -tlayout even-horizontal || exit 1
panes '#{pane_width}'
set -- $(spread $OUT)
[ $(($2 - $1)) -le 1 ] || fail "6: even-horizontal spread $1-$2"
TOTAL=$(awk '{s += $1} END {print s + NR - 1}' $OUT)
[ "$TOTAL" -eq 100 ] || fail "6: even-horizontal widths total $TOTAL"
pass 6 "even-horizontal layout (w:$1-$2)"

# --- Test 7: select-layout even-vertical ---
$TMUX select-layout -tlayout even-vertical || exit 1
panes '#{pane_height}'
set -- $(spread $OUT)
[ $(($2 - $1)) -le 1 ] || fail "7: even-vertical spread $1-$2"
TOTAL=$(awk '{s += $1} END {print s + NR - 1}' $OUT)
[ "$TOTAL" -eq 40 ] || fail "7: even-vertical heights total $TOTAL"
pass 7 "even-vertical layout (h:$1-$2)"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL LAYOUT TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME LAYOUT TESTS FAILED"
	exit 1
fi
