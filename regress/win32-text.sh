#!/bin/sh

# Windows text handling regression tests: strftime (crash triggers), POSIX
# regular expressions, fnmatch, UTF-8 character widths, terminal features
# and capabilities (through a nested client), send-keys of non-ASCII text
# and the buffer editor.
#
# The nested client harness runs a client of an inner server inside a pane
# of an outer server, so the outer pane shows what a terminal attached to
# the inner server would see.

ORIG_PATH=$PATH
PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-test}
TMUX="$TEST_TMUX -L$LABEL"
INNER="$TEST_TMUX -L$LABEL-inner"
OUTER="$TEST_TMUX -L$LABEL-outer"
for i in "$TMUX" "$INNER" "$OUTER"; do
	$i kill-server 2>/dev/null
done

FNULL="-fNUL"
OUT=$(mktemp -d)
OUTW=$(cygpath -w "$OUT")
trap "$TMUX kill-server 2>/dev/null; $INNER kill-server 2>/dev/null; \
    $OUTER kill-server 2>/dev/null; rm -rf $OUT" 0 1 15
FAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
}

pass() {
	echo "PASS: $1"
}

# check $name $expected $actual
check() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1: expected '$2', got '$3'"
	fi
}

# The server must survive whatever the last command did.
alive() {
	if $TMUX has-session -tab 2>/dev/null; then
		pass "server alive after $1"
	else
		fail "server died after $1"
		exit 1
	fi
}

# fmt $name $expected $format
fmt() {
	out=$(timeout 10 $TMUX display-message -p -tab "$3" | tr -d '\r')
	check "$1" "$2" "$out"
}

# wait_for $tmux $target $pattern: poll the pane contents.
wait_for() {
	n=0
	while [ $n -lt 100 ]; do
		$1 capture-pane -p -t"$2" | tr -d '\r' | grep -q -- "$3" && \
			return 0
		sleep 0.1
		n=$((n + 1))
	done
	return 1
}

# wait_file $file: poll for a non-empty file.
wait_file() {
	n=0
	while [ $n -lt 100 ]; do
		[ -s "$1" ] && return 0
		sleep 0.1
		n=$((n + 1))
	done
	return 1
}

BASH_W=$(cygpath -w "$(command -v bash)")

$TMUX $FNULL new -d -sab -x100 -y30 </dev/null || exit 1
$TMUX set -g default-shell "$BASH_W"

# --- strftime: conversions the C runtime rejects used to kill the server ---
fmt "percent at end" "50%" "50%"
alive "display -p 50%"
fmt "lone percent" "%" "%"
alive "display -p %"
fmt "percent space" "% x" "% x"
alive "display -p '% x'"
out=$($TMUX display -p -tab '%k|%l|%P|%e|%-I|%-d|%s' | tr -d '\r')
alive "display -p %k %l %P %e %-I %-d %s"
echo "$out" | grep -Eq '^[ 0-9][0-9]\|[ 0-9][0-9]\|[^|]+\|[ 0-9][0-9]\|[0-9]{1,2}\|[0-9]{1,2}\|[0-9]+$' || \
	fail "GNU strftime conversions gave '$out'"
fmt "strftime %D %F %R %T shape" "ok" \
	"#{?#{m/r:^../../.. ....-..-.. ..:.. ..:..:..$,%D %F %R %T},ok,bad}"
fmt "strftime %%" "a%b" "a%%b"
$TMUX set -g status-right '%-I:%M %p %k'
fmt "status-right with %-I and %k" "ok" \
	"#{?#{m/r:^[0-9][0-9]?:[0-9][0-9] .* [ 0-9][0-9]$,#{T:status-right}},ok,bad}"
alive "status-right '%-I:%M %p %k'"
$TMUX set @x 99999999999999999999
$TMUX display -p -tab '#{t/f/%H:@x}' >/dev/null
alive "#{t/f/%H:@x} with a huge time"
$TMUX set @x -99999999999
$TMUX display -p -tab '#{t:@x} #{t/f/%c %x %X %Z %z:@x}' >/dev/null
alive "#{t:@x} with a negative time"
$TMUX setw -g clock-mode-style 12
$TMUX clock-mode -tab
sleep 0.5
alive "12 hour clock mode"
$TMUX send-keys -tab -X cancel 2>/dev/null
$TMUX setw -g clock-mode-style 24

# --- regular expressions (#{s/...} and #{m/r:...}) ---
$TMUX set @v xaby
fmt "regex alternation" "xZbZ" "#{s/a|y/Z/:@v}"
$TMUX set @v xababc
fmt "regex group quantifier" "x<ab>c" "#{s/(ab)+/<\1>/:@v}"
$TMUX set @v foobar
fmt "regex optional group" "[foo]" "#{s/(foo)?bar/[\1]/:@v}"
# An unset group is copied as the digit, as upstream does.
$TMUX set @v bar
fmt "regex unmatched optional group" "[1]" "#{s/(foo)?bar/[\1]/:@v}"
# Braces and colons cannot appear in the pattern of a format modifier, so
# put the pattern in an option.
$TMUX set @p '^ca{2,3}b$'
fmt "regex interval 2" "1" "#{m/r:#{@p},caab}"
fmt "regex interval 3" "1" "#{m/r:#{@p},caaab}"
fmt "regex interval 4" "0" "#{m/r:#{@p},caaaab}"
fmt "regex interval 1" "0" "#{m/r:#{@p},cab}"
fmt "regex bracket class" "1" "#{m/r:^ab[[:digit:]]+c$,ab123c}"
fmt "regex bracket class no match" "0" "#{m/r:^ab[[:digit:]]+c$,abxc}"
fmt "regex negated class" "1" "#{m/r:^[^[:alpha:]]+$,123}"
fmt "regex negated class no match" "0" "#{m/r:^[^[:alpha:]]+$,1a3}"
fmt "regex nested groups" "[ab|a]" "#{s/((a)b)/[\1|\2]/:session_name}"
alive "nested group substitution"
$TMUX set @s baaac
fmt "regex s/^/>/" ">baaac" "#{s/^/>/:@s}"
fmt "regex s/^b/B/" "Baaac" "#{s/^b/B/:@s}"
fmt "regex s/\$/</" "baaac<" "#{s/\$/</:@s}"
fmt "regex s/a*/X/" "bXcX" "#{s/a*/X/:@s}"
fmt "regex s/c\$/C/" "baaaC" "#{s/c\$/C/:@s}"
fmt "regex no match" "baaac" "#{s/z/Q/:@s}"
$TMUX set @k 한글테스트
fmt "regex Hangul dot" "1" "#{m/r:^한.테,#{@k}}"
fmt "regex Hangul dot count" "1" "#{m/r:^.....$,#{@k}}"
fmt "regex Hangul range" "1" "#{m/r:^[가-힣]+$,#{@k}}"
fmt "regex Hangul range on ASCII" "0" "#{m/r:[가-힣],abc}"
fmt "regex Hangul substitution" "한X테스트" "#{s/글/X/:@k}"
fmt "regex dot per character" "XXXXX" "#{s/./X/:@k}"
fmt "regex case-insensitive" "1" "#{m/ri:^HELLO$,hello}"
fmt "regex case-sensitive" "0" "#{m/r:^HELLO$,hello}"
fmt "regex case-insensitive Latin-1" "1" "#{m/ri:^É$,é}"
fmt "regex bad pattern" "0" "#{m/r:(ab,ab}"
alive "bad regex"
A60=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
fmt "regex catastrophic pattern" "0" "#{m/r:(a*)*b,$A60}"
alive "catastrophic regex"

# --- fnmatch (#{m:...}) ---
fmt "fnmatch escaped star" "1" '#{m:a\*b,a*b}'
fmt "fnmatch escaped star no match" "0" '#{m:a\*b,axb}'
fmt "fnmatch ] first in set" "1" "#{m:[]a]x,]x}"
fmt "fnmatch unterminated [" "1" "#{m:a[b,a[b}"
fmt "fnmatch class" "1" "#{m:[[:alpha:]][[:digit:]],b1}"
fmt "fnmatch class no match" "0" "#{m:[[:alpha:]][[:digit:]],11}"
fmt "fnmatch Hangul ?" "1" "#{m:?,한}"
fmt "fnmatch Hangul ??" "0" "#{m:??,한}"
fmt "fnmatch Hangul star" "1" "#{m:*글*,#{@k}}"
fmt "fnmatch case-insensitive" "1" "#{m/i:HELLO,hello}"
fmt "fnmatch many stars" "0" "#{m:*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b,$A60}"

# --- character widths ---
$TMUX set @w 한글
fmt "width Hangul" "4" "#{w:@w}"
$TMUX set @w "$(printf '\360\237\230\200')"
fmt "width emoji" "2" "#{w:@w}"
$TMUX set @w "$(printf '\342\234\205\342\235\214\342\255\220\342\232\241')"
fmt "width BMP emoji" "8" "#{w:@w}"
$TMUX set @w "$(printf '\341\204\222\341\205\241\341\206\253')"
fmt "width conjoining Jamo" "2" "#{w:@w}"
$TMUX set @w "$(printf 'e\314\201')"
fmt "width combining accent" "1" "#{w:@w}"
$TMUX set @w "$(printf '\357\214\233\357\216\205')"
fmt "width Nerd Font icons" "2" "#{w:@w}"
$TMUX set @w "$(printf 'a\342\200\213b')"
fmt "width zero width space" "2" "#{w:@w}"
$TMUX set -as codepoint-widths 'U+1F600=1' && pass "codepoint-widths U+1F600 accepted" || \
	fail "codepoint-widths U+1F600 rejected"
$TMUX set @w "$(printf '\360\237\230\200')"
fmt "codepoint-widths U+1F600=1 effective" "1" "#{w:@w}"
$TMUX set -u codepoint-widths
fmt "codepoint-widths reset" "2" "#{w:@w}"

# Widths as drawn in a pane: the cursor ends after the text.
# pane_width $name $printf-text $expected-cursor-x
pane_width() {
	$TMUX new-window -d -tab: -nw
	wait_for "$TMUX" ab:w '\$' || sleep 2
	$TMUX send-keys -tab:w "clear; printf 'A$2B'; sleep 30" Enter
	n=0
	while [ $n -lt 100 ]; do
		x=$($TMUX display -p -tab:w '#{cursor_y},#{cursor_x}' | tr -d '\r')
		[ "$x" = "0,$3" ] && break
		sleep 0.1
		n=$((n + 1))
	done
	check "$1" "0,$3" "$x"
	$TMUX kill-window -tab:w
}
pane_width "pane width Hangul" '\355\225\234' 4
pane_width "pane width emoji" '\360\237\230\200' 4
pane_width "pane width check mark" '\342\234\205' 4
pane_width "pane width Nerd Font icon" '\357\214\233' 3

# --- copy mode searches with anchors and Hangul ---
$TMUX setw -g mode-keys vi
$TMUX new-window -tab: -nsrch
wait_for "$TMUX" ab:srch '\$' || sleep 2
$TMUX send-keys -tab:srch "clear; printf 'foo foo\\nbar\\nfoo end\\nabc 한글\\n'; sleep 60" Enter
wait_for "$TMUX" ab:srch "^abc" || fail "copy mode search setup"
$TMUX copy-mode -tab:srch
$TMUX send-keys -tab:srch -X top-line
$TMUX send-keys -tab:srch -X start-of-line
$TMUX send-keys -tab:srch -X -N3 cursor-right
$TMUX send-keys -tab:srch -X search-forward '^foo'
fmt "copy mode search ^ not at line start" "0,2" \
	"#{copy_cursor_x},#{copy_cursor_y}"
$TMUX send-keys -tab:srch -X search-forward '[가-힣]+'
fmt "copy mode search Hangul range" "4,3" \
	"#{copy_cursor_x},#{copy_cursor_y}"
$TMUX send-keys -tab:srch -X search-backward 'o .'
fmt "copy mode search backward" "2,2" \
	"#{copy_cursor_x},#{copy_cursor_y}"
timeout 20 $TMUX send-keys -tab:srch -X search-forward '(a*)*b(a*)*z' || \
	fail "catastrophic copy mode search hung"
alive "catastrophic copy mode search"
$TMUX send-keys -tab:srch -X cancel
$TMUX kill-window -tab:srch

# --- send-keys of non-ASCII text arrives byte for byte ---
$TMUX new-window -tab: -nsk
wait_for "$TMUX" ab:sk '\$' || sleep 2
$TMUX send-keys -tab:sk "cat >'$OUTW\\sk.txt'" Enter
sleep 1
$TMUX send-keys -tab:sk -l '한글 é'
$TMUX send-keys -tab:sk Enter C-d
wait_file "$OUT/sk.txt"
sleep 0.5
check "send-keys Hangul and accent" "ed 95 9c ea b8 80 20 c3 a9 0a" \
	"$(od -An -tx1 "$OUT/sk.txt" | tr -s ' \n' '  ' | sed 's/^ //;s/ $//')"
# MSYS cat mangles astral characters from the console itself, so use a
# native reader if there is one.
PY=$(PATH=$ORIG_PATH command -v python 2>/dev/null)
if [ -n "$PY" ]; then
	$TMUX send-keys -tab:sk "'$PY' -c \"import sys; open(r'$OUTW\\py.txt','wb').write(sys.stdin.buffer.readline())\"" Enter
	sleep 2
	$TMUX send-keys -tab:sk -l "$(printf '\360\237\230\200 x')"
	$TMUX send-keys -tab:sk Enter
	wait_file "$OUT/py.txt"
	check "send-keys emoji" "f0 9f 98 80 20 78 0d 0a" \
		"$(od -An -tx1 "$OUT/py.txt" | tr -s ' \n' '  ' | sed 's/^ //;s/ $//')"
else
	echo "SKIP: send-keys emoji (no python)"
fi
$TMUX kill-window -tab:sk

# --- nested client: terminal features, mouse, cursor, clipboard ---
$INNER $FNULL new -d -ss -x100 -y30 </dev/null || exit 1
$INNER set -g status off
$INNER set -g default-shell "$BASH_W"
$INNER set -s set-clipboard on
$INNER set -as terminal-features 'xterm*:sync'
$OUTER $FNULL new -d -so -x120 -y40 </dev/null || exit 1
$OUTER set -s set-clipboard on
wait_for "$OUTER" o '\$' || sleep 2
$OUTER send-keys -to "env -u TMUX -u WT_SESSION -u TERM_PROGRAM TERM=xterm-256color '$TEST_TMUX' -L$LABEL-inner attach -ts" Enter
n=0
while [ $n -lt 100 ]; do
	[ -n "$($INNER list-clients 2>/dev/null)" ] && break
	sleep 0.1
	n=$((n + 1))
done
sleep 1
features=$($INNER list-clients -F '#{client_termfeatures}' | tr -d '\r')
for f in clipboard ccolour cstyle focus title sync; do
	case ",$features," in
	*,$f,*) pass "inner client has feature $f";;
	*) fail "inner client lacks feature $f ($features)";;
	esac
done

check "outer mouse off before" "0" \
	"$($OUTER display -p -to '#{mouse_any_flag}' | tr -d '\r')"
$INNER set -g mouse on
n=0
while [ $n -lt 50 ]; do
	m=$($OUTER display -p -to '#{mouse_any_flag}#{mouse_sgr_flag}' | tr -d '\r')
	[ "$m" = "11" ] && break
	sleep 0.1
	n=$((n + 1))
done
check "mouse on enables SGR mouse reporting" "11" "$m"
$INNER set -g mouse off

$INNER send-keys -ts "printf '\\033[6 q\\033]12;#ff0000\\007'; sleep 60" Enter
n=0
while [ $n -lt 50 ]; do
	c=$($OUTER display -p -to '#{cursor_shape} #{cursor_colour}' | tr -d '\r')
	[ "$c" = "bar #ff0000" ] && break
	sleep 0.1
	n=$((n + 1))
done
check "cursor style and colour reach the terminal" "bar #ff0000" "$c"
$INNER send-keys -ts C-c

$INNER send-keys -ts "clear; echo COPY_ME_42" Enter
wait_for "$INNER" s "^COPY_ME_42" || fail "clipboard setup"
$INNER copy-mode -ts
$INNER send-keys -ts -X search-backward 'COPY_ME_42'
$INNER send-keys -ts -X begin-selection
$INNER send-keys -ts -X -N10 cursor-right
$INNER send-keys -ts -X copy-selection-and-cancel
n=0
while [ $n -lt 50 ]; do
	$OUTER list-buffers -F '#{buffer_sample}' | grep -q COPY_ME_42 && break
	sleep 0.1
	n=$((n + 1))
done
$OUTER list-buffers -F '#{buffer_sample}' | grep -q COPY_ME_42 && \
	pass "OSC 52 copy reaches the terminal" || \
	fail "OSC 52 copy did not reach the terminal"

# The status line and clock are drawn for a client with GNU conversions.
$INNER set -g status on
$INNER set -g status-right '%-I:%M %P %k %l'
$INNER setw -g clock-mode-style 12
$INNER clock-mode -ts
sleep 1
$INNER has-session -ts 2>/dev/null && pass "inner server alive after clock" || \
	fail "inner server died drawing status and clock"
$INNER send-keys -ts q

# --- buffer editor round trip ---
cat >"$OUT/ed.sh" <<'EOF'
cp "$1" "$(dirname "$0")/ed-orig.txt"
printf 'one\r\ntwo\r\nthree\r\n' >"$1"
EOF
printf '@"%s" "%%~dp0ed.sh" %%1\r\n' "$BASH_W" >"$OUT/ed.cmd"
$INNER set -g editor "$OUTW\\ed.cmd"
$INNER set -g default-shell "${COMSPEC:-cmd.exe}"
$INNER set-buffer -b edbuf "$(printf 'x\ny')"
$INNER choose-buffer -ts -F '#{buffer_name}'
sleep 0.5
client=$($INNER list-clients -F '#{client_name}' | tr -d '\r' | head -1)
$INNER send-keys -c"$client" -ts e
n=0
while [ $n -lt 100 ]; do
	b=$($INNER list-buffers -F '#{buffer_name} #{buffer_size}' | \
	    tr -d '\r' | grep '^edbuf ')
	[ "$b" != "edbuf 3" ] && break
	sleep 0.1
	n=$((n + 1))
done
check "buffer editor writes the buffer unchanged" "x \\n y" \
	"$(od -An -c "$OUT/ed-orig.txt" 2>/dev/null | tr -s ' \n' '  ' | sed 's/^ //;s/ $//')"
# CRLF from the editor becomes LF and the final newline is removed.
check "buffer editor reads back the edit" "edbuf 13" "$b"
check "buffer editor content" "one two three" \
	"$($INNER show-buffer -b edbuf | tr -d '\r' | tr '\n' ' ')"
$INNER has-session -ts 2>/dev/null || fail "inner server died in buffer editor"

$OUTER kill-server 2>/dev/null
$INNER kill-server 2>/dev/null
alive "all tests"
$TMUX kill-server 2>/dev/null

[ $FAIL -eq 0 ] && echo "All text tests passed"
exit $FAIL
