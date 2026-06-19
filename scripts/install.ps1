param(
    [string]$Repo = $(if ($env:KIKO_REPO) { $env:KIKO_REPO } else { "suir1/kiko" }),
    [string]$Version = $env:KIKO_VERSION,
    [string]$InstallDir = $(if ($env:KIKO_INSTALL_DIR) { $env:KIKO_INSTALL_DIR } else { Join-Path $HOME "bin" }),
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not $DryRun -and $env:KIKO_INSTALL_DRY_RUN) {
    $DryRun = $env:KIKO_INSTALL_DRY_RUN.ToLowerInvariant() -in @("1", "true", "yes", "on")
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
    throw "Could not determine latest kiko release from https://github.com/$Repo/releases. Set KIKO_VERSION=v0.1.4-alpha and retry."
}

$arch = if ($env:KIKO_TEST_ARCH) { $env:KIKO_TEST_ARCH } else { [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() }
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
    $pathEntries = $env:Path -split [System.IO.Path]::PathSeparator
    if ($pathEntries -notcontains $InstallDir) {
        Write-Host "Add $InstallDir to PATH to run 'kiko' from anywhere."
    }
}
finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
