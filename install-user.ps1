param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'tmux-windows\bin'),
    [string]$BuildExe = (Join-Path $PSScriptRoot 'build\win32\tmux.exe')
)

$ErrorActionPreference = 'Stop'

if (!(Test-Path -LiteralPath $BuildExe)) {
    throw "tmux.exe not found at $BuildExe. Build first with CMake."
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -LiteralPath $BuildExe -Destination (Join-Path $InstallDir 'tmux.exe') -Force

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
