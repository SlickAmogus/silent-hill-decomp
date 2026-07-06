# Launcher — Antivirus False-Positive Notes

The launcher (`SilentHillPC_Launcher.exe`) is increasingly flagged by antivirus
engines. This documents *why* and *what was done / what still needs doing*.

## What it actually is

A plain **.NET Framework 4.7.2 WinForms** app (`WinExe`). It is **not** packed,
obfuscated, single-file/self-contained, trimmed, or ReadyToRun. It has **no**
`VirtualAlloc`/`WriteProcessMemory`, no registry writes, no `Assembly.Load`, no
startup-folder self-copy, and no admin manifest. So the detections are **false
positives**, not a real malware signature.

## Why AV flags it (root causes)

1. **Unsigned + zero reputation + a new hash every nightly.** No Authenticode
   signature anywhere, and `AssemblyFileVersion` bumps every build, so each
   nightly is a never-before-seen unsigned binary. Reputation engines
   (SmartScreen, Defender cloud) treat low-prevalence unsigned exes as
   suspicious — and it gets *worse over time* as more one-off hashes accumulate
   from the same unsigned "publisher." This matches "increasingly detected."

2. **Downloader/self-updater behavior.** `UpdateChecker.cs` fetches files —
   including `.exe`s — from GitHub release URLs into `%TEMP%`, copies them into
   the install dir, and the running launcher **replaces its own exe**
   (move-live-exe-to-`.old`, drop the downloaded one in). Then `Form1.cs`
   `Process.Start`s the game exe. "Download an exe and run/replace an exe" is the
   textbook heuristic for a trojan-downloader, independent of signing.

## Done in-repo (this change)

- **Filled in publisher metadata** (`Properties/AssemblyInfo.cs`): Company,
  Description, Product, Copyright — blank fields are a known ML signal.
- **Added `app.manifest`** wired via `<ApplicationManifest>`: stable app
  identity, `asInvoker` (never elevate), supported-OS list. Unsigned
  .NET exes with no manifest score higher.
  **Caution:** the manifest originally declared `dpiAware=true`, which broke
  the fixed-pixel WinForms layout on scaled displays (fonts grew, layout
  didn't — clipped controls). It is now explicitly `false` and must stay
  that way; the dpiAware flag has negligible AV-heuristic weight anyway.

These lower the heuristic weight but do **not** fix the two root causes.

## Still needed (require action outside the code)

1. **Code-sign the exe (highest impact by far).** Sign
   `SilentHillPC_Launcher.exe` with a code-signing cert — ideally **EV** (grants
   immediate SmartScreen reputation), or at least **OV**. Add a post-build step:
   `signtool sign /fd sha256 /tr <timestamp-url> /td sha256 <exe>`. A signed
   binary from a known publisher is trusted far more readily and lets reputation
   accrue across nightlies instead of resetting.

2. **Reduce the "downloader that runs an exe" signature.** Prefer delivering
   game-data updates as data/zip; avoid the launcher writing/replacing `.exe`
   files itself. If self-update must stay, move it into a small **separate,
   signed** updater rather than the main exe rewriting its own image. Keep
   downloads over HTTPS to github.com (already the case).

3. **Submit false-positive reports** to each flagging vendor (Microsoft
   Defender/SmartScreen portal first), ideally *after* signing so the whitelist
   attaches to the publisher cert and covers future nightlies.

4. **Cut fewer, signed, versioned releases** so prevalence/reputation can build
   (build is already `Deterministic`).
