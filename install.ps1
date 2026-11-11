# Quirk installer for Windows — fetches the latest GitHub Release and unpacks it.
#
# One-liner (PowerShell):
#     irm https://raw.githubusercontent.com/AlexVachon/quirk/main/install.ps1 | iex
#
# With options, download it first:
#     irm https://raw.githubusercontent.com/AlexVachon/quirk/main/install.ps1 -OutFile install.ps1
#     .\install.ps1 -Version v7.25.0 -Dir C:\tools\quirk -WithExtension
#
# Environment equivalents (handy with `| iex`): QUIRK_REPO, QUIRK_VERSION,
# INSTALL_DIR, INSTALL_EXTENSION=1, CODE_CMD, QUIRK_NO_MODIFY_PATH=1.
# QUIRK_INSTALL_TARBALL=<path to quirk-<ver>-windows-x86_64.tar.gz> installs
# that file instead of downloading (CI tests the installer this way).
#
# Layout after install:
#     <Dir>\bin\quirk.exe, runtime.dll, runtime.lib, and the DLLs they load
#     <Dir>\packages\...            (stdlib)
#
# Windows support is experimental (see quirk-compiler/docs/WINDOWS-PORT.md).
# Building executables (`quirk -o`) also needs a linker: Visual Studio
# Build Tools (link.exe + the Windows SDK), or LLVM's lld-link.

param(
    [string]$Version = $env:QUIRK_VERSION,
    [string]$Dir = $env:INSTALL_DIR,
    [switch]$WithExtension,
    [switch]$NoModifyPath
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # Invoke-WebRequest is much faster without the bar
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$repo = if ($env:QUIRK_REPO) { $env:QUIRK_REPO } else { 'AlexVachon/quirk' }
if (-not $Dir) { $Dir = Join-Path $env:USERPROFILE '.quirk' }
if ($env:INSTALL_EXTENSION -eq '1') { $WithExtension = $true }
if ($env:QUIRK_NO_MODIFY_PATH -eq '1') { $NoModifyPath = $true }

# --- Platform ----------------------------------------------------------------
$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
if ($arch -ne 'AMD64') {
    Write-Error "Quirk ships Windows x86_64 only (this machine: $arch). Build from source: https://github.com/$repo/blob/main/quirk-compiler/docs/WINDOWS-PORT.md"
}
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tar)) {
    Write-Error "tar.exe not found in System32 (it ships with Windows 10 1803 and later)."
}

# --- Resolve version ---------------------------------------------------------
# /releases/latest may be an extension release (vscode-v*): take the first
# `v<digit>...` tag without a pre-release suffix.
$local = $env:QUIRK_INSTALL_TARBALL
if ($local) {
    if (-not ((Split-Path -Leaf $local) -match '^quirk-(.+)-windows-x86_64\.tar\.gz$')) { Write-Error "QUIRK_INSTALL_TARBALL must be a quirk-<version>-windows-x86_64.tar.gz" }
    $Version = "v" + $Matches[1]
}
if (-not $Version) {
    $api = "https://api.github.com/repos/$repo/releases?per_page=30"
    $releases = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'quirk-install' }
    $rel = $releases | Where-Object { $_.tag_name -match '^v\d[^-]*$' } | Select-Object -First 1
    if (-not $rel) { Write-Error "Failed to resolve a compiler release from $api" }
    $Version = $rel.tag_name
}
$ver = $Version.TrimStart('v')
$pkg = "quirk-$ver-windows-x86_64"
$url = "https://github.com/$repo/releases/download/$Version/$pkg.tar.gz"

Write-Host "Installing Quirk $ver -> $Dir"

# --- Download + verify -------------------------------------------------------
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("quirk-install-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $tarball = Join-Path $tmp "$pkg.tar.gz"
    if ($local) {
        Copy-Item $local $tarball
        if (Test-Path "$local.sha256") { Copy-Item "$local.sha256" "$tarball.sha256" }
    } else {
        try {
            Invoke-WebRequest -Uri $url -OutFile $tarball -UseBasicParsing
        } catch {
            Write-Error "Download failed: $url`n(release $Version may have no Windows build — Windows builds start with 7.25.0)"
        }
    }
    if ($local) {
        if (Test-Path "$tarball.sha256") {
            $expected = ((Get-Content "$tarball.sha256" -Raw) -split '\s+')[0].ToLower()
            $actual = (Get-FileHash -Algorithm SHA256 $tarball).Hash.ToLower()
            if ($expected -ne $actual) { Write-Error "Checksum mismatch! expected $expected got $actual" }
            Write-Host "Verified sha256 $($actual.Substring(0, 16))..."
        }
    } else { try {
        $sidecar = (Invoke-WebRequest -Uri "$url.sha256" -UseBasicParsing).Content
        if ($sidecar -is [byte[]]) { $sidecar = [Text.Encoding]::ASCII.GetString($sidecar) }
        $expected = ($sidecar -split '\s+')[0].ToLower()
        $actual = (Get-FileHash -Algorithm SHA256 $tarball).Hash.ToLower()
        if ($expected -ne $actual) { Write-Error "Checksum mismatch! expected $expected got $actual" }
        Write-Host "Verified sha256 $($actual.Substring(0, 16))..."
    } catch [System.Net.WebException] {
        Write-Warning "No checksum published for $Version — not verified"
    } }

    # --- Extract + install ---------------------------------------------------
    & $tar -xzf $tarball -C $tmp
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $tmp "$pkg\bin\quirk.exe"))) {
        Write-Error "Couldn't unpack $pkg.tar.gz"
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    foreach ($d in 'bin', 'lib', 'libs', 'packages') {
        $p = Join-Path $Dir $d
        if (Test-Path $p) {
            try { Remove-Item -Recurse -Force $p }
            catch { Write-Error "Couldn't replace $p — is quirk (or a program it built) still running? Close it and retry." }
        }
    }
    Copy-Item -Recurse -Force (Join-Path $tmp "$pkg\*") $Dir

    $bin = Join-Path $Dir 'bin'
    Write-Host ""
    Write-Host "Installed Quirk $ver."

    # --- PATH ------------------------------------------------------------------
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $onPath = ($userPath -split ';') -contains $bin
    if ($NoModifyPath) {
        if (-not $onPath) {
            Write-Host ""
            Write-Host "Add $bin to your PATH to use it:"
            Write-Host "    [Environment]::SetEnvironmentVariable('Path', `"$bin;`" + [Environment]::GetEnvironmentVariable('Path', 'User'), 'User')"
        }
    } elseif (-not $onPath) {
        $newPath = if ($userPath) { "$bin;$userPath" } else { $bin }
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
        Write-Host "Added $bin to your user PATH (open a new terminal to pick it up)."
    }
    $env:Path = "$bin;$env:Path"

    # --- VSCode extension (opt-in) ---------------------------------------------
    $code = if ($env:CODE_CMD) { $env:CODE_CMD } else { 'code' }
    if ($WithExtension) {
        if (-not (Get-Command $code -ErrorAction SilentlyContinue)) {
            Write-Host "'$code' is not on PATH — skipping the VSCode extension (set CODE_CMD=codium / cursor / ...)."
        } else {
            $all = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases" -Headers @{ 'User-Agent' = 'quirk-install' }
            $vsix = $all | ForEach-Object { $_.assets } | Where-Object { $_.name -like '*.vsix' } | Select-Object -First 1
            if (-not $vsix) {
                Write-Host "Couldn't locate a published Quirk VSCode extension. Skipping."
            } else {
                $vsixPath = Join-Path $tmp $vsix.name
                Invoke-WebRequest -Uri $vsix.browser_download_url -OutFile $vsixPath -UseBasicParsing
                Write-Host "Installing the Quirk VSCode extension into '$code'..."
                & $code --install-extension $vsixPath
                if ($LASTEXITCODE -ne 0) { Write-Host "Extension install failed; install it manually: $code --install-extension $($vsix.browser_download_url)" }
            }
        }
    } else {
        Write-Host ""
        Write-Host "VSCode extension (optional): re-run with -WithExtension, or download the .vsix from"
        Write-Host "    https://github.com/$repo/releases"
    }
    Write-Host ""
    Write-Host "Then open a new terminal and run:  quirk --version"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
