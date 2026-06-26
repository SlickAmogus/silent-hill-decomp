# Silent Hill PC nightly release script
#
# Two release modes (prompted if -Mode is not given):
#
#   loose  - the original per-file flow. Hashes the local build files, diffs
#            them against the previous loose release's version.json, creates a
#            release on the default branch, and uploads ONLY the changed files +
#            a new version.json. Used for the public/stable stream and for the
#            final loose build before everyone is on the zip-capable launcher.
#
#   zip    - bundles the whole launchable file set (everything in the reference
#            SHPC_Alpha8.zip EXCEPT the disc image and the save .MCD files) from
#            the build folder into one timestamped .zip, and publishes it as a
#            release on the dedicated `beta` branch (auto-created on first use)
#            together with a small version.json (per-file hashes + the zip URL)
#            so the launcher can do a cheap update check and only pull the zip
#            when it actually needs to apply. No pre-release hash diff is done in
#            zip mode -- the maintainer is the only publisher, so every run
#            produces a release.
#
# Both modes prepend an auto-generated section to CHANGELOG.md (from commit
# subjects since the previous release of the SAME stream) and PAUSE so it can be
# hand-edited before anything is hashed/zipped/uploaded.
#
# Usage:
#   .\tools\release-nightly.ps1 [-Mode zip|loose] [-DryRun] [-BuildDir path]
#                               [-Notes string] [-NoPause] [-BetaBranch beta]
#                               [-SkipCrossPlatform]
#
# Linux + macOS builds are included BY DEFAULT: the newest successful Linux +
# macOS CI artifacts (build-linux.yml / build-macos.yml on the source repo) are
# attached to the release as standalone zips. They are deliberately kept OUT of
# version.json so the Windows launcher ignores them (cannot affect the launcher).
# Pass -SkipCrossPlatform for a Windows-only release. If the CI build for the
# release's commit isn't ready yet, the newest available build is attached with
# a warning.
#
# Requires:
#   - gh CLI installed and authenticated (gh auth login).
#   - Build artifacts present in pc_port/build/ (run cmake --build first).
#   - Repo SlickAmogus/silent-hill-pc-nightly already created on GitHub.

[CmdletBinding()]
param(
    [string]$BuildDir   = "$PSScriptRoot\..\pc_port\build",
    [string]$Repo       = "SlickAmogus/silent-hill-pc-nightly",
    [ValidateSet("", "zip", "loose")]
    [string]$Mode       = "",
    [string]$BetaBranch    = "beta",
    [string]$SourceRepoUrl = "https://github.com/SlickAmogus/silent-hill-decomp",
    [string]$Notes         = "",
    [switch]$DryRun,
    [switch]$NoPause,
    # Cross-platform builds are attached by DEFAULT: the newest CI-built Linux +
    # macOS artifacts from the source repo are pulled and attached to the release
    # as standalone assets. They are NOT added to version.json's files[] -- the
    # Windows launcher can't run them and would try to "update" them, so they
    # cannot affect the launcher. Linux/macOS users download these zips by hand.
    # Pass -SkipCrossPlatform to publish a Windows-only release.
    [switch]$SkipCrossPlatform,
    [string]$CrossPlatformBranch = "pc-port"
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
$launcherExe = Join-Path $PSScriptRoot "..\pc_port\launcher\SilentHillPC_Launcher\bin\Release\SilentHillPC_Launcher.exe"

if (-not (Test-Path $exe))  { throw "SilentHillPC.exe not found at $exe. Run cmake --build first." }
if (-not (Test-Path $maps)) { throw "maps/ folder not found at $maps." }

$changelogPath = Join-Path $PSScriptRoot "..\pc_port\CHANGELOG.md"

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower()
}

# Download the newest successful CI build artifacts (Linux + macOS) from the
# source repo and attach them to the just-created release on the nightly repo.
# These are extra downloads for non-Windows users; they never enter version.json.
function Add-CrossPlatformAssets {
    param([string]$Tag, [string]$SourceCommit)
    if ($SkipCrossPlatform) { return }

    $sourceSlug = $SourceRepoUrl -replace '^https?://github\.com/', '' -replace '/$', ''
    Write-Host ""
    Write-Host "Attaching cross-platform CI artifacts from $sourceSlug (branch $CrossPlatformBranch)..." -ForegroundColor Cyan

    $dlRoot = Join-Path ([IO.Path]::GetTempPath()) "shpc-xplat-$Tag"
    if (Test-Path $dlRoot) { Remove-Item -Recurse -Force $dlRoot }
    New-Item -ItemType Directory -Force -Path $dlRoot | Out-Null

    $targets = @(
        @{ Workflow = "build-linux.yml"; Artifact = "SHPC-linux-x64" },
        @{ Workflow = "build-macos.yml"; Artifact = "SHPC-macos-arm64" }
    )

    $assets = @()
    foreach ($t in $targets) {
        $run = @((gh run list --repo $sourceSlug --workflow $t.Workflow `
                    --branch $CrossPlatformBranch --status success `
                    --limit 1 --json databaseId,headSha 2>$null) | ConvertFrom-Json)
        if ($LASTEXITCODE -ne 0 -or -not $run -or $run.Count -eq 0) {
            Write-Host "  WARN: no successful '$($t.Workflow)' run on '$CrossPlatformBranch' -- skipping $($t.Artifact)." -ForegroundColor Yellow
            continue
        }
        $runId  = "$($run[0].databaseId)".Trim()
        $runSha = "$($run[0].headSha)".Trim()
        # Flag (but don't block) a build from a different commit than this
        # release -- usually means CI hasn't finished building the latest push.
        if ($SourceCommit -and $runSha -and $runSha -ne $SourceCommit) {
            Write-Host "  WARN: newest $($t.Workflow) build is commit $($runSha.Substring(0,9)), but this release is $($SourceCommit.Substring(0,9)) -- CI may still be building the latest push. Attaching it anyway; re-run the release once CI finishes for a matching build." -ForegroundColor Yellow
        }
        $outDir = Join-Path $dlRoot $t.Artifact
        gh run download $runId --repo $sourceSlug --name $t.Artifact --dir $outDir 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  WARN: failed to download $($t.Artifact) (run $runId) -- skipping." -ForegroundColor Yellow
            continue
        }
        Get-ChildItem -Recurse -File $outDir | ForEach-Object {
            $assets += $_.FullName
            Write-Host "  + $($_.Name) (run $runId)" -ForegroundColor Gray
        }
    }

    if ($assets.Count -eq 0) {
        Write-Host "  No cross-platform artifacts attached." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $dlRoot -ErrorAction SilentlyContinue
        return
    }

    gh release upload $Tag --repo $Repo @assets --clobber
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  WARN: gh release upload of cross-platform assets failed (exit $LASTEXITCODE)." -ForegroundColor Yellow
    } else {
        Write-Host "  Attached $($assets.Count) cross-platform asset(s) to $Tag." -ForegroundColor Green
    }
    Remove-Item -Recurse -Force $dlRoot -ErrorAction SilentlyContinue
}

# Launcher FileVersion (AssemblyFileVersion, date-based yyyy.M.d.rev). Carried in
# version.json so the launcher can decide self-updates WITHOUT downloading: it
# only replaces itself when this is strictly greater than its own version.
function Get-LauncherVersion([string]$path) {
    try { return ([System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)).FileVersion }
    catch { return $null }
}
$launcherVersion = if (Test-Path $launcherExe) { Get-LauncherVersion $launcherExe } else { $null }

# ---- Resolve release mode ---------------------------------------------------

if (-not $Mode) {
    Write-Host ""
    Write-Host "Release mode:" -ForegroundColor Magenta
    Write-Host "  [1] loose  - per-file release on the default branch (current/stable)" -ForegroundColor Gray
    Write-Host "  [2] zip    - bundled zip release on the '$BetaBranch' branch (beta)" -ForegroundColor Gray
    $ans  = Read-Host "Choose 1 or 2"
    $Mode = if ($ans.Trim() -eq '2') { 'zip' } else { 'loose' }
}
$isZip     = ($Mode -eq 'zip')
$tagPrefix = if ($isZip) { 'beta-' } else { 'v' }
Write-Host "Mode: $Mode" -ForegroundColor Magenta

# ---- Find the previous release of THIS stream -------------------------------
# Loose and zip releases are interleaved in time; key the version counter and
# the changelog commit range off the previous release of the SAME stream
# (loose = v*, zip = beta-*), not whatever is globally newest.

function Get-LatestStreamReleaseTag {
    param([bool]$Zip)
    $json = gh release list --repo $Repo --limit 100 --json tagName,createdAt 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $json) { return $null }
    $rels = $json | ConvertFrom-Json
    if (-not $rels) { return $null }
    $prefix = if ($Zip) { 'beta-' } else { 'v' }
    $stream = if ($Zip) { $rels | Where-Object { $_.tagName -like 'beta-*' } }
              else       { $rels | Where-Object { $_.tagName -notlike 'beta-*' } }
    if (-not $stream) { return $null }
    # Sort by the PARSED version (YYYY.MM.DD.N), NOT createdAt: the nightly repo's
    # release timestamps are all identical (a bulk re-upload reset them), so a
    # createdAt sort returns an arbitrary release -> wrong version counter + a giant
    # changelog spanning dozens of already-released commits.
    $sorted = $stream | Sort-Object -Property @{ Expression = {
        $v = $_.tagName
        if ($v.StartsWith($prefix)) { $v = $v.Substring($prefix.Length) }
        $ver = $null
        if ([version]::TryParse($v, [ref]$ver)) { $ver } else { [version]'0.0.0.0' }
    } } -Descending
    return ($sorted | Select-Object -First 1).tagName
}

$prevManifest   = $null
$prevReleaseTag = Get-LatestStreamReleaseTag -Zip $isZip
if ($prevReleaseTag) {
    try {
        $tmpManifest = New-TemporaryFile
        gh release download $prevReleaseTag --repo $Repo --pattern "version.json" --output $tmpManifest --clobber 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $prevManifest = Get-Content $tmpManifest -Raw | ConvertFrom-Json
            $prevFileCount = if ($prevManifest -and $prevManifest.PSObject.Properties.Name -contains 'files' -and $prevManifest.files) { $prevManifest.files.Count } else { 0 }
            Write-Host "Previous $Mode release: $prevReleaseTag ($prevFileCount files in manifest)" -ForegroundColor Cyan
        }
        Remove-Item $tmpManifest -Force -ErrorAction SilentlyContinue
    } catch {
        Write-Host "Previous $Mode release $prevReleaseTag has no readable version.json." -ForegroundColor Yellow
    }
} else {
    Write-Host "No previous $Mode release found (first $Mode release for this repo)." -ForegroundColor Yellow
}

# ---- Compute changelog: commits since the previous release's git_commit -----

$curCommitFull  = (git rev-parse HEAD).Trim()
$curCommitShort = (git rev-parse --short HEAD).Trim()

# Footer appended to every release body so a nightly build links straight back to
# the exact source commit it was built from (the manifest also carries git_commit).
$sourceFooter = "`r`n`r`n---`r`nBuilt from source commit $curCommitShort`r`nSource: $SourceRepoUrl/commit/$curCommitFull"

# Changelog baseline: the same-stream previous release if any, else the globally
# newest release of the OTHER stream -- so the FIRST release of a stream still
# lists commits since the last release of any kind instead of "(no commits)".
$baseManifest = $prevManifest
if (-not $baseManifest) {
    $otherTag = Get-LatestStreamReleaseTag -Zip (-not $isZip)
    if ($otherTag) {
        try {
            $tmpBase = New-TemporaryFile
            gh release download $otherTag --repo $Repo --pattern "version.json" --output $tmpBase --clobber 2>$null | Out-Null
            if ($LASTEXITCODE -eq 0) {
                $baseManifest = Get-Content $tmpBase -Raw | ConvertFrom-Json
                Write-Host "First $Mode release: changelog baseline = $otherTag (latest release of the other stream)." -ForegroundColor Cyan
            }
            Remove-Item $tmpBase -Force -ErrorAction SilentlyContinue
        } catch {}
    }
}

$commitLog = @()
if ($baseManifest -and $baseManifest.PSObject.Properties.Name -contains "git_commit" -and $baseManifest.git_commit) {
    $prevOutEnc = $null
    try { $prevOutEnc = [Console]::OutputEncoding; [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
    try {
        $commitLog = (git log "$($baseManifest.git_commit)..HEAD" --pretty=format:"- %s" --reverse)
    } finally {
        if ($null -ne $prevOutEnc) { try { [Console]::OutputEncoding = $prevOutEnc } catch {} }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Warning: couldn't compute commit log since $($baseManifest.git_commit) -- leaving section empty." -ForegroundColor Yellow
        $commitLog = @()
    } elseif (-not $commitLog) {
        $commitLog = @()
    } elseif ($commitLog -is [string]) {
        $commitLog = @($commitLog)
    }
    $commitLog = @($commitLog | Where-Object { $_ -notmatch '^- changelog:' })
}

# ---- Compute next version (YYYY.MM.DD.N) ------------------------------------

$today   = (Get-Date).ToString("yyyy.MM.dd")
$counter = 1

if ($prevReleaseTag) {
    $prev = $prevReleaseTag
    if ($prev.StartsWith($tagPrefix)) { $prev = $prev.Substring($tagPrefix.Length) }
    if ($prev -match "^$([regex]::Escape($today))\.(\d+)$") {
        $counter = [int]$matches[1] + 1
    }
}

$newVersion = "$today.$counter"
$newTag     = "$tagPrefix$newVersion"

Write-Host ""
Write-Host "New version: $newVersion  tag: $newTag  branch: $(if ($isZip) { $BetaBranch } else { 'default' })" -ForegroundColor Magenta

# ---- Prepend release section to local CHANGELOG.md --------------------------

if (Test-Path $changelogPath) {
    $existingBytes = [System.IO.File]::ReadAllBytes($changelogPath)
    $bomStart = 0
    if ($existingBytes.Length -ge 3 -and $existingBytes[0] -eq 0xEF -and $existingBytes[1] -eq 0xBB -and $existingBytes[2] -eq 0xBF) {
        $bomStart = 3
    }
    $existing = [System.Text.Encoding]::UTF8.GetString($existingBytes, $bomStart, $existingBytes.Length - $bomStart)

    $section = [System.Text.StringBuilder]::new()
    [void]$section.AppendLine("## $newTag -- $((Get-Date).ToString('yyyy-MM-dd'))")
    if ($commitLog.Count -gt 0) {
        $commitLog | ForEach-Object { [void]$section.AppendLine($_) }
    } else {
        [void]$section.AppendLine("- (no commits since last release)")
    }
    [void]$section.AppendLine()

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

# ---- Pause for manual changelog edit ----------------------------------------

if (-not $NoPause) {
    Write-Host ""
    Write-Host "==> CHANGELOG.md updated with auto-generated notes for $newTag." -ForegroundColor Yellow
    Write-Host "    Edit and SAVE it now if you want (e.g. add command usage)." -ForegroundColor Yellow
    Write-Host "    File: $changelogPath" -ForegroundColor Yellow
    [void](Read-Host "    Press Enter to hash + publish the release (Ctrl+C to abort)")
}

# =============================================================================
# ZIP MODE - bundle the launchable file set + publish to the beta branch
# =============================================================================

if ($isZip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    # Reference set = pc_port/build/SHPC_Alpha8.zip MINUS the disc image (never
    # present here) and the save .MCD files (omitted -- the game creates the
    # save folder fresh; we never publish anyone's playthrough). config.cfg is
    # the clean template from source so a fresh install starts on the stable
    # stream, not whatever the dev's local config points at.
    $cfgTemplate = Join-Path $PSScriptRoot "..\pc_port\config.cfg"

    if (-not (Test-Path $launcherExe)) {
        throw "Launcher exe not found at $launcherExe. Build the launcher (Release) before a zip release."
    }

    $stage = Join-Path ([IO.Path]::GetTempPath()) "sh-zip-$newVersion"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    try {
        # Top-level files.
        Copy-Item $exe         (Join-Path $stage "SilentHillPC.exe") -Force
        Copy-Item $launcherExe (Join-Path $stage "SilentHillPC_Launcher.exe") -Force
        if (Test-Path $changelogPath) { Copy-Item $changelogPath (Join-Path $stage "CHANGELOG.md") -Force }
        if (Test-Path $cfgTemplate)   { Copy-Item $cfgTemplate   (Join-Path $stage "config.cfg")   -Force }

        # Runtime DLLs next to the exe (MinGW runtime, SDL2, OpenAL, libjpeg).
        Get-ChildItem $BuildDir -Filter "*.dll" | ForEach-Object {
            Copy-Item $_.FullName (Join-Path $stage $_.Name) -Force
        }

        # Map overlay DLLs.
        $stageMaps = Join-Path $stage "maps"
        New-Item -ItemType Directory -Force -Path $stageMaps | Out-Null
        Get-ChildItem $maps -Filter "*.dll" | ForEach-Object {
            Copy-Item $_.FullName (Join-Path $stageMaps $_.Name) -Force
        }

        # Empty save folder so the game has somewhere to write (no cards shipped).
        New-Item -ItemType Directory -Force -Path (Join-Path $stage "gamedata\save") | Out-Null

        # Hash every staged file for the manifest (relative POSIX paths).
        $stageFull = (Resolve-Path $stage).Path
        $files = @()
        Get-ChildItem -Recurse -File $stage | ForEach-Object {
            $rel = $_.FullName.Substring($stageFull.Length).TrimStart('\','/').Replace('\','/')
            $files += [ordered]@{ path = $rel; sha256 = (Get-Sha256 $_.FullName) }
        }
        Write-Host "Staged $($files.Count) files for the zip." -ForegroundColor Cyan

        # Build the zip (CreateFromDirectory keeps the empty gamedata/save dir).
        $zipName = "SHPC_$newTag.zip"
        $zipPath = Join-Path ([IO.Path]::GetTempPath()) $zipName
        if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
        [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath)
        $zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
        Write-Host "Built $zipName ($zipSize MB)." -ForegroundColor Cyan

        # Manifest: per-file hashes for the launcher's cheap check + the zip URL
        # it pulls when it actually applies an update.
        $baseUrl  = "https://github.com/$Repo/releases/download/$newTag"
        $manifest = [ordered]@{
            version    = $newVersion
            tag        = $newTag
            mode       = "zip"
            branch     = $BetaBranch
            build_date = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
            git_commit = $curCommitFull
            launcher_version = $launcherVersion
            zip_name   = $zipName
            zip_url    = "$baseUrl/$zipName"
            files      = $files
        }
        $manifestPath = Join-Path ([IO.Path]::GetTempPath()) "version.json"
        [System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6))

        # Release notes from the (hand-edited) CHANGELOG.md section for $newTag.
        if (-not $Notes -and (Test-Path $changelogPath)) {
            $clText  = [System.IO.File]::ReadAllText($changelogPath)
            $pattern = "(?ms)^##\s+" + [regex]::Escape($newTag) + ".*?(?=^##\s|\z)"
            $m       = [regex]::Match($clText, $pattern)
            if ($m.Success) {
                $parts = $m.Value -split "`n", 2
                if ($parts.Count -eq 2) { $Notes = $parts[1].Trim() }
            }
        }
        if (-not $Notes) { $Notes = "Beta zip release $newVersion" }
        $Notes = $Notes + $sourceFooter
        $notesFile = Join-Path ([IO.Path]::GetTempPath()) "release_notes_$newVersion.txt"
        [System.IO.File]::WriteAllText($notesFile, $Notes)

        if ($DryRun) {
            Write-Host "[DryRun] Would publish $newTag to branch '$BetaBranch' with:" -ForegroundColor Yellow
            Write-Host "  $zipName ($zipSize MB) + version.json ($($files.Count) files)" -ForegroundColor Gray
            Write-Host "  Zip kept at: $zipPath" -ForegroundColor Gray
            Remove-Item $manifestPath, $notesFile -Force -ErrorAction SilentlyContinue
            exit 0
        }

        # Ensure the beta branch exists (releases target it). Create it from the
        # repo's default branch head on first use.
        $branchOk = $true
        gh api "repos/$Repo/branches/$BetaBranch" 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { $branchOk = $false }
        if (-not $branchOk) {
            Write-Host "Creating '$BetaBranch' branch on $Repo..." -ForegroundColor Cyan
            $defBranch = (gh api "repos/$Repo" --jq ".default_branch").Trim()
            $defSha    = (gh api "repos/$Repo/git/refs/heads/$defBranch" --jq ".object.sha").Trim()
            if (-not $defSha) { throw "Couldn't resolve $Repo default branch head to seed '$BetaBranch'." }
            gh api "repos/$Repo/git/refs" -f "ref=refs/heads/$BetaBranch" -f "sha=$defSha" | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Failed to create '$BetaBranch' branch." }
        }

        Write-Host "Creating release $newTag on branch '$BetaBranch'..." -ForegroundColor Cyan
        # CHANGELOG.md goes up as its OWN asset (next to version.json) so the
        # launcher can preview it without downloading the whole zip. Betas are
        # REGULAR releases (not prereleases) so they appear on the repo's Releases
        # page; the launcher distinguishes alpha from beta by tag (v* vs beta-*)
        # and picks the newest build across both streams by parsed version.
        $betaAssets = @($zipPath, $manifestPath)
        if (Test-Path $changelogPath) { $betaAssets += $changelogPath }
        gh release create $newTag `
            --repo $Repo `
            --target $BetaBranch `
            --title "Beta $newVersion" `
            --notes-file $notesFile `
            @betaAssets
        if ($LASTEXITCODE -ne 0) { throw "gh release create failed (exit $LASTEXITCODE). Release was NOT published." }

        Add-CrossPlatformAssets -Tag $newTag -SourceCommit $curCommitFull

        Remove-Item $zipPath, $manifestPath, $notesFile -Force -ErrorAction SilentlyContinue
        Write-Host ""
        Write-Host "Done. Beta zip release published: https://github.com/$Repo/releases/tag/$newTag" -ForegroundColor Green
        Write-Host "Commit the updated CHANGELOG.md when ready:" -ForegroundColor Cyan
        Write-Host "  git add pc_port/CHANGELOG.md && git commit -m 'changelog: $newTag'" -ForegroundColor Gray
    }
    finally {
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
    }
    exit 0
}

# =============================================================================
# LOOSE MODE - per-file hash diff + upload (original behavior)
# =============================================================================

# ---- Hash local files -------------------------------------------------------

$localFiles = @{}
$localFiles["SilentHillPC.exe"] = Get-Sha256 $exe

Get-ChildItem $maps -Filter "*.dll" | ForEach-Object {
    $rel = "maps/" + $_.Name
    $localFiles[$rel] = Get-Sha256 $_.FullName
}

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
if ($prevManifest -and $prevManifest.PSObject.Properties.Name -contains 'files' -and $prevManifest.files) {
    foreach ($f in $prevManifest.files) {
        $prevHashes[$f.path] = $f.sha256
        $prevUrls[$f.path]   = $f.url
    }
}

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
    if (Test-Path $changelogPath) {
        $clText  = [System.IO.File]::ReadAllText($changelogPath)
        $pattern = "(?ms)^##\s+" + [regex]::Escape($newTag) + ".*?(?=^##\s|\z)"
        $m       = [regex]::Match($clText, $pattern)
        if ($m.Success) {
            $parts = $m.Value -split "`n", 2
            if ($parts.Count -eq 2) { $Notes = $parts[1].Trim() }
        }
    }
    if (-not $Notes) {
        $sb = [System.Text.StringBuilder]::new()
        if ($commitLog.Count -gt 0) {
            $commitLog | ForEach-Object { [void]$sb.AppendLine($_) }
        } else {
            [void]$sb.AppendLine("(no commits since last release)")
        }
        $Notes = $sb.ToString()
    }
}

$Notes = $Notes + $sourceFooter

$stagingDir = Join-Path ([IO.Path]::GetTempPath()) "sh-nightly-$newVersion"
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

$uploadAssets = @()
foreach ($path in $changed) {
    $src = if ($path -eq "SilentHillPC.exe") { $exe }
           elseif ($path -eq "CHANGELOG.md") { $changelogPath }
           elseif ($path -eq "SilentHillPC_Launcher.exe") { $launcherExe }
           else { Join-Path $BuildDir $path }
    $assetName = $path -replace '/', '__'
    $dst = Join-Path $stagingDir $assetName
    Copy-Item $src $dst -Force
    $uploadAssets += $dst
}

$notesFile = Join-Path $stagingDir "release_notes.txt"
[System.IO.File]::WriteAllText($notesFile, $Notes)

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
    mode       = "loose"
    build_date = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    git_commit = $curCommitFull
    launcher_version = $launcherVersion
    files      = $newFiles
}

$manifestPath = Join-Path $stagingDir "version.json"
$jsonContent = $manifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($manifestPath, $jsonContent)

Write-Host "Uploading version.json..." -ForegroundColor Cyan
gh release upload $newTag --repo $Repo $manifestPath --clobber
if ($LASTEXITCODE -ne 0) {
    Remove-Item -Recurse -Force $stagingDir
    throw "gh release upload version.json failed (exit $LASTEXITCODE)."
}

Add-CrossPlatformAssets -Tag $newTag -SourceCommit $curCommitFull

Remove-Item -Recurse -Force $stagingDir

Write-Host ""
Write-Host "Done. Release published: https://github.com/$Repo/releases/tag/$newTag" -ForegroundColor Green
Write-Host "Commit the updated CHANGELOG.md when ready:" -ForegroundColor Cyan
Write-Host "  git add pc_port/CHANGELOG.md && git commit -m 'changelog: $newTag'" -ForegroundColor Gray
