#!/bin/sh

# Windows client file I/O. A client that is not attached (run from a shell or
# a tool) reads and writes files, pipes and its standard input and output for
# the server: load-buffer, save-buffer, show-buffer, source-file and their "-"
# forms. None may hang and output must not have LF turned into CRLF.
# Must be run on Windows (Git Bash or similar).

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -L${TMUX_TEST_LABEL:-files}"
$TMUX kill-server 2>/dev/null

DIR=$(mktemp -d)
trap "rm -rf $DIR; $TMUX kill-server 2>/dev/null" 0 1 15
WDIR=$(cygpath -w "$DIR")

FAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
}
pass() {
	echo "PASS: $1"
}

# Run a tmux command with a time limit so that a hang is a failure.
t() {
	timeout 20 $TMUX "$@"
}

# Compare a file with the expected bytes, or fail showing what was there.
check() {
	if printf "$2" | cmp -s - "$3"; then
		pass "$1"
	else
		fail "$1: got $(od -c "$3" | head -3 | tr '\n' ' ')"
	fi
}

t -fNUL new -d -s files -x 80 -y 24 </dev/null || {
	echo "FAIL: new-session"
	exit 1
}

# 1. load-buffer from a file, show-buffer to stdout.
printf 'hello\nworld' >$DIR/in.txt
t load-buffer -b in "$WDIR\\in.txt" </dev/null || fail "load-buffer FILE"
t show-buffer -b in >$DIR/out.txt </dev/null
check "load-buffer FILE, show-buffer" 'hello\nworld' $DIR/out.txt

# 2. load-buffer from a pipe on stdin.
printf 'a\nb' | t load-buffer -b stdin - || fail "load-buffer - exited $?"
t show-buffer -b stdin >$DIR/out.txt </dev/null
check "load-buffer - from a pipe" 'a\nb' $DIR/out.txt

# 3. A larger pipe, in many reads, arrives intact.
i=0
: >$DIR/big.txt
while [ $i -lt 4000 ]; do
	echo "line $i of the big input with some padding text" >>$DIR/big.txt
	i=$((i + 1))
done
t load-buffer -b big - <$DIR/big.txt || fail "load-buffer - (big)"
t save-buffer -b big - >$DIR/out.txt </dev/null
if cmp -s $DIR/big.txt $DIR/out.txt; then
	pass "load-buffer -, save-buffer - ($(wc -c <$DIR/big.txt) bytes)"
else
	fail "big buffer differs: $(wc -c <$DIR/out.txt) bytes"
fi

# 4. save-buffer to a file truncates, -a appends.
t set-buffer -b s 'first line
second line' </dev/null
t save-buffer -b s "$WDIR\\save.txt" </dev/null || fail "save-buffer FILE"
check "save-buffer FILE" 'first line\nsecond line' $DIR/save.txt
t set-buffer -b s 'xy' </dev/null
t save-buffer -b s "$WDIR\\save.txt" </dev/null
check "save-buffer FILE truncates" 'xy' $DIR/save.txt
t save-buffer -a -b s "$WDIR\\save.txt" </dev/null
check "save-buffer -a FILE appends" 'xyxy' $DIR/save.txt

# 5. Output to stdout has LF line endings.
t display -p -t files 'x#{session_name}' >$DIR/out.txt </dev/null
check "display -p" 'xfiles\n' $DIR/out.txt
t list-sessions -F '#{session_name}' >$DIR/out.txt </dev/null
check "list-sessions" 'files\n' $DIR/out.txt
t send-keys -t files 'echo capture-marker' Enter </dev/null
n=0
while [ $n -lt 100 ]; do
	t capture-pane -p -t files >$DIR/out.txt </dev/null
	grep -q '^capture-marker' $DIR/out.txt && break
	sleep 0.1
	n=$((n + 1))
done
if ! grep -q '^capture-marker' $DIR/out.txt; then
	fail "capture-pane -p: marker not found"
elif od -c $DIR/out.txt | grep -q '\\r'; then
	fail "capture-pane -p has CR"
else
	pass "capture-pane -p"
fi

# 6. source-file from a file, from stdin, and -n (parse only).
printf 'set -g @files_src yes\n' >$DIR/src.conf
t source-file "$WDIR\\src.conf" </dev/null || fail "source-file FILE"
t show -gv @files_src >$DIR/out.txt </dev/null
check "source-file FILE" 'yes\n' $DIR/out.txt
printf 'set -g @files_stdin ok\n' | t source-file - || fail "source-file -"
t show -gv @files_stdin >$DIR/out.txt </dev/null
check "source-file -" 'ok\n' $DIR/out.txt
printf 'set -g @files_n set\n' >$DIR/n.conf
t source -n "$WDIR\\n.conf" </dev/null || fail "source -n exited $?"
t show -gqv @files_n >$DIR/out.txt 2>&1 </dev/null
if grep -q set $DIR/out.txt; then
	fail "source -n ran the file"
else
	pass "source -n parses only"
fi
printf 'set -g @files_bad {\n' >$DIR/bad.conf
t source -n "$WDIR\\bad.conf" >$DIR/out.txt 2>&1 </dev/null
r=$?
if [ $r -eq 0 ] || [ $r -eq 124 ] || ! grep -q 'bad.conf' $DIR/out.txt; then
	fail "source -n of a bad file: exit $r, $(cat $DIR/out.txt)"
else
	pass "source -n reports errors"
fi

# 7. Paths and contents that are not ASCII.
mkdir -p "$DIR/한글 폴더"
printf '안녕하세요\n세계' >"$DIR/한글 폴더/입력.txt"
t load-buffer -b ko "$WDIR\\한글 폴더\\입력.txt" </dev/null ||
    fail "load-buffer non-ASCII path"
t save-buffer -b ko "$WDIR\\한글 폴더\\출력.txt" </dev/null ||
    fail "save-buffer non-ASCII path"
if cmp -s "$DIR/한글 폴더/입력.txt" "$DIR/한글 폴더/출력.txt"; then
	pass "non-ASCII path and contents"
else
	fail "non-ASCII path and contents"
fi

# 8. A missing file is an error, not a hang.
t load-buffer -b none "$WDIR\\missing.txt" >$DIR/out.txt 2>&1 </dev/null
r=$?
if [ $r -eq 0 ] || [ $r -eq 124 ]; then
	fail "load-buffer of a missing file: exit $r"
else
	pass "load-buffer of a missing file fails"
fi

$TMUX kill-server 2>/dev/null
[ $FAIL -eq 0 ] && echo "ALL FILE TESTS PASSED"
exit $FAIL
