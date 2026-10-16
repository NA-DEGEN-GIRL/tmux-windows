# tmux-windows

Native Windows port of `tmux.exe`, based on the upstream tmux C codebase.

이 프로젝트는 WSL, Cygwin, MSYS2 안에서 돌아가는 tmux가 아니라 Windows Terminal과 ConPTY 위에서 직접 실행되는 네이티브 Windows `tmux.exe` 포트를 목표로 합니다.

## 한국어

### 목표

- upstream `tmux/tmux` C 코드 기반의 Windows 네이티브 포트
- Windows Terminal 또는 VT-compatible console에서 `tmux` 명령 그대로 사용
- `tmux new`, `attach`, `detach`, `send-keys`, `capture-pane`, `split-window` 같은 핵심 워크플로우 우선 지원
- WSL/Cygwin/MSYS2 런타임 없이 Windows 프로세스와 ConPTY로 pane 실행

현재 목표는 완전한 tmux parity가 아니라, 실제 작업에 쓸 수 있는 Windows native MVP를 안정화하는 것입니다.

### 현재 동작 확인된 기능

```powershell
tmux -V
tmux new -s main
tmux new-session -d -s work
tmux ls
tmux attach -t work
tmux send-keys -t work "echo hello" Enter
tmux capture-pane -t work -p
tmux split-window -t work
tmux kill-session -t work
```

지원되는 핵심 기능:

- detached session 생성 및 재접속
- Windows Terminal attach/detach
- `cmd.exe`, PowerShell, `pwsh.exe`, `claude` 같은 Windows CLI 실행
- ConPTY 기반 pane I/O
- vertical/horizontal split
- `send-keys`, `capture-pane`, `list-panes`
- 기본 mouse/key binding 파싱
- `%USERPROFILE%\.tmux.conf` 설정 파일 (LF/CRLF, UTF-8 BOM 모두 가능)
- `run-shell`, `if-shell`, `#()`, `pipe-pane`, `copy-pipe`, popup 실행 (서버가 죽지 않음)
- `default-shell`로 cmd.exe, Git Bash, `pwsh.exe` 사용 (Windows 전체 경로로 지정)
- 한글 등 UTF-8: session/window 이름, option 값, `send-keys`, 경로, job 출력
- `#{pane_current_command}`, `#{pane_current_path}`, `automatic-rename`
- 셸에서 `source-file`, `load-buffer`/`save-buffer` (파일과 `-`) 사용, 출력은 LF (CRLF 아님)
- control mode (`tmux -C`), `kill-pane` 시 하위 프로세스 트리 전체 종료

### 요구 사항

- Windows 10 1903 이상 권장 (1809에서도 동작하지만 경로에 한글 등 ANSI 코드 페이지 밖의 문자가 있으면 안 됩니다)
- Windows Terminal 권장
- 로컬 빌드 시 Visual Studio Build Tools 또는 MSVC/clang-cl
- CMake
- vcpkg `libevent:x64-windows-static`

### 빌드

가장 간단한 방법은 설치 스크립트가 Release 빌드까지 하게 하는 것입니다 (아래 설치 참고). 직접 빌드하려면 "x64 Native Tools" 프롬프트에서 (환경에 따라 vcpkg toolchain 경로는 조정):

```powershell
# Release: build\release\tmux.exe
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\release

# Debug: build\win32\tmux.exe
cmake -S . -B build\win32 -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\win32
ctest --test-dir build\win32 --output-on-failure
```

Ninja는 `--config`를 무시하므로 빌드 종류는 `-DCMAKE_BUILD_TYPE`으로 정합니다.

### 설치

로컬 빌드 결과를 사용자 PATH에 설치하려면:

```powershell
.\install-user.ps1 -Build    # Release 빌드 후 설치
.\install-user.ps1           # 이미 있는 build\release\tmux.exe 설치
.\install-user.ps1 -DryRun   # 실제로 바꾸지 않고 할 일만 표시 (-WhatIf와 같음)
```

이 스크립트는 다음을 수행합니다.

- `build\release\tmux.exe`(`-BuildExe`로 변경 가능)를 `%LOCALAPPDATA%\tmux-windows\bin\tmux.exe`로 복사 (`-InstallDir`로 변경 가능)
- 해당 디렉터리를 User PATH에 추가 (레지스트리 값의 `%VAR%` 항목과 형식은 그대로 유지, `-NoPathUpdate`로 생략 가능)
- 교체되는 `tmux.exe`로 실행 중인 서버를 모든 `-L` label에 대해 먼저 종료 (다른 tmux.exe로 실행한 서버는 건드리지 않음)

현재 세션을 일부러 유지하고 싶다면:

```powershell
.\install-user.ps1 -KeepSessions
```

이 경우 실행 중인 예전 `tmux.exe`는 옆으로 옮겨 두고 새 파일을 설치합니다.

설치 스크립트는 일반 터미널에서 실행하세요. Claude 데스크톱 앱 같은 패키지(MSIX) 앱이나 그 안의 에이전트가 실행하면 `%LOCALAPPDATA%`와 사용자 레지스트리 쓰기가 `%LOCALAPPDATA%\Packages\<앱>\LocalCache`로 리디렉트되어, 설치와 PATH 변경이 다른 터미널에서는 보이지 않습니다. 스크립트가 이를 감지해 경고하며, 그런 환경에서는 이미 PATH에 있는 `%LOCALAPPDATA%` 밖의 디렉터리에 설치하세요.

```powershell
.\install-user.ps1 -InstallDir "$env:USERPROFILE\.local\bin" -NoPathUpdate
```

IPC 보안 강화 이전 버전에서 업그레이드할 때: 예전 `tmux.exe`가 띄운 서버는 새 클라이언트가 접속을 거부하며 (`server ... grants access to other users; refusing to connect`), 새 `tmux kill-server`로도 끌 수 없습니다. 그 예전 서버를 작업 관리자나 `Stop-Process`로 한 번 종료해야 합니다 (그 세션은 사라집니다). `install-user.ps1`은 `-KeepSessions` 없이 실행하면 교체되는 `tmux.exe`의 서버를 예전 바이너리로 종료합니다.

### 기본 사용법

```powershell
tmux new -s main
tmux ls
tmux attach -t main
tmux kill-session -t main
tmux kill-server
```

tmux 안에서 자주 쓰는 키:

```text
Ctrl-b d        detach
Ctrl-b c        새 window
Ctrl-b n        다음 window
Ctrl-b p        이전 window
Ctrl-b %        좌우 split
Ctrl-b "        상하 split
Ctrl-b 방향키   pane 이동
Ctrl-b x        pane 종료
Ctrl-b [        copy mode
q               copy mode 종료
```

### 추천 설정

설정 파일 위치:

```powershell
$env:USERPROFILE\.tmux.conf
```

예시:

```tmux
set -g mouse on
setw -g mode-keys vi
set -g history-limit 10000
unbind -T copy-mode-vi MouseDragEnd1Pane
set -g set-clipboard on
```

이미 서버가 떠 있으면:

```powershell
tmux source-file $env:USERPROFILE\.tmux.conf
```

### 현재 한계

- 아직 실험적인 포트입니다.
- 모든 upstream tmux 기능과 plugin 호환성을 보장하지 않습니다.
- clipboard는 OSC 52로 터미널에 보내므로 터미널의 OSC 52 지원 여부에 따라 달라집니다.
- job control이 없어 `suspend-client`는 화면만 다시 그립니다. `-CC`는 검증되지 않았습니다.
- `#{pane_current_command}`/`#{pane_current_path}`는 근사치입니다. PowerShell에서 `cd`를 따라가게 하려면 [README_WIN32.MD](README_WIN32.MD)의 프로필 설정(PowerShell 7.2+)이 필요합니다.
- package manager 배포, 고급 copy-mode/mouse 동작은 계속 검증이 필요합니다.

자세한 Windows 포트 구조와 테스트 목록은 [README_WIN32.MD](README_WIN32.MD)를 보세요.

## English

### Goal

This repository carries a native Windows port of tmux, built from the upstream tmux C codebase. It is not a tmux-like rewrite and it does not require WSL, Cygwin, or MSYS2 at runtime.

The port targets Windows Terminal or another VT-capable Windows console and uses ConPTY for panes.

### Verified Core Workflow

```powershell
tmux -V
tmux new -s main
tmux new-session -d -s work
tmux ls
tmux attach -t work
tmux send-keys -t work "echo hello" Enter
tmux capture-pane -t work -p
tmux split-window -t work
tmux kill-session -t work
```

Currently verified:

- detached sessions and reattach
- Windows Terminal attach/detach
- Windows shells and CLIs inside panes
- ConPTY-backed pane I/O
- horizontal and vertical splits
- `send-keys`, `capture-pane`, `list-panes`
- default mouse/key binding parsing
- `%USERPROFILE%\.tmux.conf` (LF or CRLF, with or without a UTF-8 BOM)
- jobs: `run-shell`, `if-shell`, `#()`, `pipe-pane`, `copy-pipe` and popups, without crashing the server
- `default-shell` set to cmd.exe, Git Bash or `pwsh.exe` (as a full Windows path)
- Korean and other UTF-8 text in names, option values, `send-keys`, paths and job output
- `#{pane_current_command}`, `#{pane_current_path}` and `automatic-rename`
- `source-file`, `load-buffer`/`save-buffer` (files and `-`) from a shell, with LF (not CRLF) output
- control mode (`tmux -C`), and `kill-pane` ending the pane's whole process tree

### Requirements

- Windows 10 version 1903 or newer recommended (1809 works, but paths must then be representable in the ANSI code page)
- Windows Terminal recommended
- Visual Studio Build Tools or MSVC/clang-cl for local builds
- CMake
- vcpkg `libevent:x64-windows-static`

### Build

The simplest way is to let the installer build Release (see below). To build
by hand, from an "x64 Native Tools" prompt (adjust the vcpkg toolchain path
for your environment):

```powershell
# Release: build\release\tmux.exe
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\release

# Debug: build\win32\tmux.exe
cmake -S . -B build\win32 -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build\win32
ctest --test-dir build\win32 --output-on-failure
```

Ninja ignores `--config`; the build type comes from `-DCMAKE_BUILD_TYPE`.

### Install For The Current User

```powershell
.\install-user.ps1 -Build    # build Release, then install
.\install-user.ps1           # install an existing build\release\tmux.exe
.\install-user.ps1 -DryRun   # show what would be done (same as -WhatIf)
```

The installer:

- copies `build\release\tmux.exe` (`-BuildExe` to choose another) to `%LOCALAPPDATA%\tmux-windows\bin\tmux.exe` (`-InstallDir` to change)
- adds that directory to the User PATH, keeping existing `%VAR%` entries and the value type (`-NoPathUpdate` to skip)
- stops the servers running from the `tmux.exe` it replaces, whatever their `-L` label, so new clients do not attach to a server from an older binary; servers from other tmux copies are left alone

Use `-KeepSessions` only when you intentionally want to keep existing sessions; the running `tmux.exe` is then moved aside:

```powershell
.\install-user.ps1 -KeepSessions
```

Upgrading from a build before the IPC hardening: a server started by the
older `tmux.exe` is refused by the new client (`server ... grants access to
other users; refusing to connect`) and the new `tmux kill-server` cannot
reach it. End that old server once with Task Manager or `Stop-Process` (its
sessions are lost). Without `-KeepSessions`, `install-user.ps1` stops the
servers of the `tmux.exe` it replaces using that old binary.

### Basic Usage

```powershell
tmux new -s main
tmux ls
tmux attach -t main
tmux kill-session -t main
tmux kill-server
```

Inside tmux:

```text
Ctrl-b d        detach
Ctrl-b c        new window
Ctrl-b n        next window
Ctrl-b p        previous window
Ctrl-b %        vertical split
Ctrl-b "        horizontal split
Ctrl-b arrows   move between panes
Ctrl-b x        kill pane
Ctrl-b [        copy mode
q               leave copy mode
```

### Configuration

Config file:

```powershell
$env:USERPROFILE\.tmux.conf
```

Example:

```tmux
set -g mouse on
setw -g mode-keys vi
set -g history-limit 10000
unbind -T copy-mode-vi MouseDragEnd1Pane
set -g set-clipboard on
```

Reload a running server:

```powershell
tmux source-file $env:USERPROFILE\.tmux.conf
```

### Status And Caveats

- This is still an experimental Windows-native port.
- Full upstream tmux parity is not guaranteed yet.
- The clipboard is set through OSC 52, so it depends on the terminal supporting it.
- There is no job control: `suspend-client` only redraws. `-CC` is untested.
- `#{pane_current_command}` and `#{pane_current_path}` are approximations; in PowerShell, `pane_current_path` follows `cd` only with the profile snippet in [README_WIN32.MD](README_WIN32.MD) (PowerShell 7.2+).
- Package manager distribution and advanced plugin compatibility are post-MVP work.

For the implementation details, Windows architecture notes, and regression tests, see [README_WIN32.MD](README_WIN32.MD).

## Source Provenance / 출처

이 저장소는 upstream [tmux/tmux](https://github.com/tmux/tmux)를 기준 소스로 사용합니다. Windows 포트 작업은 이 저장소에서 네이티브 Windows `tmux.exe`를 목표로 유지하며, `arndawg/tmux-windows`는 개발 중 참고 구현으로 사용했습니다.

This repository is based on upstream [tmux/tmux](https://github.com/tmux/tmux). The Windows work is maintained here as a native Windows `tmux.exe` port, with `arndawg/tmux-windows` used as an implementation reference during development.

## License

tmux is distributed under the ISC license. See [COPYING](COPYING) and license notices in individual source files.
