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
- `%USERPROFILE%\.tmux.conf` 설정 파일

### 요구 사항

- Windows 10 1809 이상
- Windows Terminal 권장
- 로컬 빌드 시 Visual Studio Build Tools 또는 MSVC/clang-cl
- CMake
- vcpkg `libevent:x64-windows-static`

### 빌드

환경에 따라 vcpkg toolchain 경로는 조정해야 합니다.

```powershell
cmake -S . -B build\win32 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

cmake --build build\win32 --config Debug
```

빌드 결과물:

```text
build\win32\tmux.exe
```

### 설치

로컬 빌드 결과를 사용자 PATH에 설치하려면:

```powershell
.\install-user.ps1
```

이 스크립트는 다음을 수행합니다.

- `build\win32\tmux.exe`를 `%LOCALAPPDATA%\tmux-windows\bin\tmux.exe`로 복사
- 해당 디렉터리를 User PATH에 추가
- 오래된 tmux 서버/세션 찌꺼기로 새 클라이언트가 잘못 붙는 문제를 막기 위해 기존 native tmux 서버를 먼저 종료

현재 세션을 일부러 유지하고 싶다면:

```powershell
.\install-user.ps1 -KeepSessions
```

단, 실행 중인 서버가 있으면 Windows가 `tmux.exe` 교체를 막을 수 있습니다.

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
- clipboard 동작은 Windows Terminal/ConPTY/OSC 52 지원 상태에 영향을 받을 수 있습니다.
- package manager 배포, control mode 완전 호환, 고급 copy-mode/mouse 동작은 계속 검증이 필요합니다.

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
- `%USERPROFILE%\.tmux.conf`

### Requirements

- Windows 10 version 1809 or newer
- Windows Terminal recommended
- Visual Studio Build Tools or MSVC/clang-cl for local builds
- CMake
- vcpkg `libevent:x64-windows-static`

### Build

Adjust the vcpkg toolchain path for your environment.

```powershell
cmake -S . -B build\win32 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static

cmake --build build\win32 --config Debug
```

The expected local binary is:

```text
build\win32\tmux.exe
```

### Install For The Current User

```powershell
.\install-user.ps1
```

The installer:

- copies `build\win32\tmux.exe` to `%LOCALAPPDATA%\tmux-windows\bin\tmux.exe`
- adds that directory to the User PATH
- stops existing native tmux servers first, so new clients do not attach to stale servers from an older binary

Use `-KeepSessions` only when you intentionally want to keep existing sessions:

```powershell
.\install-user.ps1 -KeepSessions
```

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
- Clipboard behavior depends on terminal support for the relevant escape sequences.
- Package manager distribution, full control mode compatibility, and advanced plugin compatibility are post-MVP work.

For the implementation details, Windows architecture notes, and regression tests, see [README_WIN32.MD](README_WIN32.MD).

## Source Provenance / 출처

이 저장소는 upstream [tmux/tmux](https://github.com/tmux/tmux)를 기준 소스로 사용합니다. Windows 포트 작업은 이 저장소에서 네이티브 Windows `tmux.exe`를 목표로 유지하며, `arndawg/tmux-windows`는 개발 중 참고 구현으로 사용했습니다.

This repository is based on upstream [tmux/tmux](https://github.com/tmux/tmux). The Windows work is maintained here as a native Windows `tmux.exe` port, with `arndawg/tmux-windows` used as an implementation reference during development.

## License

tmux is distributed under the ISC license. See [COPYING](COPYING) and license notices in individual source files.
