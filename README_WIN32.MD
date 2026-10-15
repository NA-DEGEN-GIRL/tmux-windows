# tmux Windows Port

This document describes the Windows port of tmux, which enables tmux to build and
run on Windows 10+ using MSVC and the Windows ConPTY API.

## Overview

The port preserves the existing Unix codebase almost entirely intact, adding a
Windows platform abstraction layer in the `win32/` directory (10 new source
files, ~4,100 lines) and making targeted `#ifdef _WIN32` modifications across existing
source files.

### Verified Commands

The following tmux commands have been tested and work on Windows:

```
tmux new-session -d -s test
tmux ls
tmux send-keys -t test "echo hello" Enter
tmux capture-pane -t test -p
tmux split-window -t test
tmux list-panes -t test
tmux attach -t test
tmux kill-session -t test
```

Both manual server start (`tmux -D`) and automatic server launch (client spawns
a detached server process) are supported. Terminal attach with full-window
rendering and dynamic resize works correctly. The server survives SSH
disconnects (via job object breakaway), so sessions started over SSH persist
and can be reattached from the local console or a new SSH connection.

## Architecture

### Key Design Decisions

1. **IPC: Named pipe discovery + TCP data path.**
   Windows lacks `AF_UNIX` support in older builds. The server creates a named
   pipe `\\.\pipe\tmux-<user>-<label>` with a kernel-enforced DACL restricting
   access to the current user. Clients connect to the pipe, receive an ephemeral
   nonce and TCP port, then authenticate over TCP with the nonce. The data path
   stays on TCP for libevent `select()` compatibility.

2. **PTY: ConPTY (Windows Pseudo Console) instead of /dev/pty.**
   Child processes are spawned with `CreatePseudoConsole()`. Two bridge threads
   shuttle I/O between the ConPTY pipe handles and a Winsock socket pair so that
   libevent can drive all I/O through `select()`.

3. **Signals: Console events + polling + self-pipe pattern.**
   A console control handler catches Ctrl+C (SIGINT) and window close (SIGTERM).
   A polling thread detects console resize (SIGWINCH). All signals are delivered
   to the libevent loop via a TCP socketpair self-pipe.

4. **Terminal capabilities: Hardcoded xterm-256color database.**
   No ncurses dependency on Windows. About 110 terminfo capabilities are compiled into
   `win32-terminfo.c` with VT escape sequences that Windows Terminal and modern
   consoles support natively.

5. **Process management: Watcher thread instead of SIGCHLD.**
   A background thread monitors child process handles via
   `WaitForMultipleObjects` and delivers exit notifications through the signal
   pipe, emulating `waitpid()` behavior. Exit codes are encoded in Unix
   wait-status format (`code << 8`) so `WIFEXITED`/`WEXITSTATUS` macros work
   unchanged.

6. **File I/O: Direct CRT writes for stdout/stderr.**
   libevent's `select()` on Windows only works with Winsock sockets. The port
   bypasses `bufferevent` for stdout/stderr file descriptors and writes directly
   via `_write()`, preventing the event loop from freezing.

7. **TTY channel separation for attach.**
   On Unix, the client's terminal fd is passed to the server via `SCM_RIGHTS`.
   On Windows, `c->fd` is a TCP socket — it cannot carry terminal semantics.
   Instead, the client performs a second pipe handshake to obtain a nonce and
   TCP port for the TTY channel (`MSG_IDENTIFY_TTYTOKEN`). The server correlates
   this connection to the client and uses it exclusively for escape sequence I/O,
   with `send()` instead of `write()` to avoid CRT invalid parameter aborts
   on Winsock sockets.

8. **Client-driven terminal sizing.**
   The server runs as a `DETACHED_PROCESS` with no console, so
   `GetConsoleScreenBufferInfo()` fails. The client sends its console dimensions
   via `MSG_RESIZE` with a `struct msg_resize_data` payload — both at startup
   (after `MSG_IDENTIFY_DONE`) and on each `SIGWINCH`. The server applies these
   via `tty_set_size()` instead of calling `tty_resize()` (which returns
   immediately on Windows).

## CLI Compatibility

The Windows port accepts the same command-line flags as Unix tmux. The following
cross-platform differences are handled automatically:

| Flag / Feature | Unix | Windows |
|---------------|------|---------|
| `-f /dev/null` | Opens `/dev/null` | Translated to `NUL` automatically |
| `-S /path/to/sock` | Unix domain socket file | Basename extracted as IPC label (warning emitted) |
| `-L name` | Socket label in `/tmp/tmux-$UID/` | IPC label in `%LOCALAPPDATA%\tmux\` |
| `default-shell` | Must start with `/` | Unix paths (starting with `/`) rejected with fallback to `cmd.exe` |
| `$SHELL` | Required to start with `/` | Used as is when set (Git Bash passes a Windows path); otherwise `$COMSPEC`, then `cmd.exe` |
| `$TMUX_TMPDIR` | Controls socket directory | Not used (IPC directory is `%LOCALAPPDATA%\tmux\`) |
| `~/.tmux.conf` | `~` = `$HOME` or pw_dir | `~` = `$USERPROFILE` or `$HOME` or `C:\` |
| `-u` (UTF-8) | Opt-in | Auto-enabled on Windows |

## New Files (win32/)

| File | Lines | Purpose |
|------|------:|---------|
| `win32-platform.h` | 1,106 | POSIX type mappings, constants, signal stubs, struct termios/winsize, function declarations |
| `win32-compat.c` | 837 | POSIX shims: flock, clock_gettime, socketpair, waitpid, kill, wcwidth, fnmatch, tparm (with full conditional support), etc. |
| `win32-ipc.c` | 634 | Named pipe discovery + nonce auth replacing Unix domain sockets, with TCP data path and tty channel support |
| `win32-regex.c` | 429 | Simple POSIX ERE regex engine (character classes, quantifiers, anchors, alternation) |
| `win32-regex.h` | 51 | POSIX regex interface header |
| `win32-pty.c` | 286 | ConPTY wrapper: pseudo-console creation, process spawn, bidirectional bridge threads |
| `win32-process.c` | 284 | Process watcher thread, `win32_launch_server()` (with job object breakaway for SSH survival), child spawn helpers |
| `win32-terminfo.c` | 213 | Hardcoded xterm-256color terminal capability table (about 110 entries) |
| `win32-signal.c` | 157 | Signal emulation via console events, resize polling, self-pipe delivery |
| `win32-tty.c` | 98 | Console mode management: raw VT mode, size queries, UTF-8 code page |
| `tmux.manifest` | | Application manifest: UTF-8 process code page, long path awareness |
| `cmd-parse.cmake` | | Generates `cmd-parse.c` from `cmd-parse.y` with bison |

`osdep-windows.c` (pane foreground process and working directory, UTF-8
argument fallback) is the Windows counterpart of `osdep-linux.c` and friends.

## Unicode

`tmux.exe` embeds `win32/tmux.manifest`, which sets the process code page to
UTF-8 (`activeCodePage`, Windows 10 1903 and later). Command-line arguments,
the environment and the narrow Win32 APIs used throughout tmux
(`CreateProcessA`, `GetModuleFileNameA`, `fopen`, ...) then all use UTF-8, so
Korean or other non-ASCII session names, `send-keys` text, `-c` directories,
`-e` values and install paths work regardless of the system locale. Sources
are compiled with `/utf-8`.

Windows 10 1809 ignores the manifest. There tmux rebuilds `argv` and the
environment from their UTF-16 originals, so arguments still arrive intact,
but file and process APIs keep using the ANSI code page: paths (including the
directory `tmux.exe` is installed in) must then be representable in it.

Configuration files may use LF or CRLF line endings.

## Pane Command And Path

There is no terminal foreground process group on Windows, so
`#{pane_current_command}`, `automatic-rename` and `#{pane_current_path}` look
at the processes attached to the pane's pseudoconsole: the newest process
started by a shell (cmd, PowerShell, bash, ...) is taken as the foreground
one, so `sleep` run from Git Bash or `ping` run from cmd.exe is reported while
it runs, and a program such as vim or an agent keeps the name while it runs
tools of its own. One process snapshot is shared by all panes for 250 ms, and
`automatic-rename` rechecks once a second.

The path is the working directory of that process. PowerShell's
`Set-Location` does not change the process working directory, so add this to
the PowerShell profile (PowerShell 7.2+) for `#{pane_current_path}`, and
`split-window -c "#{pane_current_path}"`, to follow `cd`:

```powershell
$ExecutionContext.InvokeCommand.LocationChangedAction = {
    if ($PWD.Provider.Name -eq 'FileSystem') {
        [Environment]::CurrentDirectory = $PWD.ProviderPath
    }
}
```

## Modified Files

### Core Changes (significant new logic)

| File | Changes |
|------|---------|
| `spawn.c` | ConPTY spawn path; `environ_to_win32_block()` for Unicode env blocks; cmd.exe invocation |
| `compat/imsg-buffer.c` | `WSAGetLastError()` for Winsock non-blocking errors; Windows sendmsg/recvmsg |
| `tmux.c` | Shell detection (COMSPEC), label paths + validation, `/dev/null` → `NUL` for `-f`, `-S` path sanitization, `checkshell()` Unix path guard |
| `job.c` | ConPTY for PTY jobs; `win32_process_spawn()` for non-PTY jobs; no fork/setsid |
| `proc.c` | Signal pipe integration; `proc_fork_and_daemon()` via CreateProcess; closesocket |
| `client.c` | TCP IPC connection with poll-retry; `win32_launch_server()` fallback; tty channel with token auth; client-driven resize via `MSG_RESIZE` |
| `input.c` | Conditional includes; input handling adjustments |
| `server.c` | `win32_ipc_create_server()`; label-based identification; nonce verification; tty channel correlation; bounded pending TTY queue |
| `server-client.c` | IPC client handling; `MSG_RESIZE` with `msg_resize_data` payload; `MSG_IDENTIFY_TTYTOKEN` |
| `tty.c` | `send()` instead of `write()` for tty_raw on Winsock sockets; `CLIENT_DEAD` guard; `tty_resize()` no-op on Windows |
| `server-fn.c` | ConPTY cleanup via `win32_pty_close()` in `server_destroy_pane()` |
| `tmux-protocol.h` | `struct msg_resize_data`, `MSG_IDENTIFY_TTYTOKEN`, Windows-specific message types |
| `cmd-pipe-pane.c` | Windows pipe handling |
| `tty-term.c` | `win32_terminfo_*()` lookups instead of terminfo database |
| `file.c` | Skip bufferevent for stdout/stderr/stdin; direct `_write()` for output |
| `window.c` | ConPTY cleanup on pane destruction |
| `cmd-source-file.c` | Path handling for Windows |

### Minor Changes (conditional includes and guards)

The remaining ~45 files have 2-12 line changes each, primarily:
- `#ifndef _WIN32` guards around Unix-specific includes (`<signal.h>`, `<unistd.h>`, `<sys/socket.h>`, etc.)
- Minor compatibility adjustments (path separators, type casts)
- Safe fallbacks for missing POSIX functions

## Regression Tests

Windows-specific tests are in `regress/`: one native C test (`win32-tparm`,
run by ctest) and the `regress/win32-*.sh` scripts, run from Git Bash in the
source root. `regress/win32-run-all.sh` runs every script (new ones are
picked up automatically), each under its own server label
(`$TMUX_TEST_PREFIX-<name>`) and a timeout, and prints a summary; CI runs it
against both the Debug and the Release build. ctest also registers each
script (label `regress`; the C test has label `unit`).

```bash
TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh
TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh basic keys
TMUX_TEST_LABEL=mytest TEST_TMUX=./build/win32/tmux.exe bash regress/win32-basic.sh
```

```powershell
ctest --test-dir build\win32 -L unit --output-on-failure
```

Every check asserts on command output (for example a computed value printed
by the pane, not the command text that was typed), and waits by polling with
a timeout rather than sleeping.

### win32-tparm (native C test)

Validates numeric expansion and 64-bit string parameters used by terminal
capabilities such as OSC 8 hyperlinks. This guards the Codex `/status` path
that previously crashed the server when a pointer was truncated to 32 bits.

### win32-basic.sh (8 tests)

Session lifecycle, `send-keys` and `capture-pane` (command output), split and
pane exit cleanup (ConPTY paths), detached session size, `-f /dev/null` →
`NUL`, `-S` with a Unix-style path, and the `default-shell` guard against
Unix paths (also from a configuration file when `-f` reaches the server).

### win32-claude-swarm.sh (11 tests)

The command sequences Claude Code agent teams use, over 3 cycles: `tmux -V`,
`has-session`, rapid splits, `send-keys` to every pane with each pane's
output checked, `list-panes -F`, teardown, multiple sessions and rapid
create/destroy; then an agent driver sequence (`new-session -c`,
`load-buffer`, `paste-buffer -p`, `send-keys Enter`, `capture-pane -J -S`,
`delete-buffer`, `kill-session`).

### win32-format-strings.sh (125 assertions)

Port of upstream `format-strings.sh`: escapes, substitutions, conditionals,
comparisons, boolean operators, literals and `#()` format jobs (run through
`default-shell`).

### win32-conf-syntax.sh (21 config files)

Port of upstream `conf-syntax.sh`. Loads each `regress/conf/*.conf` file with
LF and with CRLF line endings in a foreground server and checks that it
parsed (a marker option set after it is present).

### win32-keys.sh (15 tests)

Windows-specific key handling: Enter, literals, Space, Tab, Escape, Ctrl-C,
arrow keys in copy mode, Home/End, PgUp/PgDn, F-keys, Backspace, modifier
combos, mouse wheel key names, ASCII punctuation, and UTF-8 key metadata.
Tests key effects, not raw codes.

### win32-has-session.sh

Port of upstream `has-session-return.sh`: exit codes with no server, with a
missing session, with exact and prefix targets and after `kill-session`.

### win32-layout.sh (7 tests)

Pane geometry after splits and layouts: single pane, h-split, v-split, a
two-by-two arrangement, tiled, even-horizontal, even-vertical.

### win32-control-client.sh (7 tests)

A control client (`-C`) with piped stdin must run a command or fail promptly,
never hang; then the upstream control-client pane operations (splitw,
selectp, killp, swapp, selectl tiled, killw) with their effects checked.

### win32-unicode.sh (7 tests)

Korean option values, session and window names, `send-keys -l` (including
characters outside the BMP), `-e` values, `-c` directories and a `tmux.exe`
in a directory whose name is outside the ANSI code page.

### win32-pane-info.sh

`#{pane_current_command}`, `automatic-rename` and `#{pane_current_path}` for
Git Bash, cmd.exe and PowerShell panes, programs that run shells of their
own (vim, node), and window names from Windows command lines.

### win32-install.sh

`install-user.ps1` with a scratch install directory and a scratch registry
key, under Windows PowerShell and pwsh.

Note: The tests use `OUT` instead of `TMP` for temp file variables to avoid
shadowing the Windows `%TMP%` environment variable (which would cause
`GetTempFileName()` to fail with ACCESS_DENIED in the server process).

## Installation

### Local build install

From a local build tree:

```powershell
.\install-user.ps1 -Build    # configure and build Release, then install
.\install-user.ps1           # install an existing build\release\tmux.exe
.\install-user.ps1 -DryRun   # show what would be done
```

This copies `build\release\tmux.exe` to `%LOCALAPPDATA%\tmux-windows\bin`
(`-InstallDir` to change) and adds that directory to the user `Path` in the
registry, keeping existing `%VAR%` entries unexpanded (`-NoPathUpdate` to
skip). Servers running from the `tmux.exe` being replaced are stopped first,
whatever their `-L` label, so new clients do not attach to a server from the
older binary; servers started from other copies of tmux are left alone. With
`-KeepSessions` running servers are kept and the old `tmux.exe` is moved
aside so the new one can be installed.

### Release zip

If this repository publishes a release artifact:

1. Download the latest `tmux-windows-*.zip` from
   [GitHub Releases](https://github.com/NA-DEGEN-GIRL/tmux-windows/releases)
2. Extract `tmux.exe` to a folder in your `%PATH%`
3. Run `tmux new-session`

### External packages

The `arndawg.tmux-windows` winget package belongs to the reference Windows
port, not this repository. It can be useful for comparison, but it should not
be treated as a release of this fork.

### Upgrading

Run `tmux kill-server` before upgrading release zip or package-manager
installations. Windows cannot overwrite a running executable, and package
upgrades can fail or leave clients attached to an old server if it is
resident. The local `install-user.ps1` helper does this automatically unless
`-KeepSessions` is used.

With the exit-empty fix, the server auto-exits when all sessions are destroyed,
so the server is only resident while sessions are active.

## Build System

The project uses CMake with MSVC and [vcpkg](https://vcpkg.io/) for libevent
(declared in `vcpkg.json`). The canonical local layout uses Ninja, which
builds one configuration per directory chosen with `CMAKE_BUILD_TYPE`
(`--config` is ignored): Debug in `build\win32`, Release in `build\release`.
From a "x64 Native Tools" prompt (or after running `vcvarsall.bat x64`):

```powershell
cmake -S . -B build\win32 -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\win32

cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\release
```

With the Visual Studio vcpkg component, the toolchain file is
`<Visual Studio>\VC\vcpkg\scripts\buildsystems\vcpkg.cmake`. CI uses the
multi-configuration Visual Studio generator instead (`-G "Visual Studio 17
2022"`, then `cmake --build build --config Debug|Release`, binaries in
`build\Debug` and `build\Release`).

The resulting `tmux.exe` is a single statically-linked binary. Copy it to a
directory in `%PATH%` or run `.\install-user.ps1`.

`tmux -V` reports the version in `configure.ac` with a `-win32` suffix, or
`-DTMUX_VERSION=...` when given (release tags set it).

`cmd-parse.c` is generated from `cmd-parse.y`. With `win_bison`
([WinFlexBison](https://github.com/lexxmark/winflexbison)) or `bison` on
`PATH` it is generated into the build directory; otherwise the committed
`cmd-parse.c` is used, and configuring fails if it was not generated from the
current `cmd-parse.y` (its first line records the grammar's hash). After
changing `cmd-parse.y`, refresh the committed copy with
`cmake --build build\win32 --target update-cmd-parse`.

CI builds run automatically on every push via GitHub Actions
(`.github/workflows/ci.yml`). Tagged releases (`v*`) produce downloadable
zip artifacts.

## Debugging

Use the `-v` flag for verbose logging. Each `-v` increases the log level:

```bash
tmux -vvv new-session
```

This creates timestamped log files in the current directory:
- `tmux-client-<pid>.log` — client-side timing (connect, identify, attach)
- `tmux-server-<pid>.log` — server-side timing (socket, auth, ConPTY, events)

Each line has microsecond-precision timestamps (`epoch.usec`) for profiling:

```
1771885428.291080 client started (43248): version next-3.7-win32, ...
1771885428.295389 connect failed, starting server
1771885428.512943 add peer ...: connected
```

## Runtime Requirements

- Windows 10 version 1903+ (1809, the minimum for ConPTY, works but file
  and process APIs use the ANSI code page there; see Unicode)
- Windows Terminal or a VT-capable console host
- No additional DLLs required (libevent is statically linked)

## Security

The Windows port uses named pipes for server discovery and TCP for the data
path. The following hardening measures are in place:

- **Kernel-enforced DACL**: The server's named pipe
  (`\\.\pipe\tmux-<user>-<label>`) is created with a DACL restricting access
  to the current user's SID only. Other users and SYSTEM cannot connect.
- **Ephemeral nonces**: When a client connects to the pipe, the server
  generates a one-time nonce via `RtlGenRandom` and sends it along with the
  TCP port. The nonce expires after 30 seconds and is single-use.
- **Label validation**: `-L` labels are restricted to `[A-Za-z0-9._-]`.
  A `-S` value is not validated; it is passed quoted to the server it
  starts.
- **Bounded TTY queue**: Pending TTY channel connections are capped at 16
  entries with a 30-second expiry, preventing resource exhaustion from
  unclaimed connections.
- **No persistent secrets**: There are no auth files, port files, or tokens
  stored on disk. All authentication state is in-memory and ephemeral.
- **Pipe-before-listen**: The named pipe is created before the TCP listener
  starts, ensuring the port is never disclosed without DACL protection.

No IPC files are written to disk. The named pipe is automatically cleaned up
when the server exits or the pipe handle is closed.

## Known Limitations

- No Unix domain socket support; IPC uses named pipe discovery + TCP data path
- No file descriptor passing between processes; tty channel model used instead
- Regex engine is simplified (covers tmux's pattern matching needs)
- `SIGCHLD` is emulated via process watcher thread; timing may differ from Unix
- Some terminal features may behave differently under ConPTY vs. native Unix PTYs
- Control mode (`-C`) does not work: the client's stdin and stdout are not
  passed to the server, so `tmux -C` waits without output
- tmux output uses `\r\n` line endings; scripts should strip `\r` when comparing output
