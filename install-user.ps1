<#
.SYNOPSIS
Install tmux.exe for the current user and add it to the user PATH.

.DESCRIPTION
Copies the Release build (build\release\tmux.exe by default) to
%LOCALAPPDATA%\tmux-windows\bin and appends that directory to the user PATH.

Running tmux servers started from the tmux.exe being replaced are asked to
exit with kill-server (every label, not only the default one); processes of
that tmux.exe still running afterwards are stopped. Servers started from any
other tmux.exe are left alone.

The user PATH is edited in the registry as stored, so entries such as
%USERPROFILE%\bin stay unexpanded and the value keeps its type
(REG_EXPAND_SZ); running programs are then told the environment changed.

.PARAMETER InstallDir
Directory to install tmux.exe into. Relative paths are taken from the
current location.

.PARAMETER BuildExe
tmux.exe to install. Defaults to the Release build, build\release\tmux.exe.

.PARAMETER Build
Configure and build the Release build first (Visual Studio C++ build tools,
CMake, Ninja and vcpkg, as in README_WIN32.MD).

.PARAMETER KeepSessions
Do not stop running servers. The old tmux.exe is moved aside so the new one
can be installed; running servers keep using it until they exit.

.PARAMETER NoPathUpdate
Do not change the user PATH.

.PARAMETER DryRun
Show what would be done without doing it (the same as -WhatIf).

.PARAMETER EnvironmentKey
Registry key under HKEY_CURRENT_USER holding the user environment. Only for
testing this script against a scratch key.

.EXAMPLE
.\install-user.ps1 -Build

.EXAMPLE
.\install-user.ps1 -DryRun
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'tmux-windows\bin'),
    [string]$BuildExe,
    [switch]$Build,
    [switch]$KeepSessions,
    [switch]$NoPathUpdate,
    [switch]$DryRun,
    [string]$EnvironmentKey = 'Environment'
)

$ErrorActionPreference = 'Stop'
# Native commands report failure through $LASTEXITCODE, checked below; do not
# let PowerShell 7.3+ turn kill-server's "no server" exit status into an error.
$PSNativeCommandUseErrorActionPreference = $false
if ($DryRun) {
    $WhatIfPreference = $true
}

# Do not depend on PATH to find cmd.exe.
$Cmd = Join-Path ([Environment]::SystemDirectory) 'cmd.exe'

$BuildDir = Join-Path $PSScriptRoot 'build\release'
if ([string]::IsNullOrEmpty($BuildExe)) {
    $BuildExe = Join-Path $BuildDir 'tmux.exe'
}

# Resolve paths against the PowerShell location, not the process directory.
$InstallDir = $PSCmdlet.GetUnresolvedProviderPathFromPSPath($InstallDir).TrimEnd('\')
$BuildExe = $PSCmdlet.GetUnresolvedProviderPathFromPSPath($BuildExe)
$InstalledExe = Join-Path $InstallDir 'tmux.exe'

function Get-NormalizedPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim().Trim('"'))
    try {
        $expanded = [System.IO.Path]::GetFullPath($expanded)
    } catch {
    }
    if ($expanded.Length -gt 3) {
        $expanded = $expanded.TrimEnd('\')
    }
    return $expanded
}

function Invoke-ReleaseBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found: install the Visual Studio C++ build tools."
    }
    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($vs)) {
        throw "No Visual Studio installation with the C++ x64 tools was found."
    }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'

    $vcpkgRoot = $env:VCPKG_ROOT
    if ([string]::IsNullOrEmpty($vcpkgRoot) -or
        !(Test-Path -LiteralPath $vcpkgRoot)) {
        $vcpkgRoot = Join-Path $vs 'VC\vcpkg'
    }
    $toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (!(Test-Path -LiteralPath $toolchain)) {
        throw "vcpkg not found: set VCPKG_ROOT or install the Visual Studio vcpkg component."
    }

    # Run the build from a batch file: quoting a command line for cmd.exe
    # differs between PowerShell versions. % is doubled for cmd.exe and the
    # file is UTF-8 for paths outside the console code page.
    $q = { param($path) '"' + $path.Replace('%', '%%') + '"' }
    $configure = 'cmake -S {0} -B {1} -G Ninja -DCMAKE_BUILD_TYPE=Release ' +
        '-DCMAKE_TOOLCHAIN_FILE={2} -DVCPKG_TARGET_TRIPLET=x64-windows-static' +
        ' || exit /b 1'
    $configure = $configure -f (& $q $PSScriptRoot), (& $q $BuildDir),
        (& $q $toolchain)
    $lines = @(
        '@echo off',
        'chcp 65001 >nul',
        ('call {0} x64 >nul || exit /b 1' -f (& $q $vcvars)),
        $configure,
        ('cmake --build {0}' -f (& $q $BuildDir))
    )

    if ($PSCmdlet.ShouldProcess($BuildDir, 'Configure and build Release')) {
        $bat = Join-Path ([IO.Path]::GetTempPath()) (
            'tmux-build-{0}.cmd' -f [Guid]::NewGuid().ToString('N'))
        [IO.File]::WriteAllText($bat, ($lines -join "`r`n") + "`r`n",
            (New-Object Text.UTF8Encoding $false))
        try {
            & $Cmd /d /c $bat
            $status = $LASTEXITCODE
        } finally {
            Remove-Item -LiteralPath $bat -ErrorAction SilentlyContinue
        }
        if ($status -ne 0) {
            throw "Release build failed (exit code $status)."
        }
    }
}

# Processes running the tmux.exe that is about to be replaced.
function Get-InstalledTmuxProcesses {
    $target = Get-NormalizedPath $InstalledExe
    Get-CimInstance Win32_Process -Filter "Name = 'tmux.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [string]::Equals((Get-NormalizedPath $_.ExecutablePath), $target,
                [StringComparison]::OrdinalIgnoreCase)
        }
}

function Stop-InstalledTmux {
    if (!(Test-Path -LiteralPath $InstalledExe)) {
        return
    }
    $processes = @(Get-InstalledTmuxProcesses)
    if ($processes.Count -eq 0) {
        return
    }

    # A server's command line is: tmux.exe -D -S "tmux-<user>-<label>" ...
    $sockets = @($processes | ForEach-Object {
            if ($_.CommandLine -match '\s-D\b' -and
                $_.CommandLine -match '\s-S\s*(?:"([^"]+)"|(\S+))') {
                if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
            }
        } | Sort-Object -Unique)

    if ($KeepSessions) {
        Write-Host ("Keeping {0} running tmux server(s); they keep using the " +
            "old tmux.exe until they exit.") -f $sockets.Count
        return
    }

    foreach ($socket in $sockets) {
        if ($PSCmdlet.ShouldProcess($socket, 'Stop tmux server (kill-server)')) {
            # Windows PowerShell turns native stderr into errors when the
            # preference is Stop.
            $ErrorActionPreference = 'Continue'
            & $InstalledExe -S $socket kill-server 2>&1 | Out-Null
            $ErrorActionPreference = 'Stop'
        }
    }
    if ($WhatIfPreference) {
        return
    }

    $deadline = [DateTime]::Now.AddSeconds(5)
    do {
        $left = @(Get-InstalledTmuxProcesses)
        if ($left.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::Now -lt $deadline)

    foreach ($process in $left) {
        if ($PSCmdlet.ShouldProcess("$($process.ProcessId)",
                'Stop remaining tmux process')) {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
    Start-Sleep -Milliseconds 300
}

function Install-Exe {
    if (!$PSCmdlet.ShouldProcess($InstalledExe, "Copy $BuildExe")) {
        return
    }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    try {
        Copy-Item -LiteralPath $BuildExe -Destination $InstalledExe -Force
        return
    } catch {
        if (!(Test-Path -LiteralPath $InstalledExe)) {
            throw
        }
    }

    # A running executable cannot be overwritten but can be renamed.
    $stamp = Get-Date -Format 'yyyyMMddHHmmss'
    $old = "$InstalledExe.old-$stamp"
    Move-Item -LiteralPath $InstalledExe -Destination $old
    Copy-Item -LiteralPath $BuildExe -Destination $InstalledExe -Force
    Write-Host "The old tmux.exe is still in use; it was moved to $old."
}

# Add InstallDir to the user PATH in the registry, keeping the stored
# (unexpanded) entries and the value type.
function Update-UserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($EnvironmentKey)
    try {
        $raw = [string]$key.GetValue('Path', '',
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        if ($key.GetValueNames() -contains 'Path') {
            $kind = $key.GetValueKind('Path')
        }

        $target = Get-NormalizedPath $InstallDir
        foreach ($part in ($raw -split ';')) {
            if ([string]::Equals((Get-NormalizedPath $part), $target,
                    [StringComparison]::OrdinalIgnoreCase)) {
                return $false
            }
        }

        $new = if ([string]::IsNullOrWhiteSpace($raw)) {
            $InstallDir
        } else {
            $raw.TrimEnd(';') + ';' + $InstallDir
        }
        if ($PSCmdlet.ShouldProcess("HKCU\$EnvironmentKey\Path",
                "Append $InstallDir")) {
            $key.SetValue('Path', $new, $kind)
        }
    } finally {
        $key.Dispose()
    }

    if ($EnvironmentKey -eq 'Environment' -and !$WhatIfPreference) {
        Send-EnvironmentChange
    }
    return $true
}

# Tell running programs (Explorer, terminals) that the environment changed.
function Send-EnvironmentChange {
    if (-not ('TmuxInstall.NativeMethods' -as [type])) {
        Add-Type -Namespace TmuxInstall -Name NativeMethods -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg,
    UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout,
    out UIntPtr lpdwResult);
'@
    }
    $result = [UIntPtr]::Zero
    # HWND_BROADCAST, WM_SETTINGCHANGE, SMTO_ABORTIFHUNG, 5 seconds.
    [TmuxInstall.NativeMethods]::SendMessageTimeout([IntPtr]0xffff, 0x1a,
        [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result) | Out-Null
}

if ($Build) {
    Invoke-ReleaseBuild
}
if (!(Test-Path -LiteralPath $BuildExe)) {
    if ($Build -and $WhatIfPreference) {
        Write-Host "What if: $BuildExe would be built."
    } else {
        throw ("tmux.exe not found at $BuildExe. Build it first (see " +
            "README_WIN32.MD) or rerun with -Build.")
    }
}

Stop-InstalledTmux
Install-Exe

$added = $false
if (!$NoPathUpdate) {
    $added = Update-UserPath
}
if (!$WhatIfPreference) {
    $env:Path = $InstallDir + ';' + $env:Path
}

if ($WhatIfPreference) {
    Write-Host "Dry run: nothing was changed."
    return
}
Write-Host "Installed tmux.exe to $InstallDir"
if ($NoPathUpdate) {
    Write-Host "The user PATH was not changed (-NoPathUpdate)."
} elseif ($added) {
    Write-Host "Added the install directory to the user PATH. Open a new terminal for it to apply everywhere."
} else {
    Write-Host "The user PATH already contains the install directory."
}
Write-Host "This terminal can use: tmux -V"
