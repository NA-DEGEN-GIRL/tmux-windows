#!/bin/sh

# install-user.ps1: install into a scratch directory with the user PATH
# kept in a scratch registry key, so the real PATH and installation are
# never touched. Checks that:
# - -DryRun changes nothing;
# - the PATH value keeps its unexpanded entries and REG_EXPAND_SZ type and
#   the directory is added once;
# - a relative -InstallDir is taken from the current location;
# - servers running from the tmux.exe being replaced are stopped (whatever
#   their label) and servers from another tmux.exe are left alone;
# - -KeepSessions installs while a server is still running.
# Runs under every PowerShell found (Windows PowerShell 5.1 and pwsh).

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -f "$TEST_TMUX" ] || TEST_TMUX=$TEST_TMUX.exe

# One per line: the paths contain spaces.
SHELLS=
for i in powershell pwsh; do
	P=$(command -v $i 2>/dev/null) && SHELLS="$SHELLS$P
"
done
NL='
'

PATH=/bin:/usr/bin
TERM=screen

if [ -z "$SHELLS" ]; then
	echo "SKIP: PowerShell not found"
	exit 77
fi

LABEL=${TMUX_TEST_LABEL:-regress-install}
SCRIPT=$(cygpath -w "$(pwd)/install-user.ps1")
[ -f install-user.ps1 ] || { echo "run from the source root"; exit 1; }
KEY="Software\\tmux-windows-test-$LABEL"
OUT=$(mktemp -d)
OUT_W=$(cygpath -w "$OUT")
EXE_W=$(cygpath -w "$TEST_TMUX")
for i in $LABEL-a $LABEL.b $LABEL-other; do
	"$TEST_TMUX" -L$i kill-server 2>/dev/null
done

FAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
}

# psh COMMAND: run a PowerShell command, print its output without CRs.
psh() {
	"$PSH" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
	    -Command "$1" </dev/null 2>&1 | tr -d '\r'
}

# install ARGS...: run install-user.ps1 with the scratch registry key.
install() {
	"$PSH" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \
	    "$SCRIPT" -BuildExe "$EXE_W" -EnvironmentKey "$KEY" "$@" \
	    </dev/null 2>&1 | tr -d '\r'
}

reg_path() {
	psh "\$k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('$KEY'); if (\$k) { \$k.GetValueKind('Path').ToString() + '|' + \$k.GetValue('Path', '', 'DoNotExpandEnvironmentNames') }"
}

reg_reset() {
	psh "\$k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('$KEY'); \$k.SetValue('Path', '%USERPROFILE%\\tmux-fake-bin;C:\\tmux-fake-dir', 'ExpandString')" >/dev/null
}

cleanup() {
	IFS=$NL
	for i in $SHELLS; do
		IFS=' '
		PSH=$i
		psh "[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree('$KEY', \$false)" >/dev/null
		break
	done
	"$OUT/inst/tmux.exe" -L$LABEL-a kill-server 2>/dev/null
	"$OUT/inst/tmux.exe" -L$LABEL.b kill-server 2>/dev/null
	"$TEST_TMUX" -L$LABEL-other kill-server 2>/dev/null
	rm -rf "$OUT"
}
trap cleanup 0 1 15

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

no_server() {
	"$1" -L"$2" has 2>&1 | grep -q "no server running"
}

IFS=$NL
for PSH in $SHELLS; do
	IFS=' '
	N=$(basename "$PSH" .exe)
	rm -rf "$OUT/inst" "$OUT/rel"
	reg_reset

	# --- Dry run ---
	install -InstallDir "$OUT_W\\inst" -DryRun >"$OUT/log"
	if [ -e "$OUT/inst" ]; then
		fail "$N: -DryRun created the install directory"
	elif [ "$(reg_path)" != \
	    'ExpandString|%USERPROFILE%\tmux-fake-bin;C:\tmux-fake-dir' ]; then
		fail "$N: -DryRun changed PATH to '$(reg_path)'"
	elif ! grep -qi "What *if" "$OUT/log"; then
		fail "$N: -DryRun printed no actions: $(cat "$OUT/log")"
	else
		echo "PASS: $N dry run"
	fi

	# --- Install and PATH update ---
	install -InstallDir "$OUT_W\\inst" >"$OUT/log"
	WANT="ExpandString|%USERPROFILE%\\tmux-fake-bin;C:\\tmux-fake-dir;$OUT_W\\inst"
	if ! cmp -s "$TEST_TMUX" "$OUT/inst/tmux.exe"; then
		fail "$N: tmux.exe not installed: $(cat "$OUT/log")"
	elif [ "$(reg_path)" != "$WANT" ]; then
		fail "$N: PATH is '$(reg_path)', expected '$WANT'"
	else
		echo "PASS: $N install keeps unexpanded PATH entries"
	fi
	install -InstallDir "$OUT_W\\inst\\" >"$OUT/log"
	[ "$(reg_path)" = "$WANT" ] ||
	    fail "$N: second install changed PATH to '$(reg_path)'"

	# --- Relative -InstallDir ---
	psh "Set-Location -LiteralPath '$OUT_W'; & '$SCRIPT' -BuildExe '$EXE_W' -EnvironmentKey '$KEY' -InstallDir rel -NoPathUpdate" \
	    >"$OUT/log"
	if cmp -s "$TEST_TMUX" "$OUT/rel/tmux.exe"; then
		echo "PASS: $N relative -InstallDir"
	else
		fail "$N: relative -InstallDir: $(cat "$OUT/log")"
	fi

	# --- Running servers ---
	# Two servers from the installed tmux.exe under other labels and one
	# from another tmux.exe.
	INST="$OUT/inst/tmux.exe"
	"$INST" -L$LABEL-a -fNUL new -d -s a </dev/null ||
	    fail "$N: start server a"
	"$INST" -L$LABEL.b -fNUL new -d -s b </dev/null ||
	    fail "$N: start server b"
	"$TEST_TMUX" -L$LABEL-other -fNUL new -d -s o </dev/null ||
	    fail "$N: start other server"
	install -InstallDir "$OUT_W\\inst" -NoPathUpdate >"$OUT/log"
	if ! wait_for 10 no_server "$TEST_TMUX" $LABEL-a ||
	    ! wait_for 10 no_server "$TEST_TMUX" $LABEL.b; then
		fail "$N: servers of the installed tmux.exe still running: $(cat "$OUT/log")"
	elif ! "$TEST_TMUX" -L$LABEL-other has -t o 2>/dev/null; then
		fail "$N: a server of another tmux.exe was stopped"
	elif ! cmp -s "$TEST_TMUX" "$INST"; then
		fail "$N: tmux.exe not replaced: $(cat "$OUT/log")"
	else
		echo "PASS: $N stops only servers of the replaced tmux.exe"
	fi

	# --- -KeepSessions ---
	"$INST" -L$LABEL-a -fNUL new -d -s a </dev/null ||
	    fail "$N: start server a again"
	install -InstallDir "$OUT_W\\inst" -NoPathUpdate -KeepSessions \
	    >"$OUT/log"
	if ! "$INST" -L$LABEL-a has -t a 2>/dev/null; then
		fail "$N: -KeepSessions stopped the server"
	elif ! cmp -s "$TEST_TMUX" "$INST"; then
		fail "$N: -KeepSessions did not install: $(cat "$OUT/log")"
	else
		echo "PASS: $N -KeepSessions"
	fi
	"$INST" -L$LABEL-a kill-server 2>/dev/null
	wait_for 10 no_server "$TEST_TMUX" $LABEL-a
	"$TEST_TMUX" -L$LABEL-other kill-server 2>/dev/null
done

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL INSTALL TESTS PASSED"
	exit 0
fi
echo ""
echo "SOME INSTALL TESTS FAILED"
exit 1
