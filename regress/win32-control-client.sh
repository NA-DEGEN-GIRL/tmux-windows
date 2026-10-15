#!/bin/sh

# Control mode test for Windows.
# Upstream control-client-sanity.sh exercises -C mode with a sequence of
# pane operations. Test 1 runs a control client with piped stdin: it must
# either work or fail promptly with an error, never hang. The remaining
# tests make the same pane operations with ordinary clients and check
# their effects.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-control-client}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

FNULL="-fNUL"
OUT=$(mktemp)
trap 'rm -f "$OUT" "$OUT.err"; $TMUX kill-server 2>/dev/null' 0 1 15
FAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
}

# index PANE: the index of a pane in its window.
index() {
	$TMUX display -p -t "$1" '#{pane_index}' | tr -d '\r'
}

$TMUX $FNULL new -d -sctrl -x 200 -y 200 < /dev/null || exit 1

# --- Test 1: control mode with piped stdin ---
# Keep stdin open briefly after the commands, as a controlling program
# would, then close it; the client must exit by itself.
{
	printf 'display -p CONTROL_OK\n'
	sleep 2
} | timeout 20 $TMUX -C attach -tctrl >$OUT 2>$OUT.err
RC=$?
tr -d '\r' <$OUT >$OUT.tmp && mv $OUT.tmp $OUT
if [ $RC -eq 124 ]; then
	fail "1: tmux -C did not exit (hung) after stdin closed"
elif grep -qx CONTROL_OK $OUT; then
	if grep -q '^%begin' $OUT && grep -q '^%end' $OUT; then
		echo "PASS 1: control mode ran a command"
	else
		fail "1: control mode output is not framed: $(head -5 $OUT)"
	fi
elif [ $RC -ne 0 ] && [ -s $OUT.err ]; then
	echo "PASS 1: control mode failed promptly ($(head -1 $OUT.err))"
else
	fail "1: tmux -C exited $RC without output or error"
fi
$TMUX has -tctrl || { fail "1: server died after control client"; exit 1; }

# --- Test 2: Complex pane operations (equivalent to control-client-sanity) ---
$TMUX splitw -tctrl || { fail "splitw"; exit 1; }
$TMUX selectp -tctrl -t0 || { fail "selectp 0"; exit 1; }
$TMUX splitw -tctrl || { fail "splitw 2"; exit 1; }
$TMUX neww -tctrl || { fail "neww"; exit 1; }
$TMUX splitw -tctrl || { fail "splitw 3"; exit 1; }
WINDOWS=$($TMUX lsw -tctrl | wc -l)
PANES=$($TMUX lsp -s -tctrl | wc -l)
if [ "$WINDOWS" -eq 2 ] && [ "$PANES" -eq 5 ]; then
	echo "PASS 2: pane operations"
else
	fail "2: $WINDOWS windows and $PANES panes, expected 2 and 5"
fi

# --- Test 3: Kill pane ---
$TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' >$OUT
BEFORE=$(wc -l < $OUT)
KILL_PANE=$(tail -1 $OUT)
$TMUX killp -t"$KILL_PANE" || { fail "killp"; exit 1; }
AFTER=$($TMUX lsp -tctrl | wc -l)
if [ "$AFTER" -eq $((BEFORE - 1)) ] &&
    ! $TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' | grep -qxF "$KILL_PANE"
then
	echo "PASS 3: kill pane ($BEFORE -> $AFTER)"
else
	fail "3: killp $KILL_PANE: $BEFORE -> $AFTER panes"
fi

# --- Test 4: Swap panes ---
$TMUX splitw -tctrl || exit 1
$TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' >$OUT
P1=$(sed -n '1p' $OUT)
P2=$(tail -1 $OUT)
I1=$(index "$P1")
I2=$(index "$P2")
$TMUX swapp -s "$P1" -t "$P2" || { fail "swapp"; exit 1; }
J1=$(index "$P1")
J2=$(index "$P2")
if [ "$I1" != "$I2" ] && [ "$J1" = "$I2" ] && [ "$J2" = "$I1" ]; then
	echo "PASS 4: swap panes ($P1 $I1 -> $J1, $P2 $I2 -> $J2)"
else
	fail "4: swap panes: $P1 $I1 -> $J1, $P2 $I2 -> $J2"
fi

# --- Test 5: Select layout tiled ---
$TMUX neww -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX selectl -tctrl tiled || { fail "selectl tiled"; exit 1; }
COUNT=$($TMUX lsp -tctrl | wc -l)
LAYOUT=$($TMUX display -p -tctrl '#{window_layout}' | tr -d '\r')
# A tiled 2x2 layout is one column of two rows of two panes.
case "$LAYOUT" in
*\[*\{*\}*\{*\}*\]*)
	[ "$COUNT" -eq 4 ] && echo "PASS 5: select-layout tiled ($LAYOUT)" ;;
esac
[ "$COUNT" -eq 4 ] || fail "5: tiled: expected 4 panes, got $COUNT"
case "$LAYOUT" in
*\[*\{*\}*\{*\}*\]*) ;;
*) fail "5: layout is not tiled: $LAYOUT" ;;
esac

# --- Test 6: Kill window ---
WCOUNT_BEFORE=$($TMUX lsw -tctrl | wc -l)
$TMUX killw -tctrl || { fail "killw"; exit 1; }
WCOUNT_AFTER=$($TMUX lsw -tctrl | wc -l)
if [ "$WCOUNT_AFTER" -eq $((WCOUNT_BEFORE - 1)) ]; then
	echo "PASS 6: kill window ($WCOUNT_BEFORE -> $WCOUNT_AFTER)"
else
	fail "6: killw: $WCOUNT_BEFORE -> $WCOUNT_AFTER"
fi

# --- Test 7: Server still alive and every pane in its window's layout ---
$TMUX has -tctrl || { fail "server died"; exit 1; }
$TMUX lsp -s -tctrl -F '#{pane_id} #{window_layout}' | tr -d '\r' >$OUT
BAD=0
while read -r ID LAYOUT; do
	# Each pane in a layout is WxH,X,Y,N where %N is its ID.
	echo "$LAYOUT" | grep -o '[0-9]*x[0-9]*,[0-9]*,[0-9]*,[0-9]*' |
	    sed 's/.*,/%/' | grep -qxF "$ID" ||
	    { BAD=1; fail "7: pane $ID missing from layout $LAYOUT"; }
done <$OUT
[ $BAD -eq 0 ] && echo "PASS 7: server alive, layouts list every pane"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL CONTROL CLIENT TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME CONTROL CLIENT TESTS FAILED"
	exit 1
fi
