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
# Linux + macOS builds are included BY DEFAULT and are VERIFIED BEFORE anything
# is published: the release is NOT created until the build-linux.yml /
# build-macos.yml CI runs for THIS EXACT commit have finished successfully on the
# source repo. Their artifacts are attached as standalone zips, kept OUT of
# version.json so the Windows launcher ignores them. Pass -SkipCrossPlatform for
# a deliberate Windows-only release.
#
# Cross-platform gate (runs BEFORE the release is created -- so a not-ready build
# aborts the WHOLE release instead of leaving a half-published one):
#   - all matching builds succeeded -> download + proceed silently.
#   - a build is still running       -> WAIT for it (gh run watch) to finish.
#   - a build FAILED / has no run / the commit isn't pushed -> STOP and prompt:
#       [A] Abort        - publish NOTHING (default).
#       [R] Re-check     - poll CI again (after a build finishes).
#       [P] Push         - push the branch to the source repo, then re-check.
#       [S] Windows-only - publish with NO Linux/macOS assets.
#       [O] Old builds   - attach mismatched, older builds (must type OLD).
#     It NEVER silently attaches a stale build and NEVER publishes a partial
#     release without an explicit choice.
# Pass -NoPause (unattended): if the matching builds aren't ready the release is
# ABORTED (nothing published) rather than shipping stale artifacts. -DryRun
# reports CI status but publishes nothing.
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

# Shipped runtime assets: pc_port/assets/ mirrors the game folder layout
# (assets/gamedata/decal.png ships as gamedata/decal.png). Both modes package
# from THIS directory — never glob the build folder's gamedata/, which holds
# the user's disc images and saves. RelPath is the runtime-relative POSIX path
# used in the zip, the manifest, and the loose-mode asset name.
$assetsDir = Join-Path $PSScriptRoot "..\pc_port\assets"
function Get-ShippedAssets {
    if (-not (Test-Path $assetsDir)) { return @() }
    $root = (Resolve-Path $assetsDir).Path
    return @(Get-ChildItem -Recurse -File $root | ForEach-Object {
        [pscustomobject]@{
            RelPath = $_.FullName.Substring($root.Length).TrimStart('\','/').Replace('\','/')
            Source  = $_.FullName
        }
    })
}

# Find the newest run of $Workflow on $Branch that was triggered by the exact
# commit this release is being built from (any status -- queued/in_progress/
# completed), so a pending run can be waited on instead of silently skipped.
function Get-CrossPlatformRunForCommit {
    param([string]$SourceSlug, [string]$Workflow, [string]$Branch, [string]$Commit)
    # ConvertFrom-Json emits a parsed JSON array as a single non-enumerated
    # pipeline object. @(X | ConvertFrom-Json) -- wrapping the LIVE PIPE, even
    # with the native command already split out -- treats that one emitted
    # object as ONE array element, silently nesting the real N-item array
    # inside a 1-item wrapper (.Count lies as 1). Piping that wrapper through
    # Where-Object then "self-heals" via PowerShell's collection dot-property
    # broadcasting, but the filter condition becomes "does ANY run in the
    # whole batch match" instead of "does THIS run match", so it silently
    # returns every run un-filtered (this produced the 20-run-id-joined-by-
    # spaces malformed URL / 404 seen in the wild -- TWICE, because the first
    # fix attempt still wrapped a live pipe here). ConvertFrom-Json's result
    # MUST be assigned to a plain variable in its OWN statement first; only
    # THEN is @(thatVariable) safe. Verified 2026-07-06 against the live repo.
    # A redirected stderr stream (2>$null) still routes through PowerShell's
    # error pipeline before being discarded -- under $ErrorActionPreference =
    # 'Stop' (set at the top of this script) a failing native command THROWS a
    # terminating RemoteException instead of just setting $LASTEXITCODE, even
    # though the redirect looks like it should suppress it. try/catch is
    # required for any gh call whose failure is meant to be handled gracefully.
    try { $rawJson = gh run list --repo $SourceSlug --workflow $Workflow --branch $Branch --limit 20 --json databaseId,headSha,status,conclusion,createdAt,url 2>$null }
    catch { $rawJson = $null }
    if ($LASTEXITCODE -ne 0 -or -not $rawJson) { return $null }
    $parsedRuns = $rawJson | ConvertFrom-Json
    $runs = @($parsedRuns)
    return ($runs | Where-Object { $_.headSha -eq $Commit } |
                     Sort-Object -Property createdAt -Descending | Select-Object -First 1)
}

# Is this commit present on the source repo at all? If not, CI cannot have built
# it (the release commit was never pushed) -- the #1 cause of stale attachments.
function Test-CommitOnSourceRepo {
    param([string]$SourceSlug, [string]$Commit)
    if (-not $Commit) { return $false }
    try { gh api "repos/$SourceSlug/commits/$Commit" --jq ".sha" 2>$null | Out-Null }
    catch { return $false }
    return ($LASTEXITCODE -eq 0)
}

# The local git remote whose push URL points at the source repo (so [P] can push
# the branch that feeds CI). Returns $null if there isn't one.
function Get-SourceRemoteName {
    param([string]$SourceSlug)
    $lines = git remote -v 2>$null
    foreach ($line in $lines) {
        if ($line -match "^(\S+)\s+\S*github\.com[:/]$([regex]::Escape($SourceSlug))(\.git)?\s+\(push\)") {
            return $matches[1]
        }
    }
    return $null
}

# Newest SUCCESSFUL run of a workflow on the branch, regardless of commit -- only
# used for the explicit [O] "attach older build" path.
function Get-NewestSuccessfulRun {
    param([string]$SourceSlug, [string]$Workflow, [string]$Branch)
    try { $raw = gh run list --repo $SourceSlug --workflow $Workflow --branch $Branch --status success --limit 1 --json databaseId,headSha 2>$null }
    catch { $raw = $null }
    if ($LASTEXITCODE -ne 0 -or -not $raw) { return $null }
    $parsed = $raw | ConvertFrom-Json
    $arr = @($parsed)
    if ($arr.Count -eq 0) { return $null }
    return $arr[0]
}

# Newest run of a workflow on the branch regardless of commit/status -- used ONLY
# to explain a 'no-run' state: it shows what CI's latest build actually is so a
# release commit that is newer than the last-built one (the real cause of the
# "no CI run for this commit yet" wait) is obvious instead of a bare "no run".
function Get-NewestRunAnyStatus {
    param([string]$SourceSlug, [string]$Workflow, [string]$Branch)
    try { $raw = gh run list --repo $SourceSlug --workflow $Workflow --branch $Branch --limit 1 --json databaseId,headSha,status,conclusion 2>$null }
    catch { $raw = $null }
    if ($LASTEXITCODE -ne 0 -or -not $raw) { return $null }
    $parsed = $raw | ConvertFrom-Json
    $arr = @($parsed)
    if ($arr.Count -eq 0) { return $null }
    return $arr[0]
}

# Download one CI artifact into $DlRoot; returns the extracted file paths (empty on failure).
function Invoke-CrossPlatformDownload {
    param([string]$SourceSlug, [string]$RunId, [string]$Artifact, [string]$DlRoot)
    $outDir = Join-Path $DlRoot $Artifact
    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue }
    try { gh run download $RunId --repo $SourceSlug --name $Artifact --dir $outDir 2>$null | Out-Null }
    catch { }
    if ($LASTEXITCODE -ne 0) { return @() }
    return @(Get-ChildItem -Recurse -File $outDir -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}

# Resolve the Linux + macOS builds for THIS commit BEFORE the release is created.
# Waits for in-progress builds; on failure/missing/not-pushed it STOPS and asks
# (or aborts unattended). Returns @{ Proceed=$bool; Assets=@(paths); DlRoot=path }.
# Never silently falls back to a stale build.
function Resolve-CrossPlatformArtifacts {
    param([string]$SourceCommit)

    $result = [ordered]@{ Proceed = $true; Assets = @(); DlRoot = $null }
    if ($SkipCrossPlatform) {
        Write-Host "Cross-platform builds skipped (-SkipCrossPlatform)." -ForegroundColor Yellow
        return $result
    }

    $sourceSlug  = $SourceRepoUrl -replace '^https?://github\.com/', '' -replace '/$', ''
    $shortCommit = if ($SourceCommit) { $SourceCommit.Substring(0, [Math]::Min(9, $SourceCommit.Length)) } else { '(none)' }
    $targets = @(
        @{ Name = 'Linux'; Workflow = 'build-linux.yml'; Artifact = 'SHPC-linux-x64' },
        @{ Name = 'macOS'; Workflow = 'build-macos.yml'; Artifact = 'SHPC-macos-arm64' }
    )

    # gh run download creates the dir tree; only made when we actually download,
    # so a dry run / abort leaves nothing behind. Cleared first if it lingers.
    $dlRoot = Join-Path ([IO.Path]::GetTempPath()) "shpc-xplat-$shortCommit"
    if (Test-Path $dlRoot) { Remove-Item -Recurse -Force $dlRoot -ErrorAction SilentlyContinue }
    $result.DlRoot = $dlRoot

    $commitPushed = Test-CommitOnSourceRepo -SourceSlug $sourceSlug -Commit $SourceCommit

    while ($true) {
        Write-Host ""
        Write-Host "Verifying Linux + macOS CI builds for commit $shortCommit on $sourceSlug ($CrossPlatformBranch)..." -ForegroundColor Cyan
        if (-not $commitPushed) {
            Write-Host "  ! Commit $shortCommit is NOT on $sourceSlug -- CI cannot have built it (push it first)." -ForegroundColor Yellow
        }

        # Evaluate each platform (waiting on in-progress builds when interactive).
        $status = @()
        foreach ($t in $targets) {
            $entry = [ordered]@{ Target = $t; State = ''; Run = $null; RunId = $null; Url = $null }
            $run = if ($SourceCommit) {
                Get-CrossPlatformRunForCommit -SourceSlug $sourceSlug -Workflow $t.Workflow -Branch $CrossPlatformBranch -Commit $SourceCommit
            } else { $null }

            if (-not $run) {
                $entry.State = if ($commitPushed) { 'no-run' } else { 'not-pushed' }
            } else {
                $entry.Run = $run; $entry.RunId = "$($run.databaseId)".Trim(); $entry.Url = $run.url
                if ($run.status -eq 'completed' -and $run.conclusion -eq 'success') {
                    $entry.State = 'ok'
                } elseif ($run.status -eq 'completed') {
                    $entry.State = 'failed'
                } elseif (@('in_progress','queued','requested','waiting','pending') -contains $run.status) {
                    if ($NoPause -or $DryRun) {
                        $entry.State = 'running'
                    } else {
                        Write-Host "  Waiting for $($t.Workflow) (run $($entry.RunId)) to finish -- Ctrl+C aborts the release..." -ForegroundColor Cyan
                        $ok = $false
                        try { gh run watch $entry.RunId --repo $sourceSlug --interval 15 --exit-status; $ok = ($LASTEXITCODE -eq 0) }
                        catch { $ok = $false }
                        $entry.State = if ($ok) { 'ok' } else { 'failed' }
                    }
                } else {
                    $entry.State = 'failed'
                }
            }
            $status += $entry
        }

        # Report per-platform.
        Write-Host ""
        foreach ($e in $status) {
            $label = $e.Target.Name.PadRight(6)
            switch ($e.State) {
                'ok'         { Write-Host "  $label : OK (run $($e.RunId))" -ForegroundColor Green }
                'failed'     { Write-Host "  $label : BUILD FAILED -- $($e.Url)" -ForegroundColor Red }
                'running'    { Write-Host "  $label : still building -- $($e.Url)" -ForegroundColor Yellow }
                'no-run'     {
                    Write-Host "  $label : no CI run for commit $shortCommit yet" -ForegroundColor Yellow
                    $newest = Get-NewestRunAnyStatus -SourceSlug $sourceSlug -Workflow $e.Target.Workflow -Branch $CrossPlatformBranch
                    if ($newest) {
                        $nshaFull = "$($newest.headSha)"
                        $nsha = $nshaFull.Substring(0, [Math]::Min(9, $nshaFull.Length))
                        if ($nshaFull -eq $SourceCommit) {
                            Write-Host "           (CI is still registering the run for this commit -- choose R to re-check)" -ForegroundColor DarkYellow
                        } else {
                            Write-Host "           CI's newest $($e.Target.Workflow) build is $nsha ($($newest.status)/$($newest.conclusion)); you're releasing $shortCommit, which CI has NOT built. Push it / wait for CI, or [O] to attach the older $nsha build." -ForegroundColor DarkYellow
                        }
                    } else {
                        Write-Host "           (couldn't list runs -- gh error or no runs exist on $CrossPlatformBranch)" -ForegroundColor DarkYellow
                    }
                }
                'not-pushed' { Write-Host "  $label : commit not pushed to $sourceSlug" -ForegroundColor Yellow }
                default      { Write-Host "  $label : $($e.State)" -ForegroundColor Yellow }
            }
        }

        $notOk = @($status | Where-Object { $_.State -ne 'ok' })
        if ($notOk.Count -eq 0) {
            if ($DryRun) {
                Write-Host "  [DryRun] all cross-platform builds match commit $shortCommit -- would attach them." -ForegroundColor Green
                return $result
            }
            $assets = @()
            $allDownloaded = $true
            foreach ($e in $status) {
                $files = @(Invoke-CrossPlatformDownload -SourceSlug $sourceSlug -RunId $e.RunId -Artifact $e.Target.Artifact -DlRoot $dlRoot)
                if ($files.Count -eq 0) {
                    Write-Host "  ! Download of $($e.Target.Artifact) (run $($e.RunId)) failed." -ForegroundColor Red
                    $allDownloaded = $false
                } else {
                    $files | ForEach-Object { $assets += $_; Write-Host "  + $([IO.Path]::GetFileName($_)) (run $($e.RunId))" -ForegroundColor Gray }
                }
            }
            if ($allDownloaded) {
                Write-Host "  All cross-platform builds match commit $shortCommit." -ForegroundColor Green
                $result.Assets = $assets
                return $result
            }
            # download failure falls through to the gate
        }

        # How to inspect the builds.
        Write-Host ""
        Write-Host "  Check these builds yourself:" -ForegroundColor Gray
        Write-Host "    gh run list --repo $sourceSlug --workflow build-linux.yml --branch $CrossPlatformBranch" -ForegroundColor Gray
        Write-Host "    gh run view <id> --repo $sourceSlug --log-failed     (why a run failed)" -ForegroundColor Gray
        Write-Host "    https://github.com/$sourceSlug/actions" -ForegroundColor Gray
        if (-not $commitPushed) {
            $pr = Get-SourceRemoteName -SourceSlug $sourceSlug
            $hint = if ($pr) { "git push $pr $CrossPlatformBranch" } else { "git push <remote-for-$sourceSlug> $CrossPlatformBranch" }
            Write-Host "    This commit isn't pushed. Push it so CI builds it:  $hint" -ForegroundColor Gray
        }

        # Non-interactive: report only / refuse to ship stale.
        if ($DryRun) {
            Write-Host "  [DryRun] cross-platform not fully ready -- would prompt here (nothing is published in a dry run)." -ForegroundColor Yellow
            $result.Assets = @()
            return $result
        }
        if ($NoPause) {
            Write-Host "  Unattended (-NoPause): refusing to publish without matching Linux/macOS builds. Nothing published." -ForegroundColor Red
            $result.Proceed = $false
            return $result
        }

        # Interactive gate.
        Write-Host ""
        Write-Host "  Linux/macOS builds are NOT ready for this commit. Choose:" -ForegroundColor Yellow
        Write-Host "    [A] Abort        - publish NOTHING (default)" -ForegroundColor Gray
        Write-Host "    [R] Re-check     - poll CI again (use after a build finishes)" -ForegroundColor Gray
        Write-Host "    [P] Push+recheck - push '$CrossPlatformBranch' to $sourceSlug, then re-check" -ForegroundColor Gray
        Write-Host "    [S] Windows-only - publish with NO Linux/macOS assets" -ForegroundColor Gray
        Write-Host "    [O] Old builds   - attach mismatched, older builds (must type OLD)" -ForegroundColor Gray
        $choice = (Read-Host "  Choose A/R/P/S/O").Trim().ToUpper()
        switch ($choice) {
            'A' { $result.Proceed = $false; return $result }
            'R' { continue }
            'P' {
                $pr = Get-SourceRemoteName -SourceSlug $sourceSlug
                if (-not $pr) {
                    Write-Host "  Couldn't find a git remote for $sourceSlug -- push manually, then choose R." -ForegroundColor Red
                } else {
                    Write-Host "  Pushing '$CrossPlatformBranch' to $pr..." -ForegroundColor Cyan
                    try { git push $pr $CrossPlatformBranch } catch { Write-Host "  Push error: $_" -ForegroundColor Red }
                    if ($LASTEXITCODE -ne 0) {
                        Write-Host "  Push failed -- resolve and choose R to re-check." -ForegroundColor Red
                    } else {
                        Write-Host "  Pushed. CI should start shortly; choosing R waits for it." -ForegroundColor Green
                        $commitPushed = Test-CommitOnSourceRepo -SourceSlug $sourceSlug -Commit $SourceCommit
                    }
                }
                continue
            }
            'S' {
                Write-Host "  Publishing Windows-only (no cross-platform assets)." -ForegroundColor Yellow
                $result.Assets = @()
                return $result
            }
            'O' {
                $confirm = (Read-Host "  Type OLD to confirm attaching mismatched, older builds").Trim()
                if ($confirm -ne 'OLD') { Write-Host "  Not confirmed -- re-checking." -ForegroundColor Yellow; continue }
                $assets = @()
                foreach ($e in $status) {
                    $useRun = if ($e.State -eq 'ok') { $e.Run } else {
                        Get-NewestSuccessfulRun -SourceSlug $sourceSlug -Workflow $e.Target.Workflow -Branch $CrossPlatformBranch
                    }
                    if (-not $useRun) {
                        Write-Host "  ! No successful $($e.Target.Workflow) build exists at all -- skipping $($e.Target.Artifact)." -ForegroundColor Red
                        continue
                    }
                    $rid = "$($useRun.databaseId)".Trim()
                    $sha = "$($useRun.headSha)".Trim()
                    if ($SourceCommit -and $sha -ne $SourceCommit) {
                        Write-Host "  Attaching $($e.Target.Artifact) from OLDER commit $($sha.Substring(0, [Math]::Min(9, $sha.Length)))." -ForegroundColor Yellow
                    }
                    $files = @(Invoke-CrossPlatformDownload -SourceSlug $sourceSlug -RunId $rid -Artifact $e.Target.Artifact -DlRoot $dlRoot)
                    if ($files.Count -eq 0) { Write-Host "  ! Download of $($e.Target.Artifact) failed." -ForegroundColor Red }
                    else { $files | ForEach-Object { $assets += $_ } }
                }
                $result.Assets = $assets
                return $result
            }
            default { Write-Host "  Enter A, R, P, S, or O." -ForegroundColor Yellow; continue }
        }
    }
}

# Upload the already-resolved cross-platform artifacts to the just-created release.
# They were verified + downloaded BEFORE the release was created, so there is no
# wait here and no chance of attaching something stale.
function Publish-CrossPlatformAssets {
    param([string]$Tag, $Resolved)
    if (-not $Resolved -or -not $Resolved.Assets -or $Resolved.Assets.Count -eq 0) {
        Write-Host "  No cross-platform assets to attach." -ForegroundColor Yellow
        return
    }
    gh release upload $Tag --repo $Repo @($Resolved.Assets) --clobber
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  WARN: gh release upload of cross-platform assets failed (exit $LASTEXITCODE)." -ForegroundColor Yellow
    } else {
        Write-Host "  Attached $($Resolved.Assets.Count) cross-platform asset(s) to $Tag." -ForegroundColor Green
    }
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
    try { $json = gh release list --repo $Repo --limit 100 --json tagName,createdAt 2>$null }
    catch { $json = $null }
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
    # If the recorded baseline commit isn't an ancestor of HEAD -- the branch
    # history was rewritten/rebased since that release (e.g. pc-port squashed on
    # 2026-07-08, preserved as backup/pc-port-old-history-*), or the commit is
    # missing entirely -- then "<commit>..HEAD" walks all the way back to the
    # common root and dumps the ENTIRE project history (thousands of commits) into
    # the changelog. Guard against it: only auto-list when the baseline is a true
    # ancestor; otherwise leave the section empty for the maintainer to fill in.
    # (2>$null under $ErrorActionPreference='Stop' can THROW for a missing commit,
    # so wrap it -- see the gh-call note above.)
    $baselineIsAncestor = $false
    try {
        git merge-base --is-ancestor $baseManifest.git_commit HEAD 2>$null
        $baselineIsAncestor = ($LASTEXITCODE -eq 0)
    } catch { $baselineIsAncestor = $false }

    if (-not $baselineIsAncestor) {
        Write-Host "Warning: previous release commit $($baseManifest.git_commit) is not an ancestor of HEAD" -ForegroundColor Yellow
        Write-Host "         (branch history was likely rewritten). Skipping the auto commit list to avoid" -ForegroundColor Yellow
        Write-Host "         dumping the whole history -- add this release's notes to CHANGELOG.md by hand." -ForegroundColor Yellow
    } else {
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

# ---- Verify + download Linux/macOS builds BEFORE publishing anything --------
# The gate that makes the release WAIT for cross-platform CI and refuse to ship
# stale artifacts. It runs before EITHER mode creates a release, so a not-ready
# build aborts the whole release with nothing published (rather than posting
# Windows first and discovering Linux/macOS aren't ready afterward).
$xplat = Resolve-CrossPlatformArtifacts -SourceCommit $curCommitFull
if (-not $xplat.Proceed) {
    if ($xplat.DlRoot -and (Test-Path $xplat.DlRoot)) { Remove-Item -Recurse -Force $xplat.DlRoot -ErrorAction SilentlyContinue }
    Write-Host ""
    Write-Host "Release aborted -- nothing was published." -ForegroundColor Red
    exit 1
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

        # Shipped runtime assets (pc_port/assets/ -> zip root, runtime layout).
        foreach ($a in Get-ShippedAssets) {
            $dst = Join-Path $stage ($a.RelPath -replace '/', '\')
            New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
            Copy-Item $a.Source $dst -Force
        }

        # Hash every staged file for the manifest (relative POSIX paths).
        $stageFull = (Resolve-Path $stage).Path
        $files = @()
        Get-ChildItem -Recurse -File $stage | ForEach-Object {
            $rel = $_.FullName.Substring($stageFull.Length).TrimStart('\','/').Replace('\','/')
            $files += [ordered]@{ path = $rel; sha256 = (Get-Sha256 $_.FullName) }
        }
        Write-Host "Staged $($files.Count) files for the zip." -ForegroundColor Cyan

        # Build the zip entry-by-entry -- NOT ZipFile.CreateFromDirectory.
        # Verified 2026-07-05: on Windows PowerShell / .NET Framework,
        # CreateFromDirectory writes entry names using '\' (the platform
        # separator) for nested files, e.g. a raw byte dump of the local file
        # header showed "maps\SilentHillPC.dll" (0x5C), not "maps/..." (0x2F).
        # The ZIP spec calls for '/'; Unix unzip/zipfile then reads that '\'
        # as a literal backslash IN the filename instead of a directory
        # separator, so Mac/Linux users complained the archive had files with
        # backslashes baked into their names. Building entries by hand lets
        # every name be forward-slash-normalized (same fix as the manifest's
        # $rel above), and also lets the empty gamedata/save dir still get an
        # explicit directory entry, which CreateFromDirectory provided for
        # free but a plain file-only loop would otherwise drop.
        $zipName = "SHPC_$newTag.zip"
        $zipPath = Join-Path ([IO.Path]::GetTempPath()) $zipName
        if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
        Add-Type -AssemblyName System.IO.Compression

        $zipFs = New-Object System.IO.FileStream($zipPath, [System.IO.FileMode]::Create)
        try {
            $zipArchive = New-Object System.IO.Compression.ZipArchive($zipFs, [System.IO.Compression.ZipArchiveMode]::Create)
            try {
                Get-ChildItem -Recurse -File $stage | ForEach-Object {
                    $entryName = $_.FullName.Substring($stageFull.Length).TrimStart('\','/').Replace('\','/')
                    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zipArchive, $_.FullName, $entryName) | Out-Null
                }
                # Directories with no files anywhere under them (gamedata/save)
                # are never visited by the file loop above -- give them an
                # explicit zero-byte directory entry (name ending in '/') so
                # the folder still exists after extraction.
                Get-ChildItem -Recurse -Directory $stage | ForEach-Object {
                    if (-not (Get-ChildItem -Recurse -File $_.FullName -ErrorAction SilentlyContinue)) {
                        $entryName = $_.FullName.Substring($stageFull.Length).TrimStart('\','/').Replace('\','/') + '/'
                        $zipArchive.CreateEntry($entryName) | Out-Null
                    }
                }
            } finally {
                $zipArchive.Dispose()
            }
        } finally {
            $zipFs.Dispose()
        }
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
        try { gh api "repos/$Repo/branches/$BetaBranch" 2>$null | Out-Null }
        catch { }
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

        Publish-CrossPlatformAssets -Tag $newTag -Resolved $xplat

        Remove-Item $zipPath, $manifestPath, $notesFile -Force -ErrorAction SilentlyContinue
        if ($xplat.DlRoot -and (Test-Path $xplat.DlRoot)) { Remove-Item -Recurse -Force $xplat.DlRoot -ErrorAction SilentlyContinue }
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

# Shipped runtime assets (manifest path = runtime-relative, e.g. gamedata/decal.png;
# the launcher creates missing dirs on apply, same as maps/). Sourced from
# pc_port/assets/, remembered here so the upload stage doesn't look in BuildDir.
$assetSources = @{}
foreach ($a in Get-ShippedAssets) {
    $localFiles[$a.RelPath]   = Get-Sha256 $a.Source
    $assetSources[$a.RelPath] = $a.Source
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
           elseif ($assetSources.ContainsKey($path)) { $assetSources[$path] }
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

Publish-CrossPlatformAssets -Tag $newTag -Resolved $xplat

Remove-Item -Recurse -Force $stagingDir
if ($xplat.DlRoot -and (Test-Path $xplat.DlRoot)) { Remove-Item -Recurse -Force $xplat.DlRoot -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "Done. Release published: https://github.com/$Repo/releases/tag/$newTag" -ForegroundColor Green
Write-Host "Commit the updated CHANGELOG.md when ready:" -ForegroundColor Cyan
Write-Host "  git add pc_port/CHANGELOG.md && git commit -m 'changelog: $newTag'" -ForegroundColor Gray
