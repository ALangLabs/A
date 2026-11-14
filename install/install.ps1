# A toolchain installer for Windows (PowerShell 5.1+ / PowerShell 7).
#
#   irm https://raw.githubusercontent.com/ALangLabs/A/main/install/install.ps1 | iex
#
# Downloads the latest GitHub release binary, installs it under
# %LOCALAPPDATA%\Programs\A (override with $env:A_PREFIX), adds it to the
# user PATH, and installs the VS Code extension into every detected
# VS Code / fork (Cursor, Windsurf, VSCodium, ...).
#
# Environment:
#   A_PREFIX    install root (default: %LOCALAPPDATA%\Programs\A)
#   A_VERSION   release tag to install (default: latest)
#   A_NO_EXTENSION=1  skip editor detection/extension install
$ErrorActionPreference = 'Stop'

$Repo       = 'ALangLabs/A'
$Version    = if ($env:A_VERSION) { $env:A_VERSION } else { 'latest' }
$Prefix     = if ($env:A_PREFIX)  { $env:A_PREFIX }  else { Join-Path $env:LOCALAPPDATA 'Programs\A' }
$NoExt      = ($env:A_NO_EXTENSION -eq '1')

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "warning: $msg" -ForegroundColor Yellow }
function Fail($msg)       { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }

# --- platform ---------------------------------------------------------------
$arch = $env:PROCESSOR_ARCHITECTURE
switch ($arch) {
  'AMD64' { $platform = 'windows-x86_64' }
  'ARM64' { $platform = 'windows-arm64' }
  default { Fail "unsupported architecture '$arch'" }
}
if ($Version -eq 'latest') {
  $assetBase = "https://github.com/$Repo/releases/latest/download"
} else {
  $assetBase = "https://github.com/$Repo/releases/download/v$Version"
}

# --- download + extract -----------------------------------------------------
$tmp = Join-Path $env:TEMP ("a-install-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  $pkg = $null
  foreach ($name in @("a-$platform.zip", "a-$platform.tar.gz")) {
    $url = "$assetBase/$name"
    $dest = Join-Path $tmp $name
    try {
      Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
      $pkg = $dest
      break
    } catch { }
  }
  if (-not $pkg) {
    Fail "could not download a release for $platform/$Version - no prebuilt binary. Build from source: https://github.com/$Repo#readme"
  }

  $x = Join-Path $tmp 'x'
  New-Item -ItemType Directory -Path $x | Out-Null
  if ($pkg.EndsWith('.zip')) {
    Expand-Archive -Path $pkg -DestinationPath $x -Force
  } else {
    # Windows 10+ ships bsdtar, which reads .tar.gz.
    & tar.exe -xzf $pkg -C $x
    if ($LASTEXITCODE -ne 0) { Fail "failed to extract $pkg (need Windows 10 1803+ for tar.exe, or download the .zip manually)" }
  }
  $root = Get-ChildItem -Path $x -Directory | Select-Object -First 1
  if (-not $root) { Fail 'malformed release archive' }
  $src = $root.FullName

  # --- install --------------------------------------------------------------
  Write-Step "Installing to $Prefix"
  $binDir = Join-Path $Prefix 'bin'
  New-Item -ItemType Directory -Force -Path $binDir | Out-Null
  Copy-Item (Join-Path $src 'bin\*') $binDir -Force
  Get-ChildItem $Prefix -Directory -Filter 'astralib*' | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
  Get-ChildItem $src -Directory -Filter 'astralib*' | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $Prefix $_.Name) -Recurse -Force
  }
  $stdlibDst = Join-Path $Prefix 'stdlib'
  if (Test-Path $stdlibDst) { Remove-Item $stdlibDst -Recurse -Force }
  Copy-Item (Join-Path $src 'stdlib') $stdlibDst -Recurse -Force

  # --- PATH + A_STDLIB (user scope) ------------------------------------------
  $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  if (-not ($userPath -split ';' | Where-Object { $_ -ieq $binDir })) {
    Write-Step "Adding $binDir to user PATH"
    $newPath = if ($userPath) { "$userPath;$binDir" } else { $binDir }
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    $env:Path = "$env:Path;$binDir"
  }
  # Belt and suspenders: the frontend also finds stdlib next to the exe, but
  # the env var keeps `a` working if the install dir is moved.
  [Environment]::SetEnvironmentVariable('A_STDLIB', $stdlibDst, 'User')
  $env:A_STDLIB = $stdlibDst

  & (Join-Path $binDir 'a.exe') --version | Out-Null
  if ($LASTEXITCODE -ne 0) { Fail 'installed binary failed to run' }
  Write-Step "A toolchain installed: $Prefix"

  if ($NoExt) { Write-Step 'Skipping editor extension (A_NO_EXTENSION=1)'; exit 0 }

  # --- editor extension -------------------------------------------------------
  $vsix = Join-Path $tmp 'a-language.vsix'
  $vsixOk = $false
  try {
    Invoke-WebRequest -Uri "$assetBase/a-language.vsix" -OutFile $vsix -UseBasicParsing
    $vsixOk = $true
  } catch {
    try {
      $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing
      $asset = $rel.assets | Where-Object { $_.name -like 'a-language*.vsix' } | Select-Object -First 1
      if ($asset) {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $vsix -UseBasicParsing
        $vsixOk = $true
      }
    } catch { }
  }
  if (-not $vsixOk) {
    Write-Warn 'editor extension package not found in the release; skipping'
    exit 0
  }

  $editors = New-Object System.Collections.Generic.List[string]
  foreach ($name in @('code','code-insiders','codium','vscodium','cursor','windsurf','trae','kiro','void','melty')) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { $editors.Add($cmd.Source) }
  }
  # Well-known per-user install locations + registry uninstall entries, for
  # editors whose CLI shim is not on PATH.
  $candidates = @(
    "$env:LOCALAPPDATA\Programs\Microsoft VS Code\bin\code.cmd",
    "$env:LOCALAPPDATA\Programs\Microsoft VS Code - Insiders\bin\code-insiders.cmd",
    "$env:LOCALAPPDATA\Programs\VSCodium\bin\codium.cmd",
    "$env:LOCALAPPDATA\Programs\cursor\bin\cursor.cmd",
    "$env:LOCALAPPDATA\Programs\Windsurf\bin\windsurf.cmd",
    "$env:LOCALAPPDATA\Programs\Trae\bin\trae.cmd"
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $editors.Add($c) } }
  try {
    $uninst = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                                 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
                                 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' `
                 -ErrorAction SilentlyContinue |
             Where-Object { $_.DisplayName -match 'Visual Studio Code|Cursor|Windsurf|VSCodium|Trae|Kiro' }
    foreach ($app in $uninst) {
      if (-not $app.InstallLocation) { continue }
      Get-ChildItem (Join-Path $app.InstallLocation 'bin') -Filter '*.cmd' -ErrorAction SilentlyContinue |
        ForEach-Object { $editors.Add($_.FullName) }
    }
  } catch { }

  $editors = $editors | Select-Object -Unique
  if ($editors.Count -eq 0) {
    Write-Step 'No VS Code / fork editor found - skipping extension install'
    Write-Warn "install it later with: code --install-extension `"$vsix`""
    exit 0
  }

  $ok = $false
  foreach ($cli in $editors) {
    & $cli --install-extension $vsix *> $null
    if ($LASTEXITCODE -eq 0) {
      Write-Step "installed A extension into $(Split-Path $cli -Leaf)"
      $ok = $true
    } else {
      Write-Warn "extension install failed for $cli"
    }
  }
  if (-not $ok) { Write-Warn 'could not install the extension automatically; retry from the editor Extensions view' }
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
exit 0
