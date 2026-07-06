# Nightly Build Releases

Auto-update system for the Silent Hill PC port. Per-file SHA-256 deltas,
hosted on a standalone GitHub repo so the main decomp's watchers don't get
spammed.

## One-time setup

1. **Create the nightly repo on GitHub:**
   - Owner/name: `SlickAmogus/silent-hill-pc-nightly`
   - Public, standalone (NOT a fork of silent-hill-decomp)
   - README pointing back to the main repo is enough; no code lives here
   - The launcher hardcodes this name. If you change it, update
     `RepoOwner` / `RepoName` in `pc_port/launcher/.../UpdateChecker.cs`.

2. **Install GitHub CLI** (`gh`) and authenticate:
   ```
   gh auth login
   ```
   The release script requires `gh` in PATH (already installed on the
   build machine — `C:\Program Files\GitHub CLI\gh.exe`).

## Publishing a nightly

After a clean local build (`cmake --build build`):

```powershell
.\tools\release-nightly.ps1            # publishes if anything changed
.\tools\release-nightly.ps1 -DryRun    # shows what would happen, uploads nothing
```

The script:
- Hashes `SilentHillPC.exe` + every file in `build/maps/` + `build/config.cfg`
- Pulls the previous release's `version.json` from the nightly repo
- Diffs hashes; if nothing changed, exits without creating a release
- Computes the next version: `YYYY.MM.DD.N` (N = next counter for today)
- Creates a pre-release tag `vYYYY.MM.DD.N` on the nightly repo
- Uploads ONLY the changed files as release assets
- Generates a new `version.json` that points unchanged files at their
  previous release's URL, changed files at the new release's URL
- Uploads `version.json` as the release manifest

## Cross-platform (Linux + macOS) builds

The Windows binary is built locally; the Linux and macOS binaries are built by
GitHub Actions (`.github/workflows/build-linux.yml`, `build-macos.yml`) because
they can't be produced on Windows. CI only **builds** them — you remain the only
publisher. Each push to `pc-port` uploads a 90-day artifact (`SHPC-linux-x64`,
`SHPC-macos-arm64`).

They're attached **by default** — no flag needed:

```powershell
.\tools\release-nightly.ps1
.\tools\release-nightly.ps1 -SkipCrossPlatform   # Windows-only release
```

This downloads the newest successful CI artifacts and attaches them to the same
release as standalone archives (`SHPC-linux-x64.tar.gz`, `SHPC-macos-arm64.zip`).
They are deliberately **left out of `version.json`** — the Windows launcher only
knows how to hash/replace Windows files, so Linux/macOS users grab those archives
by hand, or via the launcher's Build Settings "Download archives for" checkboxes
(they don't auto-update through the normal Windows update flow).

**If CI for this exact commit hasn't finished yet:** instead of silently
attaching a stale build from an older commit, the script detects the
in-progress/queued run and asks, per platform:
- **[W] Wait** — polls the run (`gh run watch`) until it finishes, then attaches it
- **[V] View** — prints the run's current status/URL and lets you re-check
- **[S] Skip** — attaches the newest already-successful build anyway (from an
  earlier commit); re-run the release once CI catches up for a matching build

Running with `-NoPause` or `-DryRun` skips the prompt and always falls back to
the newest successful build non-interactively (unattended use).

Notes:
- Linux/macOS binaries dynamically link system SDL2/OpenAL (not bundled); each
  artifact ships a `README-{linux,macos}.txt` listing the runtime deps.
- The macOS arm64 binary is unsigned — Gatekeeper warns; users clear quarantine
  with `xattr -dr com.apple.quarantine SilentHillPC`.

## How the launcher consumes it

`UpdateChecker.cs` hits the stable GitHub URL:
```
https://github.com/SlickAmogus/silent-hill-pc-nightly/releases/latest/download/version.json
```
GitHub auto-redirects this to whichever release is newest. The launcher
then:
1. Parses the manifest (JSON)
2. SHA-256-hashes each local file
3. Builds a list of files whose hash differs (or that are missing locally)
4. Confirms with the user (file count + size + version label)
5. Downloads only the changed files into a temp dir
6. Verifies each download's hash against the manifest
7. Atomically replaces the local files (`config.cfg` is preserved if it
   already exists — we never overwrite user-tuned config)

The launcher has the "Check for Updates" button at the bottom-left of
Form1; click it to run the flow.

## Why watchers don't get spammed

GitHub release notifications fire to repository watchers, regardless of
the "pre-release" flag. Putting nightly releases on a separate repo
(`silent-hill-pc-nightly`) means only people who explicitly star/watch
THAT repo get pinged. Watchers of the main decomp repo only see the
manual milestone releases you make there.

This is the same pattern VS Code (Insiders), Discord (Canary), and
Chromium (Canary) use.

## Troubleshooting

**"gh: command not found"** — `gh auth status` should succeed. If not,
re-install from https://cli.github.com/ and run `gh auth login`.

**"Nothing to release"** — Either you haven't rebuilt since the last
release, or no files actually changed. Use `-DryRun` to verify.

**Launcher reports "Update failed: 404"** — The nightly repo probably
has no releases yet. Publish a first one with `release-nightly.ps1`
(it'll happily create the very first release).

**Launcher reports "Hash mismatch"** — A downloaded file's SHA-256 didn't
match the manifest. Either the manifest is stale (re-run the release
script to regenerate) or the asset got corrupted in transit (retry).
No files are replaced on hash failure — partial updates are impossible.
