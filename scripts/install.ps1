param(
    [string]$Repo = $(if ($env:KIKO_REPO) { $env:KIKO_REPO } else { "suir1/kiko" }),
    [string]$Version = $env:KIKO_VERSION,
    [string]$InstallDir = $(if ($env:KIKO_INSTALL_DIR) { $env:KIKO_INSTALL_DIR } else { Join-Path $HOME "bin" }),
    [switch]$AddToPath,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Enable-KikoTls {
    try {
        $protocols = [Net.ServicePointManager]::SecurityProtocol
        foreach ($name in @("Tls12", "Tls13")) {
            try {
                $protocols = $protocols -bor ([Net.SecurityProtocolType]::$name)
            }
            catch {
                # Older Windows PowerShell/.NET releases may not expose newer protocol names.
            }
        }
        [Net.ServicePointManager]::SecurityProtocol = $protocols
    }
    catch {
        # PowerShell Core on some platforms may ignore ServicePointManager; downloads can still proceed.
    }
}

function Test-TruthyEnv {
    param([string]$Value)
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    return $Value.ToLowerInvariant() -in @("1", "true", "yes", "on")
}

function Get-KikoErrorText {
    param([object]$ErrorRecord)

    if ($null -eq $ErrorRecord) {
        return "unknown error"
    }
    $message = $ErrorRecord.Exception.Message
    try {
        $response = $ErrorRecord.Exception.Response
        if ($null -ne $response) {
            $status = $response.StatusCode
            if ($null -ne $status) {
                $message = "$message ($([int]$status) $status)"
            }
        }
    }
    catch {
    }
    return $message
}

function Invoke-KikoRest {
    param(
        [string]$Uri,
        [hashtable]$Headers = @{}
    )

    try {
        return Invoke-RestMethod -Uri $Uri -Headers $Headers -ErrorAction Stop
    }
    catch {
        Write-Warning "Could not query $Uri: $(Get-KikoErrorText $_)"
        return $null
    }
}

function Invoke-KikoWebContent {
    param([string]$Uri)

    try {
        $response = Invoke-WebRequest -Uri $Uri -UseBasicParsing -ErrorAction Stop
        return $response.Content
    }
    catch {
        Write-Warning "Could not fetch $Uri: $(Get-KikoErrorText $_)"
        return $null
    }
}

function Get-KikoLatestFromRedirect {
    param([string]$Repo)

    $latestUrl = "https://github.com/$Repo/releases/latest"
    try {
        $response = Invoke-WebRequest -Uri $latestUrl -UseBasicParsing -MaximumRedirection 0 -ErrorAction Stop
        $location = $response.Headers.Location
    }
    catch {
        $location = $null
        try {
            if ($null -ne $_.Exception.Response) {
                $location = $_.Exception.Response.Headers["Location"]
            }
        }
        catch {
        }
    }
    if ($location -and $location -match "/tag/(v[^/?#]+)") {
        return $Matches[1]
    }
    return ""
}

function Save-KikoFile {
    param(
        [string]$Uri,
        [string]$OutFile,
        [string]$Archive,
        [string]$Version,
        [string]$Asset,
        [string]$Repo
    )

    $errors = New-Object System.Collections.Generic.List[string]
    try {
        Invoke-WebRequest -Uri $Uri -OutFile $OutFile -UseBasicParsing -ErrorAction Stop
        return
    }
    catch {
        $errors.Add("Invoke-WebRequest: $(Get-KikoErrorText $_)")
    }

    try {
        $client = New-Object System.Net.WebClient
        $client.Headers.Add("User-Agent", "kiko-install")
        $client.DownloadFile($Uri, $OutFile)
        return
    }
    catch {
        $errors.Add("WebClient: $(Get-KikoErrorText $_)")
    }
    finally {
        if ($client) {
            $client.Dispose()
        }
    }

    $releaseUrl = "https://github.com/$Repo/releases/tag/$Version"
    $message = @(
        "Failed to download $Archive.",
        "Tried: $Uri",
        "Release page: $releaseUrl",
        "Expected asset: $Asset",
        "Errors:",
        "  $($errors -join "`n  ")",
        "If GitHub is blocked on this network, download the zip manually from the release page and unzip kiko.exe into a directory on PATH."
    ) -join [Environment]::NewLine
    throw $message
}

function Get-KikoAddToPathMode {
    if ($AddToPath) {
        return "1"
    }
    if ([string]::IsNullOrWhiteSpace($env:KIKO_ADD_TO_PATH)) {
        return "0"
    }
    $value = $env:KIKO_ADD_TO_PATH.ToLowerInvariant()
    if ($value -eq "prompt") {
        return "prompt"
    }
    if (Test-TruthyEnv $env:KIKO_ADD_TO_PATH) {
        return "1"
    }
    return "0"
}

function Test-KikoShouldAddToPath {
    param(
        [string]$Mode,
        [string]$Directory
    )

    if ($Mode -eq "1") {
        return $true
    }
    if ($Mode -eq "prompt") {
        try {
            if ([Environment]::UserInteractive) {
                $answer = Read-Host "Add $Directory to your user PATH for future PowerShell sessions? [y/N]"
                return $answer -in @("y", "Y", "yes", "YES")
            }
        }
        catch {
        }
    }
    return $false
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

Enable-KikoTls
$addToPathMode = Get-KikoAddToPathMode

if (-not $Version) {
    $headers = @{
        Accept = "application/vnd.github+json"
        "User-Agent" = "kiko-install"
    }
    $releases = Invoke-KikoRest -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers
    $firstRelease = @($releases | Where-Object { $_.tag_name } | Select-Object -First 1)
    if ($firstRelease -and $firstRelease.tag_name) {
        $Version = $firstRelease.tag_name
    }
}

if (-not $Version) {
    $feedContent = Invoke-KikoWebContent -Uri "https://github.com/$Repo/releases.atom"
    if ($feedContent) {
        [xml]$feed = $feedContent
        $Version = @($feed.feed.entry | Where-Object { $_.title -like "v*" } | Select-Object -First 1).title
    }
}

if (-not $Version) {
    $Version = Get-KikoLatestFromRedirect -Repo $Repo
}

if (-not $Version) {
    throw "Could not determine latest kiko release from https://github.com/$Repo/releases. Set KIKO_VERSION=v0.2.0-alpha and retry."
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
    Write-Host "add_to_path=$addToPathMode"
    return
}

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kiko-install-" + [System.Guid]::NewGuid().ToString("N"))
$zipPath = Join-Path $tmpDir $archive

New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

try {
    Write-Host "Downloading $url"
    Save-KikoFile -Uri $url -OutFile $zipPath -Archive $archive -Version $Version -Asset $asset -Repo $Repo

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
    if (Test-KikoShouldAddToPath -Mode $addToPathMode -Directory $InstallDir) {
        Add-KikoInstallDirToUserPath -Directory $InstallDir
    }
    elseif (-not (Test-PathContainsDirectory -PathValue $env:Path -Directory $InstallDir)) {
        Write-Host "Add $InstallDir to PATH to run 'kiko' from anywhere, or rerun with KIKO_ADD_TO_PATH=1."
    }
}
finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
