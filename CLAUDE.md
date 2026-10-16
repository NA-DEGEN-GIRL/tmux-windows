# Claude Code Project Notes — tmux-windows

## MSYS2 / Git Bash Shell Quirks

### taskkill syntax
MSYS2 converts single-slash flags (`/F`) into file paths. Always use **double slashes**:
```bash
taskkill //F //PID 1234
```
Never use `taskkill /F /PID` — MSYS2 mangles `/F` into `F:/`.

### Do not kill tmux by name to run tests
Every regression script runs its own server under its own label
(`TMUX_TEST_LABEL`; `regress/win32-run-all.sh` uses
`$TMUX_TEST_PREFIX-<name>`) and stops only that server, so tests can run
beside real sessions and beside another test run. Never run
`taskkill //IM tmux.exe` before or between tests. To clear a leftover test
server, use its label:
```bash
./build/win32/tmux.exe -Lrun-basic kill-server
```

### Rebuilding a tmux.exe that is still running
The linker fails with `LNK1104: cannot open file 'tmux.exe'` while any
process runs that `tmux.exe` (a leftover test server, or a test run in
progress). Stop the servers of that build by label first. Only as a last
resort kill by name — this ends **every** tmux on the machine, including
real sessions and any test run in progress:
```bash
taskkill //F //IM tmux.exe 2>/dev/null; sleep 2
```

### MSYS2 path conversion
MSYS2 converts Unix-style paths passed to native Windows executables. This affects:
- Relative paths like `regress/conf/foo.conf` may become `C\:/src/tmux/regress/conf/foo.conf`
- The escaped colon (`C\:`) has a literal backslash before the colon
- Code checking `path[1] == ':'` for Windows drive letters must also check `path[1] == '\' && path[2] == ':'`
- Set `MSYS_NO_PATHCONV=1` for arguments that must reach tmux unchanged (such as `/bin/bash` or `#{s/#:/...}`)

### Test temp variables
Never use `TMP` as a shell variable name in test scripts — it shadows Windows `%TMP%` and causes `GetTempFileName()` ACCESS_DENIED errors in child processes. Use `OUT` instead.

## Build Commands

Ninja builds one configuration per directory (it ignores `--config`):
Debug in `build\win32`, Release in `build\release`. The first configure
needs `-G Ninja -DCMAKE_BUILD_TYPE=Debug|Release` and the vcpkg toolchain
(see README_WIN32.MD); afterwards `cmake --build <dir>` is enough.
`.\install-user.ps1 -Build` configures and builds `build\release` itself.

```bash
# Debug build (from Git Bash)
powershell.exe -NoProfile -Command "& { cmd.exe /c '\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cmake --build build\win32' }"

# Release build
powershell.exe -NoProfile -Command "& { cmd.exe /c '\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cmake --build build\release' }"

# Run all regression scripts, some of them, or one
TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh
TEST_TMUX=./build/win32/tmux.exe TMUX_TEST_PREFIX=mine sh regress/win32-run-all.sh basic keys
TMUX_TEST_LABEL=mine-basic TEST_TMUX=./build/win32/tmux.exe bash regress/win32-basic.sh

# Native unit tests, and the scripts through ctest
ctest --test-dir build/win32 -LE regress --output-on-failure
ctest --test-dir build/win32 -L regress --output-on-failure
```

`cmd-parse.c` is generated from `cmd-parse.y` (`win32/cmd-parse.cmake`).
Its first line holds the grammar's SHA256 and configuring fails if the
committed copy is stale; after editing `cmd-parse.y` run
`cmake --build build/win32 --target update-cmd-parse` (needs `win_bison` or
`bison` on `PATH`) and commit the regenerated `cmd-parse.c`.

## Test Suite

CI runs `ctest -L unit` and `regress/win32-run-all.sh` against the Debug
and the Release build. `win32-run-all.sh` runs every `regress/win32-*.sh`
script (new ones automatically), each with its own server label and a
timeout (`TMUX_TEST_TIMEOUT`, default 600 s); exit 77 means skipped:
- `win32-basic.sh` — core session/window/pane operations (8 tests)
- `win32-claude-swarm.sh` — multi-pane agent team usage and an agent driver sequence (11 tests)
- `win32-format-strings.sh` — `#{...}` format engine (125 assertions)
- `win32-conf-syntax.sh` — config file parsing (21 files, LF, CRLF and BOM)
- `win32-keys.sh` — key handling (15 tests, tests effects not raw codes)
- `win32-has-session.sh` — exit code validation
- `win32-layout.sh` — pane dimension verification (7 tests)
- `win32-control-client.sh` — control mode and complex pane operations (7 tests)
- `win32-attach.sh` — attached clients: names, suspend, lock, detach -P, nesting, control mode
- `win32-ipc.sh` — client/server IPC: autostart args, auth, concurrency, label checks, foreign pipes
- `win32-files.sh` — client file I/O (`load-buffer`, `save-buffer`, `source-file`, `source -n`, `-`)
- `win32-jobs.sh` — run-shell/if-shell/#()/pipe-pane/copy-pipe, shell quoting, exit status, UTF-8 output
- `win32-process.sh` — pane processes: last output, Ctrl-C, environment, cwd, process tree kills
- `win32-popup.sh` — display-popup close, Ctrl-C and resize
- `win32-text.sh` — strftime, regex, fnmatch, widths, terminal features, OSC 52, buffer editor
- `win32-unicode.sh` — Korean/UTF-8 arguments, names, paths and input (8 tests)
- `win32-pane-info.sh` — pane_current_command/path and automatic-rename
- `win32-install.sh` — install-user.ps1 against a scratch directory and registry key

ctest also has three native unit tests: `win32-tparm` (label `unit`, the
only one CI runs), `win32-text-unit` and `win32-kill` (no label), and
registers each script (label `regress`, server label `ctest-<name>`).

## Winget Package Submission

Package: `arndawg.tmux-windows` in `microsoft/winget-pkgs`.

### How to submit a new version

1. **Sync the fork** before anything else:
   ```bash
   gh api repos/arndawg/winget-pkgs/merge-upstream -X POST -f branch=master
   ```

2. **Create a branch** from the synced master:
   ```bash
   MASTER_SHA=$(gh api repos/arndawg/winget-pkgs/git/refs/heads/master --jq '.object.sha')
   gh api repos/arndawg/winget-pkgs/git/refs -X POST \
     -f ref=refs/heads/arndawg.tmux-windows-VERSION \
     -f sha="$MASTER_SHA"
   ```

3. **Create files via the contents API** (one PUT per file). Do NOT use the git trees API — on a repo this large it produces phantom diffs with hundreds of unrelated files. The contents API creates clean per-file commits:
   ```bash
   CONTENT=$(printf '%s' "$YAML" | base64 -w0)
   gh api repos/arndawg/winget-pkgs/contents/manifests/a/arndawg/tmux-windows/VERSION/FILENAME \
     -X PUT -f message="Add manifest" -f content="$CONTENT" \
     -f branch=arndawg.tmux-windows-VERSION
   ```
   Three files needed: `arndawg.tmux-windows.installer.yaml`, `arndawg.tmux-windows.locale.en-US.yaml`, `arndawg.tmux-windows.yaml`.

4. **Verify** the branch diff before creating the PR:
   ```bash
   gh api repos/arndawg/winget-pkgs/compare/master...arndawg.tmux-windows-VERSION \
     --jq '{ahead_by: .ahead_by, files: [.files[].filename]}'
   ```
   Must show exactly 3 files and 3 commits ahead.

5. **Create the PR** with `gh pr create --repo microsoft/winget-pkgs`.

6. **Validation takes up to 3 hours.** Monitor with:
   ```bash
   gh pr view PRNUM --repo microsoft/winget-pkgs --json labels,comments \
     --jq '{labels: [.labels[].name], latest: .comments[-1].body[:200]}'
   ```
   Look for the `Validation-Completed` label. The Azure DevOps pipeline comment has a badge link.

### Manifest template reference

Copy from the previous version directory in `manifests/a/arndawg/tmux-windows/` — update `PackageVersion`, `InstallerUrl`, `InstallerSha256`, `ReleaseDate`, `ReleaseNotes`, and `ReleaseNotesUrl`.

### Manifest formatting

Do **not** add a trailing blank line to manifest YAML files. Earlier versions (3.6a-win32.1/3/4) had one, but from v3.6a-win32.6 onward we omit it.
