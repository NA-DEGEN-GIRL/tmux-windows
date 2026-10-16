#!/bin/sh

# Windows client/server IPC: starting the server (-f, -v, one server per
# label), many clients at once, connections that never authenticate, server
# identity checks against a squatted pipe name, non-terminal clients and the
# exact command sequence llm-router-mcp runs from Node.
# Must be run on Windows (Git Bash or similar).

ORIGPATH=$PATH
PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
LABEL=${TMUX_TEST_LABEL:-ipc}
TMUX="$TEST_TMUX -L$LABEL"
$TMUX kill-server 2>/dev/null

DIR=$(mktemp -d)
WDIR=$(cygpath -w "$DIR")
SYSROOT=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")
POWERSHELL="$SYSROOT/System32/WindowsPowerShell/v1.0/powershell.exe"
NODE=$(PATH=$ORIGPATH command -v node)
IMAGE=$(basename "$TEST_TMUX")
WTMUX=$(cygpath -w "$TEST_TMUX")

# Labels used by this script.
L2="$LABEL-2"
L3="$LABEL-3"
LD="$LABEL-d"
LQ="$LABEL-q"
cleanup() {
	for l in $LABEL $L2 $L3 $LD $LQ; do
		$TEST_TMUX -L$l kill-server 2>/dev/null
	done
	rm -rf $DIR
}
trap cleanup 0 1 15

FAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
}
pass() {
	echo "PASS: $1"
}

# Run tmux with a time limit so that a hang is a failure.
t() {
	timeout 20 $TMUX "$@"
}

# Count server processes (started with -D) for a label.
cat >$DIR/servers.ps1 <<'EOF'
param([string]$Image, [string]$Label)
$re = '-' + [regex]::Escape($Label) + '"?( |$)'
@(Get-CimInstance Win32_Process -Filter "Name='$Image'" | Where-Object {
    $_.CommandLine -cmatch '"?-D"? ' -and $_.CommandLine -cmatch $re
}).Count
EOF
servers() {
	"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File \
	    "$WDIR\\servers.ps1" -Image "$IMAGE" -Label "$1" | tr -d '\r'
}

# Wait for a file to exist.
wait_file() {
	n=0
	while [ ! -f "$1" ] && [ $n -lt 200 ]; do
		sleep 0.1
		n=$((n + 1))
	done
	[ -f "$1" ]
}

# 1. Configuration files and -v reach a server started by a client.
printf 'set -g @ipc_one 1\n' >$DIR/one.conf
printf 'set -g @ipc_two 2\n' >$DIR/two.conf
(cd $DIR && timeout 30 $TMUX -f one.conf -f "$WDIR\\two.conf" -vv \
    new -d -s ipc </dev/null) || fail "new-session with -f"
V="$(t show -gv @ipc_one </dev/null | tr -d '\r') $(t show -gv @ipc_two \
    </dev/null | tr -d '\r')"
[ "$V" = "1 2" ] && pass "-f files applied by a new server" ||
    fail "-f files: got '$V'"
if ls $DIR/tmux-server-*.log >/dev/null 2>&1; then
	pass "-vv gives a server log"
else
	fail "-vv: no server log in $(ls $DIR)"
fi
V=$(t show -sv exit-empty </dev/null | tr -d '\r')
[ "$V" = "on" ] && pass "started server exits when empty" ||
    fail "started server exit-empty is '$V'"
t kill-server </dev/null

# 1b. Arguments that are not ASCII arrive intact, although the C runtime gives
# main() its arguments in the ANSI code page: a -f file in a Korean directory
# for a new server, and a Korean option value from a client.
mkdir -p "$DIR/설정 폴더"
printf 'set -g @ipc_ko applied\n' >"$DIR/설정 폴더/ko.conf"
timeout 30 $TMUX -f "$WDIR\\설정 폴더\\ko.conf" new -d -s ipc </dev/null ||
    fail "new-session with a non-ASCII -f path"
V=$(t show -gv @ipc_ko </dev/null 2>&1 | tr -d '\r')
[ "$V" = applied ] && pass "non-ASCII -f path for a new server" ||
    fail "non-ASCII -f path: got '$V'"
t set -g @ipc_arg '한글값' </dev/null
V=$(t show -gv @ipc_arg </dev/null | tr -d '\r' | od -An -tx1 | tr -d ' \n')
E=$(printf '한글값\n' | od -An -tx1 | tr -d ' \n')
[ "$V" = "$E" ] && pass "non-ASCII argument" ||
    fail "non-ASCII argument: got $V, expected $E"
t kill-server </dev/null

t -fNUL new -d -s ipc </dev/null || fail "new-session"

# 2. A connection that never authenticates (or sends rubbish) does not block
# other clients. The port is found from the server process.
PID=$(t display -p '#{pid}' </dev/null | tr -d '\r')
PORT=$("$SYSROOT/System32/NETSTAT.EXE" -ano -p TCP | tr -d '\r' |
    awk -v pid=$PID '$4 == "LISTENING" && $5 == pid { split($2, a, ":");
    print a[2]; exit }')
if [ -z "$PORT" ]; then
	fail "no listening port for server $PID"
else
	"$POWERSHELL" -NoProfile -Command "\
	    \$s = New-Object Net.Sockets.TcpClient('127.0.0.1', $PORT); \
	    \$g = New-Object Net.Sockets.TcpClient('127.0.0.1', $PORT); \
	    \$b = [byte[]](,65 * 300); \$g.GetStream().Write(\$b, 0, 300); \
	    Set-Content '$WDIR\\held' x; Start-Sleep 12" &
	HOLDER=$!
	if wait_file $DIR/held; then
		S=$(date +%s)
		t has-session -t ipc </dev/null
		R=$?
		E=$(($(date +%s) - S))
		if [ $R -eq 0 ] && [ $E -lt 5 ]; then
			pass "silent connection does not block ($E s)"
		else
			fail "has-session with a silent connection: $R ($E s)"
		fi
	else
		fail "could not open raw connections"
	fi
	kill $HOLDER 2>/dev/null
	wait $HOLDER 2>/dev/null
fi

# 3. Many clients at once all get through to the one server.
rm -f $DIR/c.*
i=0
while [ $i -lt 50 ]; do
	case $((i % 3)) in
	0) (t has-session -t ipc </dev/null >$DIR/c.$i 2>&1;
	    echo $? >$DIR/c.$i.rc) & ;;
	1) (t new-window -d -t ipc: </dev/null >$DIR/c.$i 2>&1;
	    echo $? >$DIR/c.$i.rc) & ;;
	2) (t display -p -t ipc 'x' </dev/null >$DIR/c.$i 2>&1;
	    echo $? >$DIR/c.$i.rc) & ;;
	esac
	i=$((i + 1))
done
wait
BAD=$(cat $DIR/c.*.rc | grep -cv '^0$')
if [ "$BAD" -eq 0 ]; then
	pass "50 concurrent clients"
else
	fail "$BAD of 50 concurrent clients failed: $(cat $DIR/c.[0-9] \
	    $DIR/c.[0-9][0-9] | sort | uniq -c | head -3)"
fi
N=$(t list-sessions </dev/null | wc -l)
[ "$N" -eq 1 ] && pass "no stray sessions" || fail "$N sessions"
N=$(servers $LABEL)
[ "$N" = "1" ] && pass "one server process" || fail "$N server processes"
t kill-server </dev/null

# 4. Two clients starting the server at the same moment start only one.
round=0
while [ $round -lt 3 ]; do
	timeout 30 $TEST_TMUX -L$L2 -fNUL new -d -s a </dev/null &
	A=$!
	timeout 30 $TEST_TMUX -L$L2 -fNUL new -d -s b </dev/null &
	B=$!
	wait $A
	RA=$?
	wait $B
	RB=$?
	S=$(timeout 10 $TEST_TMUX -L$L2 list-sessions -F '#{session_name}' \
	    </dev/null | tr -d '\r' | sort | tr '\n' ' ')
	N=$(servers $L2)
	if [ $RA -ne 0 ] || [ $RB -ne 0 ] || [ "$S" != "a b " ] ||
	    [ "$N" != "1" ]; then
		fail "simultaneous start: $RA $RB, sessions '$S', $N servers"
		break
	fi
	$TEST_TMUX -L$L2 kill-server 2>/dev/null
	round=$((round + 1))
done
[ $round -eq 3 ] && pass "simultaneous first clients start one server"

# 5. A client that is not on a console cannot attach, and says so at once.
timeout 30 $TEST_TMUX -L$L3 -fNUL new -d -s keep </dev/null
S=$(date +%s)
timeout 20 $TEST_TMUX -L$L3 new -s x </dev/null >$DIR/out 2>&1
R=$?
E=$(($(date +%s) - S))
if [ $R -ne 0 ] && [ $R -ne 124 ] && grep -q 'not a terminal' $DIR/out &&
    [ $E -lt 5 ]; then
	pass "new-session without a terminal fails fast"
else
	fail "new-session without a terminal: $R ($E s): $(cat $DIR/out)"
fi

# 6. When a server we started dies, the client says how, and promptly even if
# the processes in its panes (which must not have inherited its sockets) are
# still alive.
$TEST_TMUX -L$L3 kill-server 2>/dev/null
timeout 60 $TEST_TMUX -L$L3 -fNUL new -d -s held \; wait-for ipc-never \
    </dev/null >$DIR/lost 2>&1 &
W=$!
n=0
until timeout 5 $TEST_TMUX -L$L3 has -t held >/dev/null 2>&1 </dev/null ||
    [ $n -gt 100 ]; do
	sleep 0.1
	n=$((n + 1))
done
PID=$(timeout 10 $TEST_TMUX -L$L3 display -p '#{pid}' </dev/null | tr -d '\r')
S=$(date +%s)
"$SYSROOT/System32/taskkill.exe" //F //PID $PID >/dev/null 2>&1
wait $W
E=$(($(date +%s) - S))
if [ $E -lt 5 ] && grep -q 'server exited unexpectedly (exit code' \
    $DIR/lost; then
	pass "lost server reported ($E s)"
else
	fail "lost server: $E s, '$(cat $DIR/lost)'"
fi

# 7. A user's own -D server stays when empty, like on Unix.
(cd $DIR && $TEST_TMUX -L$LD -fNUL -D </dev/null >/dev/null 2>&1 &)
n=0
until timeout 5 $TEST_TMUX -L$LD show -sv exit-empty >$DIR/out 2>&1 \
    </dev/null || [ $n -gt 50 ]; do
	sleep 0.2
	n=$((n + 1))
done
V=$(tr -d '\r' <$DIR/out)
[ "$V" = "off" ] && pass "-D server does not exit when empty" ||
    fail "-D server exit-empty is '$V'"
$TEST_TMUX -L$LD kill-server 2>/dev/null

# 8. Socket names: -S is checked like -L and $TMUX cannot inject arguments.
timeout 10 $TEST_TMUX -S 'bad"name' ls >$DIR/out 2>&1 </dev/null
R=$?
if [ $R -ne 0 ] && [ $R -ne 124 ] && grep -q 'bad label character' $DIR/out
then
	pass "-S rejects bad names"
else
	fail "-S bad name: $R: $(cat $DIR/out)"
fi
TMUX='x" -f "C:\evil.conf,1,0' timeout 10 $TEST_TMUX ls >$DIR/out 2>&1 \
    </dev/null
R=$?
if [ $R -ne 0 ] && [ $R -ne 124 ] && grep -q 'bad label character' $DIR/out
then
	pass "\$TMUX socket name is checked"
else
	fail "\$TMUX bad name: $R: $(cat $DIR/out)"
fi
timeout 30 $TEST_TMUX -L$L2 -fNUL new -d -s sp </dev/null
SP=$(timeout 10 $TEST_TMUX -L$L2 display -p '#{socket_path}' </dev/null |
    tr -d '\r')
V=$(timeout 10 $TEST_TMUX -S "$SP" list-sessions -F '#{session_name}' \
    </dev/null | tr -d '\r')
[ "$V" = "sp" ] && pass "-S #{socket_path} reaches the same server" ||
    fail "-S $SP: '$V'"
V=$(MSYS_NO_PATHCONV=1 timeout 10 $TEST_TMUX -S "/tmp/$L2" \
    list-sessions -F '#{session_name}' </dev/null 2>/dev/null | tr -d '\r')
[ "$V" = "sp" ] && pass "-S path uses the basename" || fail "-S path: '$V'"
$TEST_TMUX -L$L2 kill-server 2>/dev/null

# 9. The client refuses a pipe that is not its own server's: one with
# default permissions, and one with forged permissions but created at low
# integrity (as a sandboxed process would).
SID_NAME=$(timeout 30 $TEST_TMUX -L$LQ -fNUL new -d -s q </dev/null &&
    timeout 10 $TEST_TMUX -L$LQ display -p '#{socket_path}' </dev/null |
    tr -d '\r')
$TEST_TMUX -L$LQ kill-server 2>/dev/null
n=0
while [ "$(servers $LQ)" != "0" ] && [ $n -lt 20 ]; do
	sleep 0.5
	n=$((n + 1))
done
cat >$DIR/squat.ps1 <<'EOF'
param([string]$Name, [string]$Mode, [string]$Ready)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class Squat {
    [StructLayout(LayoutKind.Sequential)]
    struct SA { public int nLength; public IntPtr sd; public int inherit; }
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateNamedPipe(string name, uint openMode,
        uint pipeMode, uint maxInst, uint outBuf, uint inBuf, uint timeout,
        IntPtr sa);
    [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(
        string sddl, uint rev, out IntPtr sd, IntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool ConnectNamedPipe(IntPtr h, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool WriteFile(IntPtr h, byte[] b, uint n, out uint w,
        IntPtr ov);
    public static IntPtr Create(string path, string sddl) {
        IntPtr sa = IntPtr.Zero;
        if (sddl != null) {
            IntPtr sd;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptor(sddl,
                1, out sd, IntPtr.Zero))
                throw new Exception("sddl");
            SA s = new SA();
            s.nLength = Marshal.SizeOf(s);
            s.sd = sd;
            sa = Marshal.AllocHGlobal(Marshal.SizeOf(s));
            Marshal.StructureToPtr(s, sa, false);
        }
        IntPtr h = CreateNamedPipe(path, 3, 0, 255, 256, 256, 0, sa);
        if (h == new IntPtr(-1))
            throw new Exception("CreateNamedPipe");
        return h;
    }
    public static void Serve(IntPtr h) {
        Thread t = new Thread(() => ConnectNamedPipe(h, IntPtr.Zero));
        t.IsBackground = true;
        t.Start();
        if (!t.Join(20000))
            return;
        byte[] r = System.Text.Encoding.ASCII.GetBytes("1:" +
            new string('a', 64) + ":" + new string('b', 64) + "\n");
        uint w;
        WriteFile(h, r, (uint)r.Length, out w, IntPtr.Zero);
        Thread.Sleep(1000);
    }
}
'@
$sddl = $null
if ($Mode -eq 'low') {
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $sddl = "O:${sid}D:P(A;;GA;;;${sid})S:(ML;;NW;;;LW)"
}
$h = [Squat]::Create("\\.\pipe\" + $Name, $sddl)
Set-Content -Path $Ready -Value ready
[Squat]::Serve($h)
EOF
for mode in default low; do
	rm -f $DIR/ready
	"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File \
	    "$WDIR\\squat.ps1" -Name "$SID_NAME" -Mode $mode \
	    -Ready "$WDIR\\ready" >/dev/null 2>&1 &
	SQ=$!
	if ! wait_file $DIR/ready; then
		fail "squatter ($mode) did not start"
		continue
	fi
	timeout 20 $TEST_TMUX -L$LQ -fNUL new -d -s q </dev/null >$DIR/out 2>&1
	R=$?
	case $mode in
	default) WANT='grants access to other users' ;;
	low) WANT='lower integrity' ;;
	esac
	if [ $R -ne 0 ] && [ $R -ne 124 ] && grep -q "$WANT" $DIR/out; then
		pass "refuses a squatted pipe ($mode)"
	else
		fail "squatted pipe ($mode): $R: $(cat $DIR/out)"
	fi
	wait $SQ 2>/dev/null
done
N=$(servers $LQ)
[ "$N" = "0" ] && pass "no server started next to a squatter" ||
    fail "$N servers started next to a squatter"

# 10. From Node with piped stdio (as llm-router-mcp and Claude Code run tmux):
# the full llm-router-mcp sequence, with a Korean prompt over 10 KB pasted
# into a pane, and the time taken by quick commands.
if [ -z "$NODE" ]; then
	echo "SKIP: node not found"
else
	cat >$DIR/sink.js <<'EOF'
// Read what is typed into the pane until the end marker; save it.
const fs = require('fs');
let buf = '', done = false;
// Cooked mode: raw-mode console reads under ConPTY are a separate issue.
process.stdin.setEncoding('utf8');
process.stdin.on('data', (d) => {
	if (done)
		return;
	buf += d;
	const i = buf.indexOf('@@END@@');
	if (i >= 0) {
		fs.writeFileSync(process.argv[2], buf.slice(0, i));
		done = true;
	}
});
EOF
	cat >$DIR/router.js <<'EOF'
// The tmux commands llm-router-mcp runs, through child_process with pipes.
const { spawn } = require('child_process');
const fs = require('fs');
const [tmux, label, dir] = process.argv.slice(2);
function run(args) {
	return new Promise((resolve) => {
		const t0 = process.hrtime.bigint();
		const p = spawn(tmux, ['-L', label, ...args],
		    { stdio: ['pipe', 'pipe', 'pipe'] });
		let out = '', err = '';
		p.stdout.on('data', (d) => out += d);
		p.stderr.on('data', (d) => err += d);
		p.on('close', (code) => resolve({ code, out, err,
		    ms: Number(process.hrtime.bigint() - t0) / 1e6 }));
	});
}
function fail(msg) { console.log('FAIL: ' + msg); process.exitCode = 1; }
function median(a) { a.sort((x, y) => x - y); return a[a.length >> 1]; }
(async () => {
	const name = 'router', prompt = dir + '\\prompt.md';
	const out = dir + '\\pasted.txt', sink = dir + '\\sink.js';
	let line = '';
	for (let i = 0; line.length < 12000; i++)
		line += i + ' 안녕하세요 tmux 붙여넣기 시험 문장입니다.\n';
	fs.writeFileSync(prompt, line + '@@END@@');

	let r = await run(['-V']);
	if (r.code !== 0 || !/^tmux /.test(r.out)) fail('-V: ' + r.out);
	r = await run(['has-session', '-t', name]);
	if (r.code === 0) fail('has-session before new-session');
	r = await run(['-f', 'NUL', 'new-session', '-d', '-x', '220', '-y',
	    '50', '-s', name, '-c', dir, `node "${sink}" "${out}"`]);
	if (r.code !== 0) fail('new-session: ' + r.err);
	r = await run(['has-session', '-t', name]);
	if (r.code !== 0) fail('has-session: ' + r.err);
	await new Promise((res) => setTimeout(res, 1500));
	r = await run(['load-buffer', '-b', 'rbuf', prompt]);
	if (r.code !== 0) fail('load-buffer: ' + r.err);
	r = await run(['paste-buffer', '-p', '-b', 'rbuf', '-t', name]);
	if (r.code !== 0) fail('paste-buffer: ' + r.err);
	r = await run(['send-keys', '-t', name, 'Enter']);
	r = await run(['capture-pane', '-p', '-J', '-t', name, '-S', '-4000']);
	if (r.code !== 0) fail('capture-pane: ' + r.err);
	r = await run(['delete-buffer', '-b', 'rbuf']);
	if (r.code !== 0) fail('delete-buffer: ' + r.err);
	for (let i = 0; i < 100 && !fs.existsSync(out); i++)
		await new Promise((res) => setTimeout(res, 100));
	if (!fs.existsSync(out))
		fail('paste never reached the pane');
	else {
		const got = fs.readFileSync(out, 'utf8')
		    .replace(/\x1b\[20[01]~/g, '').replace(/\r\n?/g, '\n');
		if (got !== line)
			fail(`pasted text differs (${got.length} vs ${line.length})`);
		else
			console.log(`PASS: llm-router sequence, ${line.length} ` +
			    'characters of Korean pasted intact');
	}

	// Quick commands should cost little more than starting tmux at all.
	const t = { '-V': [], 'has-session': [], 'capture-pane': [],
	    'send-keys': [] };
	for (let i = 0; i < 15; i++) {
		t['-V'].push((await run(['-V'])).ms);
		t['has-session'].push((await run(['has-session', '-t', name])).ms);
		t['capture-pane'].push((await run(['capture-pane', '-p', '-J',
		    '-t', name, '-S', '-4000'])).ms);
		t['send-keys'].push((await run(['send-keys', '-t', name,
		    'x'])).ms);
	}
	const base = median(t['-V']);
	const report = Object.entries(t).map(([k, v]) =>
	    `${k} ${median(v).toFixed(0)} ms (max ${Math.max(...v).toFixed(0)})`);
	console.log('latency from node: ' + report.join(', '));
	for (const k of ['has-session', 'capture-pane', 'send-keys'])
		if (median(t[k]) - base > 150)
			fail(`${k} takes ${(median(t[k]) - base).toFixed(0)} ms ` +
			    'more than tmux -V');
	r = await run(['kill-session', '-t', name]);
	if (r.code !== 0) fail('kill-session: ' + r.err);
})();
EOF
	# Panes run the command with the default shell: use cmd.exe, with the
	# user's PATH so it finds node.
	PATH=$ORIGPATH env -u SHELL timeout 180 "$NODE" "$WDIR\router.js" \
	    "$WTMUX" "$L3" "$WDIR" >$DIR/router.out 2>&1
	R=$?
	cat $DIR/router.out
	[ $R -eq 0 ] && ! grep -q '^FAIL' $DIR/router.out ||
	    fail "llm-router sequence ($R)"
fi

# 11. Terminal channels nobody claims are queued only up to a limit; the
# connections beyond it, and the server, must survive being turned away.
if [ -z "$NODE" ]; then
	echo "SKIP: node not found"
else
	cat >$DIR/ttys.js <<'EOF2'
// Open terminal channels that no client will ever claim.
const net = require('net');
const crypto = require('crypto');
const [pipe, count] = [process.argv[2], +process.argv[3]];
function line(sock) {
	return new Promise((resolve, reject) => {
		let buf = '';
		sock.on('data', (d) => {
			buf += d;
			const i = buf.indexOf('\n');
			if (i >= 0)
				resolve(buf.slice(0, i));
		});
		sock.on('error', reject);
		sock.on('close', () => resolve(null));
	});
}
(async () => {
	const held = [];
	let ok = 0;
	for (let i = 0; i < count; i++) {
		const p = net.connect(pipe);
		const reply = await line(p);
		p.destroy();
		if (reply === null || reply === 'busy')
			continue;
		const [port, nonce] = reply.split(':');
		const s = net.connect(+port, '127.0.0.1');
		s.write('TTY:' + crypto.randomBytes(32).toString('hex') + ':' +
		    nonce + '\n');
		if (await line(s) !== null)
			ok++;
		held.push(s);
	}
	console.log(ok);
	setTimeout(() => process.exit(0), 1000);
})().catch((e) => { console.log('error ' + e); process.exit(1); });
EOF2
	t -fNUL new -d -s ttys </dev/null
	SP=$(t display -p '#{socket_path}' </dev/null | tr -d '\r')
	N=$(timeout 60 "$NODE" "$WDIR\\ttys.js" '\\.\pipe\'"$SP" 40 |
	    tr -d '\r')
	if [ "$N" = "40" ] && t has-session -t ttys </dev/null; then
		pass "unclaimed terminal channels"
	else
		fail "unclaimed terminal channels: '$N', server $(t ls 2>&1)"
	fi
fi

# 12. A server that is busy for several seconds (here: its main thread is
# held, as when it is spawning many panes on a loaded machine) is waited for
# rather than reported as failing to authenticate.
cat >$DIR/stall.ps1 <<'EOF'
param([int]$Server, [int]$Ms, [string]$Ready)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Stall {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")]
    public static extern int SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")]
    public static extern int ResumeThread(IntPtr h);
}
'@
$main = (Get-Process -Id $Server).Threads | Sort-Object StartTime, Id |
    Select-Object -First 1
$h = [Stall]::OpenThread(0x0002, $false, $main.Id)
if ([Stall]::SuspendThread($h) -lt 0) { exit 1 }
Set-Content -Path $Ready -Value held
Start-Sleep -Milliseconds $Ms
[Stall]::ResumeThread($h) | Out-Null
EOF
t -fNUL new -d -s busy </dev/null
PID=$(t display -p '#{pid}' </dev/null | tr -d '\r')
rm -f $DIR/stalled
"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$WDIR\\stall.ps1" \
    -Server $PID -Ms 8000 -Ready "$WDIR\\stalled" >/dev/null 2>&1 &
STALL=$!
if wait_file $DIR/stalled; then
	S=$(date +%s)
	timeout 30 $TMUX has-session -t busy </dev/null >$DIR/out 2>&1
	R=$?
	E=$(($(date +%s) - S))
	if [ $R -eq 0 ] && [ $E -ge 5 ]; then
		pass "a busy server is waited for ($E s)"
	else
		fail "busy server: exit $R after $E s: $(cat $DIR/out)"
	fi
else
	fail "could not hold the server's main thread"
fi
wait $STALL 2>/dev/null

for l in $LABEL $L2 $L3 $LD $LQ; do
	$TEST_TMUX -L$l kill-server 2>/dev/null
done
[ $FAIL -eq 0 ] && echo "ALL IPC TESTS PASSED"
exit $FAIL
