param(
    [string]$Repo = $(if ($env:KIKO_REPO) { $env:KIKO_REPO } else { "suir1/kiko" }),
    [string]$Version = $env:KIKO_VERSION,
    [string]$InstallDir = $(if ($env:KIKO_INSTALL_DIR) { $env:KIKO_INSTALL_DIR } else { Join-Path $HOME "bin" }),
    [switch]$AddToPath,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Test-TruthyEnv {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    return $Value.ToLowerInvariant() -in @("1", "true", "yes", "on")
}

function Get-KikoWindowsArch {
    if ($env:KIKO_TEST_ARCH) {
        return $env:KIKO_TEST_ARCH
    }

    try {
        $runtimeArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
        if ($null -ne $runtimeArch) {
            $runtimeArchText = $runtimeArch.ToString()
            if (-not [string]::IsNullOrWhiteSpace($runtimeArchText)) {
                return $runtimeArchText
            }
        }
    }
    catch {
        # Windows PowerShell 5.1 may not expose RuntimeInformation.OSArchitecture.
    }

    foreach ($candidate in @($env:PROCESSOR_ARCHITECTURE, $env:PROCESSOR_ARCHITEW6432)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
    }

    return ""
}

function Test-PathContainsDirectory {
    param(
        [string]$PathValue,
        [string]$Directory
    )
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return $false
    }
    $trimChars = [char[]]@("\", "/")
    $target = $Directory.TrimEnd($trimChars)
    foreach ($entry in ($PathValue -split [System.IO.Path]::PathSeparator)) {
        if ($entry.TrimEnd($trimChars) -ieq $target) {
            return $true
        }
    }
    return $false
}

function Add-KikoInstallDirToUserPath {
    param([string]$Directory)

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (Test-PathContainsDirectory -PathValue $userPath -Directory $Directory) {
        Write-Host "$Directory is already in the user PATH."
    }
    else {
        $newUserPath = if ([string]::IsNullOrWhiteSpace($userPath)) {
            $Directory
        }
        else {
            $userPath.TrimEnd([System.IO.Path]::PathSeparator) + [System.IO.Path]::PathSeparator + $Directory
        }
        [Environment]::SetEnvironmentVariable("Path", $newUserPath, "User")
        Write-Host "Added $Directory to the user PATH. Open a new terminal to use 'kiko' from anywhere."
    }

    if (-not (Test-PathContainsDirectory -PathValue $env:Path -Directory $Directory)) {
        $env:Path = $Directory + [System.IO.Path]::PathSeparator + $env:Path
    }
}

if (-not $DryRun -and $env:KIKO_INSTALL_DRY_RUN) {
    $DryRun = Test-TruthyEnv $env:KIKO_INSTALL_DRY_RUN
}

if (-not $AddToPath -and $env:KIKO_ADD_TO_PATH) {
    $AddToPath = Test-TruthyEnv $env:KIKO_ADD_TO_PATH
}

if (-not $Version) {
    try {
        $headers = @{
            Accept = "application/vnd.github+json"
            "User-Agent" = "kiko-install"
        }
        $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers
        if ($releases -and $releases[0].tag_name) {
            $Version = $releases[0].tag_name
        }
    }
    catch {
        $Version = ""
    }
}

if (-not $Version) {
    try {
        [xml]$feed = (Invoke-WebRequest -Uri "https://github.com/$Repo/releases.atom").Content
        $Version = @($feed.feed.entry | Where-Object { $_.title -like "v*" } | Select-Object -First 1).title
    }
    catch {
        $Version = ""
    }
}

if (-not $Version) {
    throw "Could not determine latest kiko release from https://github.com/$Repo/releases. Set KIKO_VERSION=v0.1.8-alpha and retry."
}

$arch = Get-KikoWindowsArch
switch ($arch.ToLowerInvariant()) {
    { $_ -in @("x64", "x86_64", "amd64") } { $asset = "windows-x64"; break }
    default { throw "Unsupported Windows architecture: $arch" }
}

$archive = "kiko-$Version-$asset.zip"
$url = "https://github.com/$Repo/releases/download/$Version/$archive"

if ($DryRun) {
    Write-Host "version=$Version"
    Write-Host "asset=$asset"
    Write-Host "archive=$archive"
    Write-Host "url=$url"
    Write-Host "install_dir=$InstallDir"
    Write-Host "add_to_path=$([int][bool]$AddToPath)"
    return
}

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kiko-install-" + [System.Guid]::NewGuid().ToString("N"))
$zipPath = Join-Path $tmpDir $archive

New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

try {
    Write-Host "Downloading $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $zipPath
    }
    catch {
        throw "Failed to download $archive. Check that release $Version has a $asset package at https://github.com/$Repo/releases/tag/$Version"
    }

    try {
        Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force
    }
    catch {
        throw "Failed to extract $archive"
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $sourcePath = Join-Path $tmpDir "kiko-$Version-$asset/kiko.exe"
    if (-not (Test-Path $sourcePath)) {
        throw "Release archive did not contain kiko-$Version-$asset/kiko.exe"
    }
    $targetPath = Join-Path $InstallDir "kiko.exe"
    Copy-Item -Path $sourcePath -Destination $targetPath -Force
    Copy-Item -Path (Join-Path $tmpDir "kiko-$Version-$asset/*.dll") -Destination $InstallDir -Force -ErrorAction SilentlyContinue

    Write-Host "Installed kiko $Version to $targetPath"
    if ($AddToPath) {
        Add-KikoInstallDirToUserPath -Directory $InstallDir
    }
    elseif (-not (Test-PathContainsDirectory -PathValue $env:Path -Directory $InstallDir)) {
        Write-Host "Add $InstallDir to PATH to run 'kiko' from anywhere, or rerun with KIKO_ADD_TO_PATH=1."
    }
}
finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
