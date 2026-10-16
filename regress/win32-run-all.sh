#!/bin/sh

# Run the Windows regression scripts and summarize the results.
#
# Usage (from the source root or anywhere):
#   TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh [NAME...]
#
# Runs every regress/win32-*.sh except this one (or only the NAMEs given,
# such as "basic" or "win32-keys.sh"), each with its own server label
# $TMUX_TEST_PREFIX-<name> so runs do not disturb each other or real
# sessions, under a timeout of $TMUX_TEST_TIMEOUT seconds (default 600).
# A script exiting 77 counts as skipped. Exits 1 if any script failed.

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
case "$TEST_TMUX" in
/*|[A-Za-z]:*) ;;
*) TEST_TMUX=$(pwd)/$TEST_TMUX ;;
esac
if [ ! -f "$TEST_TMUX" ] && [ ! -f "$TEST_TMUX.exe" ]; then
	echo "TEST_TMUX ($TEST_TMUX) not found" >&2
	exit 1
fi
export TEST_TMUX

# Run panes with Git Bash as a user starting tmux from Git Bash would: an
# interactive Git Bash exports SHELL, a CI step does not.
if ! env | grep -q "^SHELL="; then
	SHELL=/bin/bash.exe
	export SHELL
fi

PREFIX=${TMUX_TEST_PREFIX:-run}
LIMIT=${TMUX_TEST_TIMEOUT:-600}

# The scripts expect to run from the source root.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
SELF=$(basename "$0")

if [ $# -gt 0 ]; then
	SCRIPTS=
	for i in "$@"; do
		i=${i##*/}
		i=${i#win32-}
		i=${i%.sh}
		if [ ! -f "regress/win32-$i.sh" ]; then
			echo "no such script: regress/win32-$i.sh" >&2
			exit 1
		fi
		SCRIPTS="$SCRIPTS regress/win32-$i.sh"
	done
else
	SCRIPTS=$(ls regress/win32-*.sh | grep -v "/$SELF\$")
fi

PASSED=
FAILED=
SKIPPED=
for SCRIPT in $SCRIPTS; do
	NAME=$(basename "$SCRIPT" .sh)
	NAME=${NAME#win32-}
	echo "=== $NAME ==="
	START=$(date +%s)
	TMUX_TEST_LABEL=$PREFIX-$NAME timeout -k 10 "$LIMIT" sh "$SCRIPT" \
	    </dev/null 2>&1
	RC=$?
	TIME=$(($(date +%s) - START))
	case $RC in
	0)
		RESULT=PASS
		PASSED="$PASSED $NAME" ;;
	77)
		RESULT=SKIP
		SKIPPED="$SKIPPED $NAME" ;;
	124|137)
		RESULT="FAIL (timed out after ${LIMIT}s)"
		FAILED="$FAILED $NAME" ;;
	*)
		RESULT="FAIL (exit $RC)"
		FAILED="$FAILED $NAME" ;;
	esac
	echo "=== $NAME: $RESULT in ${TIME}s ==="
	echo
	# Stop a server a failed or timed out script left behind.
	"$TEST_TMUX" -L"$PREFIX-$NAME" kill-server >/dev/null 2>&1
done

echo "Summary:"
for NAME in $PASSED; do echo "  PASS $NAME"; done
for NAME in $SKIPPED; do echo "  SKIP $NAME"; done
for NAME in $FAILED; do echo "  FAIL $NAME"; done
set -- $PASSED; NP=$#
set -- $SKIPPED; NS=$#
set -- $FAILED; NF=$#
echo "$NP passed, $NF failed, $NS skipped"
[ $NF -eq 0 ]
