# Claude Code Project Notes — tmux-windows

## MSYS2 / Git Bash Shell Quirks

### taskkill syntax
MSYS2 converts single-slash flags (`/F`) into file paths. Always use **double slashes**:
```bash
taskkill //F //IM tmux.exe
```
Never use `taskkill /F /IM` — MSYS2 mangles `/F` into `F:/`.

### Before rebuilding tmux.exe
Always kill all tmux processes first, or the linker fails with `LNK1104: cannot open file 'tmux.exe'`:
```bash
taskkill //F //IM tmux.exe 2>/dev/null; sleep 2
```

### MSYS2 path conversion
MSYS2 converts Unix-style paths passed to native Windows executables. This affects:
- Relative paths like `regress/conf/foo.conf` may become `C\:/src/tmux/regress/conf/foo.conf`
- The escaped colon (`C\:`) has a literal backslash before the colon
- Code checking `path[1] == ':'` for Windows drive letters must also check `path[1] == '\\' && path[2] == ':'`

### Test temp variables
Never use `TMP` as a shell variable name in test scripts — it shadows Windows `%TMP%` and causes `GetTempFileName()` ACCESS_DENIED errors in child processes. Use `OUT` instead.

## Build Commands

Ninja builds one configuration per directory (it ignores `--config`):
Debug in `build\win32`, Release in `build\release`. The first configure
needs `-G Ninja -DCMAKE_BUILD_TYPE=Debug|Release` and the vcpkg toolchain
(see README_WIN32.MD); afterwards `cmake --build <dir>` is enough.

```bash
# Debug build (from Git Bash)
taskkill //F //IM tmux.exe 2>/dev/null; sleep 2
powershell.exe -NoProfile -Command "& { cmd.exe /c '\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cmake --build build\win32' }"

# Release build
powershell.exe -NoProfile -Command "& { cmd.exe /c '\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cmake --build build\release' }"

# Run all regression scripts, or one
TEST_TMUX=./build/win32/tmux.exe sh regress/win32-run-all.sh
TEST_TMUX=./build/win32/tmux.exe bash regress/win32-basic.sh
```

## Test Suite

CI runs `regress/win32-run-all.sh` against the Debug and the Release build;
it runs every `regress/win32-*.sh` script (new ones automatically), each
with its own server label (`TMUX_TEST_LABEL`) and a timeout:
- `win32-basic.sh` — core session/window/pane operations
- `win32-claude-swarm.sh` — multi-pane agent team usage and an agent driver sequence
- `win32-format-strings.sh` — `#{...}` format engine (130 assertions)
- `win32-conf-syntax.sh` — config file parsing (21 files, LF and CRLF)
- `win32-keys.sh` — key handling (15 tests, tests effects not raw codes)
- `win32-has-session.sh` — exit code validation
- `win32-layout.sh` — pane dimension verification (7 tests)
- `win32-control-client.sh` — control mode and complex pane operations (7 tests)
- `win32-unicode.sh` — Korean/UTF-8 arguments, names, paths and input
- `win32-pane-info.sh` — pane_current_command/path and automatic-rename
- `win32-install.sh` — install-user.ps1 against a scratch directory and registry key

ctest runs the native `win32-tparm` test (label `unit`) and registers the
scripts too (label `regress`).

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

## Known Quirks

- `source -n` hangs as a separate client on Windows (file-read protocol blocks)
