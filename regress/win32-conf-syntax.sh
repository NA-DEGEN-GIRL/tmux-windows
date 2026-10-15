#!/bin/sh

# Windows port of upstream conf-syntax.sh.
# Checks that every regress/conf/*.conf file parses.
#
# Upstream uses "source -n" from a client, which hangs on Windows. Instead
# each file is loaded as the configuration of a foreground server (-D, so
# the server reads -f itself), wrapped in a false %if so it is parsed but
# not run. A parse error anywhere rejects the whole file, so the marker
# option set after the %endif is only present if the file parsed.
#
# Each file is checked with LF, with CRLF line endings and with CRLF after a
# UTF-8 byte order mark: configuration files written by Windows editors (and
# checked out with core.autocrlf) use CRLF, and %else, %endif and line
# continuations used to fail with them; Windows PowerShell's "-Encoding utf8"
# and some editors also start the file with a BOM, which used to make the
# first line an unknown command.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-regress-conf-syntax}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

OUT=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; $TMUX kill-server 2>/dev/null; rm -rf "$OUT"' 0 1 15

FAIL=0
COUNT=0

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

query() {
	RESULT=$($TMUX -N show -gv @conftest-marker 2>&1 | tr -d '\r')
	case "$RESULT" in
	*"no server running"*|*"error connecting"*)
		return 1 ;;
	esac
	return 0
}

stopped() {
	! kill -0 $PID 2>/dev/null
}

# parses FILE [crlf|bom]: load FILE in a foreground server with LF (or CRLF,
# or a BOM and CRLF) line endings and check the marker.
parses() {
	{
		echo '%if 0'
		tr -d '\r' <"$1"
		echo
		echo '%endif'
		echo 'set -g @conftest-marker parsed'
	} >"$OUT/wrap.lf"
	if [ "$2" = crlf ]; then
		sed 's/$/\r/' "$OUT/wrap.lf" >"$OUT/wrap.conf"
	elif [ "$2" = bom ]; then
		printf '\357\273\277' >"$OUT/wrap.conf"
		sed 's/$/\r/' "$OUT/wrap.lf" >>"$OUT/wrap.conf"
	else
		cp "$OUT/wrap.lf" "$OUT/wrap.conf"
	fi
	$TEST_TMUX -L$LABEL -f"$(cygpath -w "$OUT/wrap.conf")" -D \
	    </dev/null >/dev/null 2>&1 &
	PID=$!
	RESULT="server did not start"
	wait_for 20 query
	$TMUX kill-server 2>/dev/null
	if ! wait_for 10 stopped; then
		kill $PID 2>/dev/null
		wait_for 5 stopped
	fi
	PID=
	[ "$RESULT" = parsed ]
}

# The check must be able to fail: a file with a syntax error must not set
# the marker, and an empty file must.
: >"$OUT/empty.conf"
if ! parses "$OUT/empty.conf"; then
	echo "FAIL: empty configuration did not load ($RESULT)"
	exit 1
fi
printf 'if-shell true {\n' >"$OUT/broken.conf"
if parses "$OUT/broken.conf"; then
	echo "FAIL: a syntax error was not detected"
	exit 1
fi

if parses "$OUT/broken.conf" crlf; then
	echo "FAIL: a syntax error was not detected (CRLF)"
	exit 1
fi
if parses "$OUT/broken.conf" bom; then
	echo "FAIL: a syntax error was not detected (BOM)"
	exit 1
fi

for i in regress/conf/*.conf; do
	BASENAME=$(basename "$i")
	for EOL in lf crlf bom; do
		if parses "$i" $EOL; then
			echo "PASS: $BASENAME ($EOL)"
		else
			echo "FAIL: $BASENAME ($EOL: $RESULT)"
			FAIL=1
		fi
	done
	COUNT=$((COUNT + 1))
done

if [ $COUNT -eq 0 ]; then
	echo "FAIL: no configuration files found (run from the source root)"
	exit 1
fi
if [ $FAIL -eq 0 ]; then
	echo "ALL $COUNT CONFIG SYNTAX TESTS PASSED"
	exit 0
else
	echo "SOME CONFIG SYNTAX TESTS FAILED"
	exit 1
fi
