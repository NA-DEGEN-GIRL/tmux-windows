# tmux Windows Port

This document describes the Windows port of tmux, which enables tmux to build and
run on Windows 10+ using MSVC and the Windows ConPTY API.

## Overview

The port preserves the existing Unix codebase almost entirely intact, adding a
Windows platform abstraction layer in the `win32/` directory and
`osdep-windows.c`, and making targeted `#ifdef _WIN32` modifications across
existing source files.

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
and can be reattached from the local console or a new SSH connection. A
server started from inside a pane likewise survives that pane being killed.

## What Works On Windows

Most items below have a regression test (see Regression Tests).

### Jobs and shells

- **Jobs**: `run-shell` (and `-b`), `if-shell`, `#()` in formats,
  `pipe-pane` (`-I` and `-O`, with end of file when the pipe is closed),
  `copy-pipe` and popups (`display-popup`, `-E` closes when the command
  exits) work and no longer crash the server. A job's standard handles are
  anonymous pipes bridged to a socket pair by threads, and jobs are started
  on a thread so a slow `CreateProcess` does not stall the event loop.
  `run-shell` output is split on CRLF, so lines have no stray `\r`.
- **default-shell** may be `cmd.exe`, Git Bash (`bin\bash.exe` or
  `usr\bin\bash.exe`), `pwsh.exe`, `powershell.exe` or another shell, given
  as a Windows path to an existing program. cmd.exe gets
  `cmd.exe /s /c "command"`; every other shell gets `-c` with the command
  quoted for its runtime (MSYS2/Cygwin rules for Git Bash, Microsoft C
  runtime rules for native programs). Commands with more than one argument
  (`new-window -- prog arg...`) are run directly with each argument quoted,
  found through the session `PATH` and `PATHEXT`; `.cmd`/`.bat` files (such
  as npm shims) are run through cmd.exe with hardened quoting.
- **Environment**: jobs get the session environment; hidden variables are
  left out. Variable names compare case-insensitively (`Path` and `PATH`
  are one variable, so children never get both).
- **Working directories**: a missing directory falls back to home, MSYS
  style paths (`/c/Users/...`) are accepted and a directory longer than
  `MAX_PATH` is used through its 8.3 short name when it has one.
- **Ctrl-C** reaches programs started directly in panes and popups (the
  server turns Ctrl-C handling back on after `CREATE_NEW_PROCESS_GROUP`).
- **Killing panes**: every pane and job runs in a job object, so
  `kill-pane`, `kill-window`, `kill-session` and `respawn-pane -k` take
  down the whole process tree, like a signal to a process group on Unix.
- **remain-on-exit**: a pane is destroyed only after its pseudoconsole
  output has been read to end of file, so the last output of a command that
  exits at once is not lost and stays visible with `remain-on-exit`.
- **Exit status**: Windows exit codes are mapped to wait statuses. 0-255
  are kept; larger codes are failures (255); small negative codes keep
  their low byte as on Unix (`exit(-1)` is 255); cmd.exe's "is not
  recognized" (9009) is 127; crashes (NTSTATUS exception codes) are reported
  as signals (access violation is SIGSEGV, divide by zero SIGFPE, abort and
  fast fail SIGABRT, Ctrl-C exit SIGINT), as are processes tmux killed.
  `tmux -c command` exits with the command's status.

### Korean and other UTF-8 text

- `tmux.exe` embeds `win32/tmux.manifest`, which sets the process code page
  to UTF-8 (`activeCodePage`, Windows 10 1903 and later). Command-line
  arguments, the environment and the narrow Win32 APIs used throughout
  tmux (`CreateProcessA`, `GetModuleFileNameA`, `fopen`, ...) then all use
  UTF-8, so Korean session and window names, option values, `send-keys`
  text, `-c` directories, `-e` values, `-f` paths, user names and install
  paths work regardless of the system locale. Sources are compiled with
  `/utf-8`.
- Fallback: Windows 10 1809 ignores the manifest. tmux then rebuilds `argv`
  (split by the C runtime's own rules) and the environment from their
  UTF-16 originals and reads `$TMUX` with the wide API, so arguments still
  arrive intact; file and process APIs keep using the ANSI code page, so
  paths (including the directory `tmux.exe` is installed in) must then be
  representable in it. The client starts the server with
  `CreateProcessW`.
- Job output is UTF-8: a job's hidden console has the system OEM code page
  (949 on a Korean system), so when the system OEM code page is not UTF-8,
  cmd.exe jobs are run as
  `cmd /d /v:on /s /c "chcp 65001>nul & cmd /s /c !TMUX_WIN32_JOB!"`, which
  leaves the command line and exit status unchanged.
- Characters are 32-bit code points on Windows (MSVC's `wchar_t` is 16
  bits), so emoji and other characters outside the BMP have the right width,
  `codepoint-widths` accepts them and `send-keys`, `\U` escapes and CSI u
  keys keep them. Character widths come from tables generated from the
  Unicode Character Database (`win32/gen-wcwidth.py`).
- The client sets its console to UTF-8 and restores the original code pages
  and console modes when it exits or detaches.
- Configuration files may use LF or CRLF line endings and may start with a
  UTF-8 byte order mark (as Windows PowerShell's `-Encoding utf8` writes);
  this applies to `-f`, the default configuration and `source-file`.

### Client commands from a shell

- **Client file commands** work from a shell or script (a client that is
  not attached): `source-file FILE` and `source-file -`, `source -n`
  (parse only, reports errors), `load-buffer FILE` and `load-buffer -`
  (stdin, including large pipes), `save-buffer FILE` (`-a` appends) and
  `save-buffer -`, `show-buffer`. Files, pipes and consoles are read by a
  thread feeding a socket pair and written synchronously; paths are UTF-8.
- **Binary output**: tmux's stdout and stderr are in binary mode, so output
  has LF line endings, as on Unix, whether it goes to a console, a pipe or a
  file (no `\r\n`).
- **Auto-started servers** get the client's `-f` files (as absolute paths)
  and `-v` level, start in the home directory (so the client's directory is
  not held open) and exit when the last session is gone, as a forked server
  does on Unix; a server you start yourself with `tmux -D` stays. If the
  server dies while starting, the client reports why (from its standard
  error, kept in `%LOCALAPPDATA%\tmux\<name>.server.log`).
- Only clients with a console on standard input, and control mode clients,
  open the terminal channel, so clients run with pipes (scripts, Node
  `child_process`, agents) never wait for it; `attach` without a console
  fails at once with "not a terminal".

### Attached clients

- **Client names** are unique: a console has no device path, so the client
  tty name is `con<pid>` and `list-clients`, `detach-client -t` and
  `lock-client -t` can tell clients apart.
- **suspend-client** (`C-b C-z`): there is no job control on Windows, so
  the client resumes at once (the screen is redrawn) rather than freezing.
- **lock**: `lock-client`/`lock-server`/`lock-after-time` hand the console
  to `lock-command` and give it back afterwards. The default
  `lock-command` is `rundll32.exe user32.dll,LockWorkStation`, which locks
  the workstation (there is no `lock(1)`).
- **detach-client -P** ends the parent of the client only when that parent
  is attached to the client's own console (normally the shell). A parent
  that hosts the console instead, such as Windows Terminal or a tmux server
  running the client directly in a pane, is left alone and the client just
  detaches (`TerminateProcess` cannot be caught like `SIGHUP`).
- **Detach** restores the console: modes the client turned on (mouse,
  alternate screen) are turned off and the code pages are put back.
- **Nested tmux**: attaching or creating a session from inside one of the
  server's own panes (with `$TMUX` set) is refused with "sessions should be
  nested with care, unset $TMUX to force". With no tty name to compare, the
  server looks for one of its pane processes among the client's ancestors.
- **Control mode (`-C`)** works from pipes and from a console: the client's
  standard input and output are carried over its terminal channel. Commands
  may end in CRLF (as PowerShell writes to a pipe), and the client exits at
  end of input.

### Pane information

`#{pane_current_command}`, `#{pane_current_path}` and `automatic-rename`
work for cmd.exe, PowerShell and Git Bash panes; see Pane Command And Path.

### Terminal features

- **terminal-features** patterns are matched with `fnmatch`, so the
  default `xterm*` entry and patterns such as `*:RGB` apply. Windows
  Terminal does not answer XTVERSION, so it is recognised from `WT_SESSION`
  (bracketed paste, cursor colour and style, clipboard, focus, hyperlinks,
  mouse, overline, progress bar, strikethrough, synchronized output, title,
  underline styles) and other terminals from `TERM_PROGRAM`.
- The built-in capability table includes `Ms` (OSC 52), so
  `set-clipboard on` sends copies to the terminal with OSC 52; `Ss`/`Se`
  (cursor style) and `Cs`/`Cr` (cursor colour); `kmous`, so `mouse on`
  enables mouse reporting; and `Hls` (OSC 8 hyperlinks with `id=`).
- **Regular expressions** (`#{s/...}`, `#{m/r:...}`, copy mode search) use
  a vendored copy of musl's POSIX regex engine (TRE) working on UTF-8 code
  points: alternation, groups, intervals, anchors, character classes and
  case-insensitive matching. Compiled patterns are limited in size
  (`REG_ESPACE`), so a pathological nested repetition cannot stall the
  server. `regsub` handles anchored and empty matches (`s/^/x/`).
- **fnmatch** supports escapes, character classes and UTF-8.
- **strftime** is implemented by tmux with the POSIX, GNU and BSD
  conversions and flags (`%k`, `%l`, `%P`, `%-d`, ...); unknown conversions
  are copied literally instead of aborting the server through the C
  runtime's invalid parameter handler.
- The buffer editor (`C-b =` then `e`) writes a binary temporary file in
  the user's temporary directory; CRLF that a Windows editor adds is turned
  back into LF unless the buffer already had CR.

## Architecture

### Key Design Decisions

1. **IPC: Named pipe discovery + TCP data path.**
   Windows lacks `AF_UNIX` support in older builds. The server creates a named
   pipe `\\.\pipe\tmux-<user>-<label>` restricted to the current user (see
   Security). Clients connect to the pipe, receive the TCP port with a
   one-time nonce and proof, then authenticate over loopback TCP. The data
   path stays on TCP for libevent `select()` compatibility.

2. **PTY: ConPTY (Windows Pseudo Console) instead of /dev/pty.**
   Child processes are spawned with `CreatePseudoConsole()`. Two bridge threads
   shuttle I/O between the ConPTY pipe handles and a Winsock socket pair so that
   libevent can drive all I/O through `select()`. Pseudo consoles are closed
   off the main thread and freed only after their threads exit.

3. **Signals: Console events + polling + self-pipe pattern.**
   A console control handler catches Ctrl+C (SIGINT) and window close
   (SIGTERM), waiting so the event loop can handle it before Windows ends the
   process. A polling thread detects console resize (SIGWINCH); it is not
   started without a console (a detached server has none). All signals are
   delivered to the libevent loop via a TCP socketpair self-pipe.

4. **Terminal capabilities: Built-in xterm-256color table.**
   No ncurses dependency on Windows. `win32-terminfo.c` holds the ncurses
   xterm-256color entry plus the extensions Windows Terminal and the Windows
   10+ console host support (about 230 capabilities). DECSLRM margins and
   DECFRA stay off by default (the `margins` and `rectfill` features turn them
   on); `terminal-features` and `terminal-overrides` still apply on top.

5. **Process management: Thread pool waits instead of SIGCHLD.**
   Every child is registered with its process handle and job object. A
   thread pool wait per child reports its exit through the signal pipe
   (SIGCHLD) and `waitpid()` returns it; the handle is kept until then so the
   PID cannot be reused, like a Unix zombie. There is no fixed limit on the
   number of children. `kill()` terminates the child's job object, and IDs
   that are not live children are never signalled.

6. **File I/O: Reader threads and synchronous writes.**
   libevent's `select()` on Windows only works with Winsock sockets. The
   client reads files, pipes and consoles on a thread that feeds a socket
   pair, and writes stdout, stderr and files synchronously.

7. **TTY channel separation for attach.**
   On Unix, the client's terminal fd is passed to the server via `SCM_RIGHTS`.
   On Windows, `c->fd` is a TCP socket — it cannot carry terminal semantics.
   Instead, the client opens a second authenticated connection for the TTY
   channel (`MSG_IDENTIFY_TTYTOKEN`) before identifying. The server correlates
   this connection to the client and uses it exclusively for escape sequence
   I/O (or, in control mode, for standard input and output). A client input
   thread waits for console input or a stop event, so it can hand the console
   to `lock-command` without a blocked read swallowing input.

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
| `-f FILE`, `-v` | Inherited by the forked server | Passed to an auto-started server (`-f` as an absolute path) |
| `-L name` | Socket in `/tmp/tmux-$UID/` | Named pipe `\\.\pipe\tmux-<user>-<name>`; names are limited to `[A-Za-z0-9._-]` |
| `-S /path/to/sock` | Unix domain socket file | Basename used as the label (warning emitted) and checked like `-L` |
| `default-shell` | Must start with `/` | A Windows path to an existing program; Unix paths (starting with `/`) are rejected |
| `$SHELL` | Required to start with `/` | Used as is when set (Git Bash passes a Windows path); otherwise `$COMSPEC`, then `cmd.exe` |
| `$TMUX_TMPDIR` | Controls socket directory | Not used (no socket files) |
| `~/.tmux.conf` | `~` = `$HOME` or pw_dir | `~` = `$USERPROFILE` or `$HOME` or `C:\` |
| `-u` (UTF-8) | Opt-in | Auto-enabled on Windows |
| Output line endings | LF | LF (stdout and stderr are binary) |
| `$TERM` unset | No terminal type | `xterm-256color` |

## New Files (win32/)

| File | Purpose |
|------|---------|
| `win32-platform.h` | POSIX type mappings, constants, signal stubs, struct termios/winsize, function declarations |
| `win32-compat.c` | POSIX shims: socketpair, getpeereid, waitpid/kill glue, flock, clock_gettime, strftime, localtime_r/gmtime_r, fnmatch, tparm, mkstemp, ttyname, invalid parameter handler |
| `win32-ipc.c` | Named pipe discovery with server identity checks, nonce and proof authentication, TCP data path, tty channel, server launch |
| `win32-process.c` | Child registry (thread pool waits, job objects), `kill()`/`waitpid()`, jobs with pipes bridged to sockets |
| `win32-spawn.c` | Command lines for cmd.exe, MSYS2/Cygwin and native programs, program lookup through `PATH`/`PATHEXT`, environment blocks, working directories, exit status mapping |
| `win32-pty.c` | ConPTY wrapper: pseudo-console creation, process spawn, bridge threads, resize, close |
| `win32-signal.c` | Signal emulation via console events, resize polling, self-pipe delivery |
| `win32-tty.c` | Console mode management: raw VT mode, size queries, UTF-8 code pages and their restoration |
| `win32-terminfo.c` | Built-in terminal capability table (xterm-256color plus extensions, about 230 entries) |
| `win32-unicode.c`, `win32-unicode.h` | UTF-8 decoding and encoding, character classes, case mapping and widths for 32-bit code points; UTF-8 `argv` and environment |
| `win32-wcwidth.h` | Width tables generated by `gen-wcwidth.py` from the Unicode Character Database |
| `win32-regex.c`, `win32-regex.h` | POSIX regex interface and `regerror()` |
| `tre-regcomp.c`, `tre-regexec.c`, `tre-mem.c`, `tre.h`, `musl-*.h` | POSIX regex engine (TRE) vendored from musl libc, adapted to UTF-8 code points |
| `tmux.manifest` | Application manifest: UTF-8 process code page, long path awareness |
| `cmd-parse.cmake` | Generates `cmd-parse.c` from `cmd-parse.y` with bison |

`osdep-windows.c` (pane foreground process and working directory) is the
Windows counterpart of `osdep-linux.c` and friends.

## Pane Command And Path

There is no terminal foreground process group on Windows, so
`#{pane_current_command}`, `automatic-rename` and `#{pane_current_path}` look
at the processes attached to the pane's pseudoconsole: the newest process
started by a shell (cmd, PowerShell, bash, ...) is taken as the foreground
one, so `sleep` run from Git Bash or `ping` run from cmd.exe is reported while
it runs, and a program such as vim or an agent keeps the name while it runs
tools of its own. MSYS2 programs fork and exec through processes that exit;
their orphans are still found. One process snapshot is shared by all panes
for 250 ms, and `automatic-rename` rechecks all windows together once a
second. Window names from Windows command lines drop the directory and the
executable extension (quoted paths and paths with spaces included).

A command started in the background (`cmd &` in bash, `start /b` in cmd.exe)
is recognised by the shell doing work again, such as reading a key at its
prompt, after starting it; from then on the pane shows the shell and its
directory. Limits of this approximation:

- Until the shell reads input again, a background command is reported as
  the foreground one (the shell has not changed directory since, so
  `#{pane_current_path}` is still right).
- A job brought back with `fg` stays treated as a background one.
- PowerShell does its work on threads that cannot be told apart from idle
  ones, so commands started with `Start-Process -NoNewWindow` are reported
  as the foreground one while they run (PowerShell's `command &` jobs run
  in a separate process outside the pane and are not affected).
- A shell that reads a command's output itself, such as bash running
  `$(command)`, may show as the shell while that command writes output.

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
| `tmux.c` | UTF-8 `argv`/environment and binary stdout/stderr at startup; shell detection (`$SHELL`, `$COMSPEC`); label naming and validation; `/dev/null` → `NUL` for `-f`; `-S` basename; `checkshell()` Unix path guard |
| `client.c` | Server autostart with `-f`/`-v` and failure reporting; tty channel for consoles and control mode; console input thread; client-driven resize; lock, suspend and `detach -P`; console restore |
| `server.c` | `win32_ipc_create_server()`; asynchronous authentication with a deadline and a bound on pending connections; bounded pending TTY queue; `exit-empty` for auto-started servers |
| `server-client.c` | tty channel matching; `MSG_RESIZE`; nested client check by pane ancestry; "not a terminal" for clients without a console |
| `spawn.c` | Panes through `win32_spawn` and ConPTY; one argument is a shell command, more are run directly |
| `job.c` | Jobs through `win32_process_spawn()` (pipes bridged to sockets) and ConPTY for PTY jobs (popups) |
| `file.c` | Client file I/O through reader threads and synchronous writes; UTF-8 paths; binary mode |
| `proc.c` | Signal pipe integration; `closesocket` |
| `names.c`, `osdep-windows.c` | Foreground process and working directory from the pseudoconsole; shared `automatic-rename` timer; Windows command lines in `parse_window_name` |
| `compat/imsg-buffer.c` | Gathered `WSASend` that drains only what was sent; `WSAGetLastError()` for Winsock errors |
| `tty.c`, `tty-term.c`, `tty-features.c` | Built-in capabilities; `fnmatch` for `terminal-features`; features from `WT_SESSION`/`TERM_PROGRAM`; the client owns its console modes |
| `utf8.c`, `compat.h`, `tmux.h`, `input-keys.c`, `key-string.c` | 32-bit `utf8_wchar` and tmux's own UTF-8 decoder and encoder |
| `cmd-parse.y` | CRLF line endings and a UTF-8 byte order mark |
| `control.c` | Control mode over the tty channel; CR before LF dropped |
| `cmd-run-shell.c`, `cmd-pipe-pane.c` | CRLF output lines; pipe-pane standard handles for `-I`/`-O` |
| `window.c`, `server-fn.c` | Wait for pane output end of file before destroying a pane; ConPTY cleanup |
| `environ.c` | Case-insensitive variable names |
| `regsub.c` | Anchored and empty matches |

### Minor Changes (conditional includes and guards)

The remaining files have small changes, primarily:
- `#ifndef _WIN32` guards around Unix-specific includes (`<signal.h>`, `<unistd.h>`, `<sys/socket.h>`, etc.)
- Minor compatibility adjustments (path separators, type casts)
- Safe fallbacks for missing POSIX functions

## Regression Tests

Windows-specific tests are in `regress/`: native C unit tests run by ctest
and the `regress/win32-*.sh` scripts, run from Git Bash in the source root.
`regress/win32-run-all.sh` runs every script (new ones are picked up
automatically), each under its own server label
(`$TMUX_TEST_PREFIX-<name>`, default prefix `run`) and a timeout
(`$TMUX_TEST_TIMEOUT` seconds, default 600), and prints a summary; a script
exiting 77 counts as skipped. CI runs it against both the Debug and the
Release build. Because every script uses its own label and only stops its
own servers, tests can run beside real sessions and beside each other:
never kill `tmux.exe` by name to prepare a test run.

```bash
TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh
TEST_TMUX=./build/win32/tmux.exe TMUX_TEST_PREFIX=mine sh regress/win32-run-all.sh basic keys
TMUX_TEST_LABEL=mytest TEST_TMUX=./build/win32/tmux.exe bash regress/win32-basic.sh
```

ctest registers the native unit tests and each script (label `regress`,
server label `ctest-<name>`):

```powershell
ctest --test-dir build\win32 -LE regress --output-on-failure   # unit tests
ctest --test-dir build\win32 -L regress --output-on-failure    # scripts
```

Every check asserts on command output (for example a computed value printed
by the pane, not the command text that was typed), and waits by polling with
a timeout rather than sleeping.

### Native unit tests (ctest)

- `win32-tparm` (label `unit`): numeric expansion and 64-bit string
  parameters used by terminal capabilities such as OSC 8 hyperlinks; this
  guards the Codex `/status` path that previously crashed the server when a
  pointer was truncated to 32 bits.
- `win32-text-unit`: terminfo expansion, strftime, fnmatch, regex, UTF-8
  and character widths.
- `win32-kill`: `kill()` and `waitpid()` of the child registry; a job ID
  that is no longer a live child never signals a bystander process.

CI runs `ctest -L unit`, which selects all three.

### Scripts

| Script | Covers |
|--------|--------|
| `win32-basic.sh` (8 tests) | Session lifecycle, `send-keys`/`capture-pane`, split and pane exit cleanup, detached session size, `-f /dev/null` → `NUL`, `-S` with a Unix-style path, the `default-shell` guard (also from a configuration file) |
| `win32-claude-swarm.sh` (11 tests) | Claude Code agent team command sequences over 3 cycles, multiple sessions, rapid create/destroy, and an agent driver sequence (`new-session -c`, `load-buffer`, `paste-buffer -p`, `capture-pane -J -S`, `delete-buffer`) |
| `win32-format-strings.sh` (125 assertions) | Port of upstream `format-strings.sh`, including `#{s/...}` and `#()` |
| `win32-conf-syntax.sh` (21 files) | Port of upstream `conf-syntax.sh`: each `regress/conf/*.conf` with LF, CRLF and a UTF-8 BOM |
| `win32-keys.sh` (15 tests) | Enter, literals, Space, Tab, Escape, Ctrl-C, copy mode keys, F-keys, Backspace, modifiers, mouse key names, punctuation, UTF-8 key metadata |
| `win32-has-session.sh` | Port of upstream `has-session-return.sh` exit codes |
| `win32-layout.sh` (7 tests) | Pane geometry after splits and layouts |
| `win32-control-client.sh` (7 tests) | A control client with piped stdin runs or fails promptly, then the upstream control-client pane operations |
| `win32-attach.sh` | Unique client names, suspend-client, lock-command, console restore on detach, `detach-client -P` (and sparing a parent off the console), the nested session guard, control mode from pipes (with CRLF) and from a console |
| `win32-ipc.sh` | `-f`/`-v` and non-ASCII arguments reaching an auto-started server, silent or bad connections, many concurrent clients, concurrent server start, piped clients and attach, dying servers, `-D` servers staying, label checks, refusing foreign pipes, Node with piped stdio, the TTY queue limit, a busy server |
| `win32-files.sh` | `load-buffer`/`save-buffer`/`show-buffer`/`source-file` with files, `-` and pipes, `source -n`, non-ASCII paths, binary output (no CR) |
| `win32-jobs.sh` | `run-shell`, `if-shell` with cmd.exe/Git Bash/PowerShell, `#()`, `pipe-pane`, `copy-pipe`, shell and argument vector quoting, `.cmd` shims, exit status mapping, `tmux -c`, UTF-8 job output |
| `win32-process.sh` | Last output and status of exiting panes, Ctrl-C, environment and working directory, handle inheritance, many sequential and concurrent children, idle CPU, process tree kills, `kill-session` time, servers started from a pane |
| `win32-popup.sh` | `display-popup -E` closing, no leaked console hosts, Ctrl-C and resize in popups |
| `win32-text.sh` | strftime, regex, fnmatch, widths, copy mode search, non-ASCII `send-keys`, and through a nested client: terminal features, mouse, cursor style and colour, OSC 52, the buffer editor |
| `win32-unicode.sh` (8 tests) | Korean option values, names, `send-keys -l` (also outside the BMP), `-e`, `-c`, a non-ANSI install directory, and (with `mt.exe`) a copy of `tmux.exe` without the UTF-8 code page |
| `win32-pane-info.sh` | `pane_current_command`, `automatic-rename` and `pane_current_path` for Git Bash, cmd.exe and PowerShell, background commands, programs with shells of their own, Windows command line names |
| `win32-install.sh` | `install-user.ps1` with a scratch install directory and registry key, under Windows PowerShell and pwsh |

Note: The tests use `OUT` instead of `TMP` for temp file variables to avoid
shadowing the Windows `%TMP%` environment variable (which would cause
`GetTempFileName()` to fail with ACCESS_DENIED in the server process).

## Installation

### Local build install

From a local build tree:

```powershell
.\install-user.ps1 -Build    # configure and build Release, then install
.\install-user.ps1           # install an existing build\release\tmux.exe
.\install-user.ps1 -DryRun   # show what would be done (same as -WhatIf)
```

This copies `build\release\tmux.exe` (`-BuildExe` to choose another) to
`%LOCALAPPDATA%\tmux-windows\bin` (`-InstallDir` to change; a relative path
is taken from the current location) and adds that directory to the user
`Path` in the registry, keeping existing `%VAR%` entries unexpanded and the
value type (`-NoPathUpdate` to skip). `-Build` finds Visual Studio with
`vswhere`, and vcpkg through `VCPKG_ROOT` or the Visual Studio vcpkg
component. Servers running from the `tmux.exe` being replaced are asked to
exit with `kill-server` whatever their `-L` label, and that `tmux.exe`'s
leftover processes are stopped, so new clients do not attach to a server
from the older binary; servers started from other copies of tmux are left
alone. With `-KeepSessions` running servers are kept and the old
`tmux.exe` is moved aside so the new one can be installed.

Run the installer from an ordinary terminal. Programs started by a packaged
(MSIX) app, such as the Claude desktop app or an agent running inside it,
can have their writes to `%LOCALAPPDATA%` and the user registry redirected
into `%LOCALAPPDATA%\Packages\<app>\LocalCache`, so the install and the PATH
change are invisible everywhere else. The script detects this and warns;
from such an app install into a directory outside `%LOCALAPPDATA%` that is
already on `PATH` instead:

```powershell
.\install-user.ps1 -InstallDir "$env:USERPROFILE\.local\bin" -NoPathUpdate
```

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

Servers started by a client exit when their last session is destroyed, so
the server is only resident while sessions are active.

**Upgrading from a build before the IPC hardening:** a server started by an
older `tmux.exe` is refused by the new client, typically with
`server ... grants access to other users; refusing to connect`, and the new
`tmux kill-server` cannot reach it either. End that old server once (Task
Manager, or `Stop-Process` on the old server's `tmux.exe` process; its
sessions are lost) and start a new one. `install-user.ps1` stops the servers
of the `tmux.exe` it replaces with that old binary, unless `-KeepSessions`
is given.

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

The resulting `tmux.exe` is a single statically-linked binary with the
manifest embedded (`-DTMUX_WIN32_MANIFEST=OFF` leaves it out). Copy it to a
directory in `%PATH%` or run `.\install-user.ps1`.

`tmux -V` reports the version in `configure.ac` with a `-win32` suffix, or
`-DTMUX_VERSION=...` when given (release tags set it).

`cmd-parse.c` is generated from `cmd-parse.y` by `win32/cmd-parse.cmake`.
With `win_bison` ([WinFlexBison](https://github.com/lexxmark/winflexbison))
or `bison` on `PATH` it is generated into the build directory; otherwise the
committed `cmd-parse.c` is used. Its first line records the SHA256 of the
grammar it was generated from (line endings normalized), and configuring
fails if that does not match the current `cmd-parse.y`
(`-DTMUX_ALLOW_STALE_CMD_PARSE=ON` overrides this). After changing
`cmd-parse.y`, refresh the committed copy with
`cmake --build build\win32 --target update-cmd-parse` (the target exists
when bison was found).

CI builds run automatically on every push to `master` and on pull requests
via GitHub Actions (`.github/workflows/ci.yml`): Debug and Release, the
`unit` ctest label and `regress/win32-run-all.sh` against each. Tagged
releases (`v*`) package the tested Release `tmux.exe` as a zip with its
SHA256.

## Debugging

Use the `-v` flag for verbose logging. Each `-v` increases the log level:

```bash
tmux -vvv new-session
```

This creates timestamped log files in the current directory (a server
started by the client gets the same `-v` level and writes its log there
too):
- `tmux-client-<pid>.log` — client-side timing (connect, identify, attach)
- `tmux-server-<pid>.log` — server-side timing (pipe, auth, ConPTY, events)
- `tmux-out-<pid>.log` — terminal output (with `-vv` or more)

Each line has microsecond-precision timestamps (`epoch.usec`) for profiling:

```
1771885428.291080 client started (43248): version next-3.7-win32, ...
1771885428.295389 connect failed, starting server
1771885428.512943 add peer ...: connected
```

The standard error of a server started by a client goes to
`%LOCALAPPDATA%\tmux\<server name>.server.log`; the client shows it if the
server dies while starting.

## Runtime Requirements

- Windows 10 version 1903+ (1809, the minimum for ConPTY, works but file
  and process APIs use the ANSI code page there; see Korean and other UTF-8
  text)
- Windows Terminal or a VT-capable console host
- No additional DLLs required (libevent is statically linked)

## Security

The Windows port uses named pipes for server discovery and loopback TCP for
the data path. The following hardening measures are in place:

- **Owner-only pipe**: The server creates the first instance of
  `\\.\pipe\tmux-<user>-<label>` itself (`FILE_FLAG_FIRST_PIPE_INSTANCE`),
  owned by the current user with a protected DACL admitting only that user,
  and rejects remote clients. If another process already owns the name the
  server refuses to start.
- **Integrity level**: A server running elevated labels its pipe (and its
  start mutex) with its own integrity level, so the same user's ordinary
  processes can neither read nor write it; they get "access denied" with a
  hint that the server may run as administrator.
- **Server identity check**: Before trusting a pipe, the client opens it at
  identification level and checks its owner, that its DACL grants nothing
  to anyone else, its integrity label and the user and integrity level of
  the server process.
- **Mutual authentication**: The pipe answers with the port, a one-time
  nonce and a proof (32 random bytes each, from `RtlGenRandom`). The client
  sends the nonce over TCP and waits for the proof, so each side knows the
  other learnt the secrets from the protected pipe. Nonces are single-use,
  expire after 60 seconds and are compared in constant time; when too many
  are outstanding the pipe answers "busy".
- **Exclusive port**: The TCP listener on 127.0.0.1 uses
  `SO_EXCLUSIVEADDRUSE`, and every socket and handle is created
  non-inheritable, so child processes never hold client connections open.
- **Asynchronous authentication**: The server reads authentication without
  blocking, with a 5 second deadline and at most 64 pending connections, so
  a silent or malicious connection cannot freeze it. A client waits up to 30
  seconds for a busy server and reports a timeout as "not responding".
- **Bounded TTY queue**: Pending TTY channel connections are capped at 16
  entries with a 30-second expiry, preventing resource exhaustion from
  unclaimed connections.
- **Serialized server start**: Concurrent first clients take a per-label
  mutex, so only one server is started.
- **Internal socket pairs**: `socketpair()` checks that the accepted
  connection is its own connecting socket, so another local process cannot
  hijack signal, job or ConPTY bridge sockets.
- **Label validation**: `-L` and `-S` labels are restricted to
  `[A-Za-z0-9._-]` (at most 100 characters), so `$TMUX` and server command
  lines cannot inject arguments.
- **No persistent secrets**: There are no auth files, port files, or tokens
  stored on disk. All authentication state is in-memory and ephemeral.

No IPC files are written to disk. The named pipe is automatically cleaned up
when the server exits or the pipe handle is closed.

## Known Limitations

- No Unix domain socket support; IPC uses named pipe discovery + TCP data path
- No file descriptor passing between processes; tty channel model used instead
- `getpeereid()` reports uid 0 for every client, so `server-access` has no
  effect; access is limited to the current user by the pipe's DACL instead
- `SIGCHLD` is emulated via thread pool waits; timing may differ from Unix
- There is no job control: `suspend-client` only redraws the client
- `-CC` (iTerm2 control mode) is untested; termios is stubbed, so its
  terminal settings are not applied
- `pane_current_command` and `pane_current_path` are approximations (see
  Pane Command And Path); PowerShell needs the profile snippet above for
  `pane_current_path` to follow `Set-Location`
- On Windows 10 1809 paths must be representable in the ANSI code page
- Job output is made UTF-8 only for jobs run through cmd.exe; with another
  `default-shell`, console programs run by a job write in the console's
  code page
- Some terminal features may behave differently under ConPTY vs. native Unix PTYs
- A server started by a build before the IPC hardening must be ended once
  after upgrading (see Upgrading)
