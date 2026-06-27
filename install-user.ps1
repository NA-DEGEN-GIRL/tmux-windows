param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'tmux-windows\bin'),
    [string]$BuildExe = (Join-Path $PSScriptRoot 'build\win32\tmux.exe'),
    [switch]$KeepSessions
)

$ErrorActionPreference = 'Stop'

function Get-NormalizedPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    try {
        return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    } catch {
        return $Path.TrimEnd('\')
    }
}

function Stop-ExistingTmux {
    param(
        [string]$InstalledExe,
        [string]$BuildExe
    )

    if ($KeepSessions) {
        Write-Host "Keeping existing tmux sessions. New clients may attach to the old running server until you run 'tmux kill-server'."
        return
    }

    $candidateExes = @()
    foreach ($path in @($InstalledExe, $BuildExe)) {
        if (Test-Path -LiteralPath $path) {
            $candidateExes += (Get-NormalizedPath (Resolve-Path -LiteralPath $path).Path)
        }
    }
    $candidateExes = @($candidateExes | Sort-Object -Unique)
    if ($candidateExes.Count -eq 0) {
        return
    }

    Write-Host "Stopping existing tmux servers before install."
    foreach ($exe in $candidateExes) {
        $killCommand = '"' + $exe + '" kill-server >nul 2>nul'
        & cmd.exe /d /s /c $killCommand | Out-Null
    }
    Start-Sleep -Milliseconds 500

    $leftovers = @()
    foreach ($process in (Get-Process -Name tmux -ErrorAction SilentlyContinue)) {
        try {
            $processPath = Get-NormalizedPath $process.Path
        } catch {
            continue
        }
        if ($candidateExes -contains $processPath) {
            $leftovers += $process
        }
    }

    if ($leftovers.Count -gt 0) {
        Write-Host "Forcing remaining tmux processes to exit before replacing tmux.exe."
        $leftovers | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }
}

if (!(Test-Path -LiteralPath $BuildExe)) {
    throw "tmux.exe not found at $BuildExe. Build first with CMake."
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$installedExe = Join-Path $InstallDir 'tmux.exe'
Stop-ExistingTmux -InstalledExe $installedExe -BuildExe $BuildExe
try {
    Copy-Item -LiteralPath $BuildExe -Destination $installedExe -Force
} catch {
    if ($KeepSessions) {
        throw "Unable to replace tmux.exe while existing sessions are kept. Run 'tmux kill-server' or rerun this script without -KeepSessions."
    }
    throw
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$parts = @()
if (![string]::IsNullOrWhiteSpace($userPath)) {
    $parts = $userPath -split ';' | Where-Object { $_ -ne '' }
}

$alreadyPresent = $false
foreach ($part in $parts) {
    if ([string]::Equals(
            [Environment]::ExpandEnvironmentVariables($part).TrimEnd('\'),
            $InstallDir.TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase)) {
        $alreadyPresent = $true
        break
    }
}

if (!$alreadyPresent) {
    $newPath = if ([string]::IsNullOrWhiteSpace($userPath)) {
        $InstallDir
    } else {
        $userPath.TrimEnd(';') + ';' + $InstallDir
    }
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
}

$env:Path = $InstallDir + ';' + $env:Path

Write-Host "Installed tmux.exe to $InstallDir"
if ($alreadyPresent) {
    Write-Host "User PATH already contains the install directory."
} else {
    Write-Host "Added the install directory to the User PATH. Open a new terminal for it to apply globally."
}
Write-Host "This terminal can use: tmux -V"
