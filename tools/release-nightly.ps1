# Silent Hill PC nightly release script
#
# Workflow:
#   - Reads the latest version.json manifest from the nightly repo (via gh CLI).
#   - Hashes the local build files (SilentHillPC.exe + maps/*.dll + config.cfg).
#   - Diffs against the manifest. If nothing changed, exits.
#   - Computes today's next version: YYYY.MM.DD.N where N increments per release.
#   - Creates a pre-release on the nightly repo, uploads ONLY changed files.
#   - Generates a new manifest: changed files point to new release URLs,
#     unchanged files keep pointing to their previous release URLs.
#   - Uploads the new manifest as version.json.
#
# Usage:
#   .\tools\release-nightly.ps1 [-DryRun] [-BuildDir <path>] [-Notes <string>]
#
# Requires:
#   - gh CLI installed and authenticated (gh auth login).
#   - Build artifacts present in pc_port/build/ (run cmake --build first).
#   - Repo SlickAmogus/silent-hill-pc-nightly already created on GitHub.

[CmdletBinding()]
param(
    [string]$BuildDir = "$PSScriptRoot\..\pc_port\build",
    [string]$Repo     = "SlickAmogus/silent-hill-pc-nightly",
    [string]$Notes    = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# ---- Sanity checks ----------------------------------------------------------

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh CLI not found in PATH. Install from https://cli.github.com/"
}

$exe  = Join-Path $BuildDir "SilentHillPC.exe"
$maps = Join-Path $BuildDir "maps"
$cfg  = Join-Path $BuildDir "config.cfg"

if (-not (Test-Path $exe))  { throw "SilentHillPC.exe not found at $exe. Run cmake --build first." }
if (-not (Test-Path $maps)) { throw "maps/ folder not found at $maps." }

# ---- Hash local files -------------------------------------------------------

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower()
}

$localFiles = @{}
$localFiles["SilentHillPC.exe"] = Get-Sha256 $exe

Get-ChildItem $maps -Filter "*.dll" | ForEach-Object {
    $rel = "maps/" + $_.Name
    $localFiles[$rel] = Get-Sha256 $_.FullName
}

# config.cfg is optional in releases — only include if present
if (Test-Path $cfg) {
    $localFiles["config.cfg"] = Get-Sha256 $cfg
}

Write-Host "Local files: $($localFiles.Count) entries hashed" -ForegroundColor Cyan

# ---- Fetch existing manifest (if any release exists) ------------------------

$prevManifest = $null
$prevReleaseTag = $null
try {
    # gh release view --json returns release metadata. We want the manifest asset.
    $latest = gh release view --repo $Repo --json tagName,assets 2>$null | ConvertFrom-Json
    if ($latest) {
        $prevReleaseTag = $latest.tagName
        $manifestAsset = $latest.assets | Where-Object { $_.name -eq "version.json" }
        if ($manifestAsset) {
            $tmpManifest = New-TemporaryFile
            gh release download $prevReleaseTag --repo $Repo --pattern "version.json" --output $tmpManifest --clobber | Out-Null
            $prevManifest = Get-Content $tmpManifest -Raw | ConvertFrom-Json
            Remove-Item $tmpManifest -Force
            Write-Host "Previous release: $prevReleaseTag ($($prevManifest.files.Count) files in manifest)" -ForegroundColor Cyan
        }
    }
} catch {
    Write-Host "No previous release found (first release for this repo)." -ForegroundColor Yellow
}

# ---- Diff local vs remote ---------------------------------------------------

$prevHashes = @{}
$prevUrls   = @{}
if ($prevManifest) {
    foreach ($f in $prevManifest.files) {
        $prevHashes[$f.path] = $f.sha256
        $prevUrls[$f.path]   = $f.url
    }
}

$changed = @()
foreach ($path in $localFiles.Keys) {
    if (-not $prevHashes.ContainsKey($path) -or $prevHashes[$path] -ne $localFiles[$path]) {
        $changed += $path
    }
}

# Also flag removed files (in prev manifest but not local) — these get dropped
$removed = @()
foreach ($path in $prevHashes.Keys) {
    if (-not $localFiles.ContainsKey($path)) {
        $removed += $path
    }
}

Write-Host ""
Write-Host "Changed files ($($changed.Count)):" -ForegroundColor Green
$changed | ForEach-Object { Write-Host "  $_" }
if ($removed.Count -gt 0) {
    Write-Host "Removed files ($($removed.Count)):" -ForegroundColor Yellow
    $removed | ForEach-Object { Write-Host "  $_" }
}

if ($changed.Count -eq 0 -and $removed.Count -eq 0) {
    Write-Host "Nothing to release. Exiting." -ForegroundColor Yellow
    exit 0
}

# ---- Compute next version (YYYY.MM.DD.N) ------------------------------------

$today = (Get-Date).ToString("yyyy.MM.dd")
$counter = 1

if ($prevReleaseTag) {
    # Tag format: vYYYY.MM.DD.N — strip leading 'v', parse.
    $prev = $prevReleaseTag.TrimStart('v')
    if ($prev -match "^$([regex]::Escape($today))\.(\d+)$") {
        $counter = [int]$matches[1] + 1
    }
}

$newVersion = "$today.$counter"
$newTag     = "v$newVersion"

Write-Host ""
Write-Host "New version: $newVersion  tag: $newTag" -ForegroundColor Magenta

if ($DryRun) {
    Write-Host "[DryRun] Skipping release creation. Would have uploaded:" -ForegroundColor Yellow
    $changed | ForEach-Object { Write-Host "  $_" }
    exit 0
}

# ---- Create release + upload changed files ----------------------------------

if (-not $Notes) {
    $Notes = "Nightly build $newVersion.`n`n**Changed files:**`n"
    $changed | ForEach-Object { $Notes += "- ``$_```n" }
    if ($removed.Count -gt 0) {
        $Notes += "`n**Removed:**`n"
        $removed | ForEach-Object { $Notes += "- ``$_```n" }
    }
}

# Stage changed files in a temp dir with sanitized names for upload.
# Asset names can't contain '/', so map "maps/foo.dll" -> "maps__foo.dll".
$stagingDir = Join-Path ([IO.Path]::GetTempPath()) "sh-nightly-$newVersion"
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

$uploadAssets = @()
foreach ($path in $changed) {
    $src = if ($path -eq "SilentHillPC.exe") { $exe }
           elseif ($path -eq "config.cfg") { $cfg }
           else { Join-Path $BuildDir $path }
    $assetName = $path -replace '/', '__'
    $dst = Join-Path $stagingDir $assetName
    Copy-Item $src $dst -Force
    $uploadAssets += $dst
}

# Create the release first (without manifest — we'll upload manifest after we
# know the asset URLs).
Write-Host "Creating release $newTag..." -ForegroundColor Cyan
gh release create $newTag `
    --repo $Repo `
    --title "Nightly $newVersion" `
    --notes $Notes `
    --prerelease `
    @uploadAssets

# ---- Build new manifest -----------------------------------------------------

# Asset URLs follow the pattern:
#   https://github.com/<owner>/<repo>/releases/download/<tag>/<assetName>
$baseUrl = "https://github.com/$Repo/releases/download/$newTag"

$newFiles = @()
foreach ($path in $localFiles.Keys | Sort-Object) {
    $entry = [ordered]@{
        path   = $path
        sha256 = $localFiles[$path]
    }
    if ($path -in $changed) {
        $assetName = $path -replace '/', '__'
        $entry["url"] = "$baseUrl/$assetName"
    } else {
        # Unchanged: reuse the URL from the previous manifest.
        $entry["url"] = $prevUrls[$path]
    }
    $newFiles += $entry
}

$manifest = [ordered]@{
    version    = $newVersion
    tag        = $newTag
    build_date = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    files      = $newFiles
}

$manifestPath = Join-Path $stagingDir "version.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content $manifestPath -Encoding UTF8

Write-Host "Uploading version.json..." -ForegroundColor Cyan
gh release upload $newTag --repo $Repo $manifestPath --clobber

# Cleanup
Remove-Item -Recurse -Force $stagingDir

Write-Host ""
Write-Host "Done. Release published: https://github.com/$Repo/releases/tag/$newTag" -ForegroundColor Green
