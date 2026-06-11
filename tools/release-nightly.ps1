# Silent Hill PC nightly release script
#
# Workflow:
#   - Reads the latest version.json manifest from the nightly repo (via gh CLI).
#   - Computes today's next version: YYYY.MM.DD.N where N increments per release.
#   - Prepends the new release section to local CHANGELOG.md (so the uploaded
#     copy already contains this release's notes).
#   - Hashes the local build files (SilentHillPC.exe + maps/*.dll + CHANGELOG.md
#     + runtime DLLs in the build root (MinGW/SDL2/OpenAL/libjpeg)
#     + SilentHillPC_Launcher.exe from the launcher solution's Release output;
#     warns and skips/carries-forward if it isn't built).
#   - Diffs against the manifest. If nothing changed, exits.
#   - Creates a release on the nightly repo, uploads ONLY changed files.
#   - Generates a new manifest: changed files point to new release URLs,
#     unchanged files keep pointing to their previous release URLs.
#   - Uploads the new manifest as version.json.
#
# Usage:
#   .\tools\release-nightly.ps1 [-DryRun] [-BuildDir path] [-Notes string]
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

# Launcher (built by the user via the launcher solution; it self-updates via
# the *.old rename swap in UpdateChecker.ReplaceFile).
# Optional: warn-and-skip when missing so a game-only release still works.
$launcherExe = Join-Path $PSScriptRoot "..\pc_port\launcher\SilentHillPC_Launcher\bin\Release\SilentHillPC_Launcher.exe"

if (-not (Test-Path $exe))  { throw "SilentHillPC.exe not found at $exe. Run cmake --build first." }
if (-not (Test-Path $maps)) { throw "maps/ folder not found at $maps." }

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower()
}

# ---- Fetch existing manifest (if any release exists) ------------------------

$prevManifest   = $null
$prevReleaseTag = $null
try {
    $latest = gh release view --repo $Repo --json tagName,assets | ConvertFrom-Json
    if ($latest) {
        $prevReleaseTag = $latest.tagName
        $manifestAsset = $latest.assets | Where-Object { $_.name -eq "version.json" }
        if ($manifestAsset) {
            $tmpManifest = New-TemporaryFile
            gh release download $prevReleaseTag --repo $Repo --pattern "version.json" --output $tmpManifest --clobber | Out-Null
            $prevManifest = Get-Content $tmpManifest -Raw | ConvertFrom-Json
            Remove-Item $tmpManifest -Force
            $prevFileCount = if ($prevManifest -and $prevManifest.files) { $prevManifest.files.Count } else { 0 }
            Write-Host "Previous release: $prevReleaseTag ($prevFileCount files in manifest)" -ForegroundColor Cyan
        }
    }
} catch {
    Write-Host "No previous release found (first release for this repo)." -ForegroundColor Yellow
}

# ---- Compute changelog: commits since the previous release's git_commit -----

$curCommitFull  = (git rev-parse HEAD).Trim()
$curCommitShort = (git rev-parse --short HEAD).Trim()

$commitLog = @()
if ($prevManifest -and $prevManifest.PSObject.Properties.Name -contains "git_commit" -and $prevManifest.git_commit) {
    $commitLog = (git log "$($prevManifest.git_commit)..HEAD" --pretty=format:"- %s" --reverse)
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Warning: couldn't compute commit log since $($prevManifest.git_commit) -- leaving section empty." -ForegroundColor Yellow
        $commitLog = @()
    } elseif (-not $commitLog) {
        $commitLog = @()
    } elseif ($commitLog -is [string]) {
        $commitLog = @($commitLog)
    }
    # Strip meta-commits that are just changelog bookkeeping (e.g. "changelog: v2026.05.18.2").
    $commitLog = @($commitLog | Where-Object { $_ -notmatch '^- changelog:' })
}

# ---- Compute next version (YYYY.MM.DD.N) ------------------------------------

$today   = (Get-Date).ToString("yyyy.MM.dd")
$counter = 1

if ($prevReleaseTag) {
    $prev = $prevReleaseTag.TrimStart('v')
    if ($prev -match "^$([regex]::Escape($today))\.(\d+)$") {
        $counter = [int]$matches[1] + 1
    }
}

$newVersion = "$today.$counter"
$newTag     = "v$newVersion"

Write-Host ""
Write-Host "New version: $newVersion  tag: $newTag" -ForegroundColor Magenta

# ---- Prepend release section to local CHANGELOG.md --------------------------
# Done BEFORE hashing so the uploaded CHANGELOG.md already contains this
# release's notes. Previously this ran after the upload, so the downloaded
# copy was always one release behind.
$changelogPath = Join-Path $PSScriptRoot "..\pc_port\CHANGELOG.md"
if (Test-Path $changelogPath) {
    $existingBytes = [System.IO.File]::ReadAllBytes($changelogPath)
    # Strip UTF-8 BOM if present (PS5.1 Set-Content adds one)
    $bomStart = 0
    if ($existingBytes.Length -ge 3 -and $existingBytes[0] -eq 0xEF -and $existingBytes[1] -eq 0xBB -and $existingBytes[2] -eq 0xBF) {
        $bomStart = 3
    }
    $existing = [System.Text.Encoding]::UTF8.GetString($existingBytes, $bomStart, $existingBytes.Length - $bomStart)

    # Build the new entry: just date + commit messages.
    $section = [System.Text.StringBuilder]::new()
    [void]$section.AppendLine("## $newTag -- $((Get-Date).ToString('yyyy-MM-dd'))")
    if ($commitLog.Count -gt 0) {
        $commitLog | ForEach-Object { [void]$section.AppendLine($_) }
    } else {
        [void]$section.AppendLine("- (no commits since last release)")
    }
    [void]$section.AppendLine()

    # Find end of first heading line ("# ...") and inject the new entry after it.
    $lines = $existing -split "`n"
    $headingIdx = -1
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match '^# ') { $headingIdx = $i; break }
    }
    if ($headingIdx -ge 0) {
        $afterHeading = ($lines[($headingIdx + 1)..($lines.Length - 1)] -join "`n").TrimStart("`n")
        $newContent = $lines[$headingIdx] + "`n`n" + $section.ToString() + $afterHeading
    } else {
        $newContent = $section.ToString() + $existing
    }

    [System.IO.File]::WriteAllText($changelogPath, $newContent)
    Write-Host "CHANGELOG.md updated locally." -ForegroundColor Cyan
} else {
    Write-Host "Warning: CHANGELOG.md not found at $changelogPath - skipping." -ForegroundColor Yellow
}

# ---- Hash local files -------------------------------------------------------
# CHANGELOG.md is hashed after being updated above, so the manifest hash
# matches the file that will actually be uploaded.

$localFiles = @{}
$localFiles["SilentHillPC.exe"] = Get-Sha256 $exe

Get-ChildItem $maps -Filter "*.dll" | ForEach-Object {
    $rel = "maps/" + $_.Name
    $localFiles[$rel] = Get-Sha256 $_.FullName
}

# Runtime DLLs next to the exe (MinGW runtime, SDL2, OpenAL, libjpeg). They
# almost never change: the hash diff uploads them once, and later manifests
# keep pointing at the release that shipped them — users who lack them get
# them on their next update, everyone else skips them.
Get-ChildItem $BuildDir -Filter "*.dll" | ForEach-Object {
    $localFiles[$_.Name] = Get-Sha256 $_.FullName
}

if (Test-Path $changelogPath) {
    $localFiles["CHANGELOG.md"] = Get-Sha256 $changelogPath
}

if (Test-Path $launcherExe) {
    $localFiles["SilentHillPC_Launcher.exe"] = Get-Sha256 $launcherExe
} else {
    Write-Host "Warning: launcher exe not found at $launcherExe - not included in this release." -ForegroundColor Yellow
}

Write-Host "Local files: $($localFiles.Count) entries hashed" -ForegroundColor Cyan

# ---- Diff local vs remote ---------------------------------------------------

$prevHashes = @{}
$prevUrls   = @{}
if ($prevManifest) {
    foreach ($f in $prevManifest.files) {
        $prevHashes[$f.path] = $f.sha256
        $prevUrls[$f.path]   = $f.url
    }
}

# If the launcher exe isn't built locally but exists in the previous
# manifest, carry the old entry forward instead of dropping it (a drop would
# silently stop hash-checking it for every user). SH1Updater.exe is retired —
# deliberately NOT carried forward so it falls out of the manifest.
foreach ($opt in @("SilentHillPC_Launcher.exe")) {
    if (-not $localFiles.ContainsKey($opt) -and $prevHashes.ContainsKey($opt)) {
        $localFiles[$opt] = $prevHashes[$opt]
        Write-Host "Carrying forward previous $opt manifest entry." -ForegroundColor Yellow
    }
}

$changed = @()
foreach ($path in $localFiles.Keys) {
    if (-not $prevHashes.ContainsKey($path) -or $prevHashes[$path] -ne $localFiles[$path]) {
        $changed += $path
    }
}

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

if ($DryRun) {
    Write-Host "[DryRun] Skipping release creation. Would have uploaded:" -ForegroundColor Yellow
    $changed | ForEach-Object { Write-Host "  $_" }
    Write-Host "Commit the updated CHANGELOG.md when ready:" -ForegroundColor Cyan
    Write-Host "  git add pc_port/CHANGELOG.md && git commit -m 'changelog: $newTag'" -ForegroundColor Gray
    exit 0
}

# ---- Create release + upload changed files ----------------------------------

if (-not $Notes) {
    $sb = [System.Text.StringBuilder]::new()
    if ($commitLog.Count -gt 0) {
        $commitLog | ForEach-Object { [void]$sb.AppendLine($_) }
    } else {
        [void]$sb.AppendLine("(no commits since last release)")
    }
    $Notes = $sb.ToString()
}

# Stage changed files in a temp dir with sanitized names for upload.
# Asset names can't contain '/', so map "maps/foo.dll" -> "maps__foo.dll".
$stagingDir = Join-Path ([IO.Path]::GetTempPath()) "sh-nightly-$newVersion"
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

$uploadAssets = @()
foreach ($path in $changed) {
    $src = if ($path -eq "SilentHillPC.exe") { $exe }
           elseif ($path -eq "config.cfg") { $cfg }
           elseif ($path -eq "CHANGELOG.md") { $changelogPath }
           elseif ($path -eq "SilentHillPC_Launcher.exe") { $launcherExe }
           else { Join-Path $BuildDir $path }
    $assetName = $path -replace '/', '__'
    $dst = Join-Path $stagingDir $assetName
    Copy-Item $src $dst -Force
    $uploadAssets += $dst
}

# Write notes to a file and use --notes-file.
# --notes "$Notes" breaks in PS5.1 when the string contains embedded double
# quotes (e.g. commit subjects like `show "You're up to date!" dialog`) —
# PS5.1 splits on the quotes and gh receives stray tokens as file-glob args,
# causing "no matches found for `up`" and a silent empty release.
$notesFile = Join-Path $stagingDir "release_notes.txt"
[System.IO.File]::WriteAllText($notesFile, $Notes)

# Create the release first (without manifest -- we'll upload manifest after we
# know the asset URLs).
Write-Host "Creating release $newTag..." -ForegroundColor Cyan
gh release create $newTag `
    --repo $Repo `
    --title "Nightly $newVersion" `
    --notes-file $notesFile `
    @uploadAssets
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $stagingDir
    throw "gh release create failed (exit $LASTEXITCODE). Release was NOT published."
}

# ---- Build new manifest -----------------------------------------------------

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
        $entry["url"] = $prevUrls[$path]
    }
    $newFiles += $entry
}

$manifest = [ordered]@{
    version    = $newVersion
    tag        = $newTag
    build_date = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    git_commit = $curCommitFull
    files      = $newFiles
}

$manifestPath = Join-Path $stagingDir "version.json"
# Write without BOM -- PS5.1's Set-Content -Encoding UTF8 adds a BOM that
# DataContractJsonSerializer can't handle, producing "unexpected character 'T'".
$jsonContent = $manifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($manifestPath, $jsonContent)

Write-Host "Uploading version.json..." -ForegroundColor Cyan
gh release upload $newTag --repo $Repo $manifestPath --clobber
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $stagingDir
    throw "gh release upload version.json failed (exit $LASTEXITCODE)."
}

# Cleanup
Remove-Item -Recurse -Force $stagingDir

Write-Host ""
Write-Host "Done. Release published: https://github.com/$Repo/releases/tag/$newTag" -ForegroundColor Green
Write-Host "Commit the updated CHANGELOG.md when ready:" -ForegroundColor Cyan
Write-Host "  git add pc_port/CHANGELOG.md && git commit -m 'changelog: $newTag'" -ForegroundColor Gray
