param(
    [string]$Repo = $(if ($env:KIKO_REPO) { $env:KIKO_REPO } else { "suir1/kiko" }),
    [string]$Version = $env:KIKO_VERSION,
    [string]$InstallDir = $(if ($env:KIKO_INSTALL_DIR) { $env:KIKO_INSTALL_DIR } else { Join-Path $HOME "bin" })
)

$ErrorActionPreference = "Stop"

if (-not $Version) {
    $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases"
    if (-not $releases -or -not $releases[0].tag_name) {
        throw "Could not determine latest kiko release. Set KIKO_VERSION=v0.1.0-alpha and retry."
    }
    $Version = $releases[0].tag_name
}

$asset = "windows-x64"
$archive = "kiko-$Version-$asset.zip"
$url = "https://github.com/$Repo/releases/download/$Version/$archive"
$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kiko-install-" + [System.Guid]::NewGuid().ToString("N"))
$zipPath = Join-Path $tmpDir $archive

New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

try {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $sourcePath = Join-Path $tmpDir "kiko-$Version-$asset/kiko.exe"
    $targetPath = Join-Path $InstallDir "kiko.exe"
    Copy-Item -Path $sourcePath -Destination $targetPath -Force

    Write-Host "Installed kiko $Version to $targetPath"
    $pathEntries = $env:Path -split [System.IO.Path]::PathSeparator
    if ($pathEntries -notcontains $InstallDir) {
        Write-Host "Add $InstallDir to PATH to run 'kiko' from anywhere."
    }
}
finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
