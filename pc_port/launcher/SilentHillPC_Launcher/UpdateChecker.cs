using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Pulls a version.json manifest from the configured nightly repo/branch/build
    /// and computes which local files differ.
    ///
    ///   loose mode - manifest files each carry a per-file download URL; only the
    ///                changed files are pulled. (alpha stream / default branch.)
    ///   zip   mode - manifest carries a single zip_url; the launcher does a cheap
    ///                hash check, and only when applying does it download + extract
    ///                the zip and copy the changed files. (beta branch.)
    ///
    /// The launcher will replace its OWN exe, but ONLY when the incoming build's
    /// launcher_version is strictly newer than the running one (so pinning an old
    /// build never downgrades the launcher). config.cfg and gamedata/save are
    /// treated as user data: never counted as updates, never overwritten.
    /// </summary>
    public static class UpdateChecker
    {
        public const string LauncherFileName = "SilentHillPC_Launcher.exe";

        // Cross-platform release assets (release-nightly.ps1's Add-CrossPlatformAssets
        // attaches these under fixed names to every release, when available). They
        // never appear in version.json — Windows never auto-updates them.
        public const string LinuxArchiveName = "SHPC-linux-x64.tar.gz";
        public const string MacArchiveName   = "SHPC-macos-arm64.zip";

        // The beta stream lives on this branch in the official repo. alpha+latest
        // users are auto-migrated to it once a newer beta release appears, so the
        // move to beta zip builds needs no manual Build Settings change.
        public const string BetaBranch = "beta";

        // -- DTOs (manifest) --------------------------------------------------

        [DataContract]
        public class Manifest
        {
            [DataMember(Name = "version")]          public string Version;
            [DataMember(Name = "tag")]              public string Tag;
            [DataMember(Name = "mode")]             public string Mode;        // "loose"|"zip"|null(=loose)
            [DataMember(Name = "branch")]           public string Branch;
            [DataMember(Name = "build_date")]       public string BuildDate;
            [DataMember(Name = "launcher_version")] public string LauncherVersion;
            [DataMember(Name = "zip_url")]          public string ZipUrl;
            [DataMember(Name = "zip_name")]         public string ZipName;
            [DataMember(Name = "files")]            public List<FileEntry> Files;
        }

        [DataContract]
        public class FileEntry
        {
            [DataMember(Name = "path")]   public string Path;
            [DataMember(Name = "sha256")] public string Sha256;
            [DataMember(Name = "url")]    public string Url;
        }

        public class UpdatePlan
        {
            public string RemoteVersion;
            public string BuildDate;
            public string Mode = "loose";   // "loose" | "zip"
            public string ZipUrl;           // zip mode only
            public string RepoLabel;        // for dialogs, e.g. "owner/repo @ beta (beta-2026.06.22.1)"
            public string ChangelogUrl;     // CHANGELOG.md asset for this build (preview before installing)
            public string LauncherVersion;  // incoming launcher version (if any)
            public bool   LauncherIsNewer;  // incoming launcher strictly newer than ours
            public string MigrateToBranch;  // non-null => switch the user's branch to this on apply (alpha->beta)
            public bool   IsBeta;          // newest build is a beta-branch release (surfaced in the update dialog)
            public List<FileEntry> Changed = new List<FileEntry>();
            public bool HasUpdate => Changed.Count > 0;

            public FileEntry LauncherEntry =>
                Changed.FirstOrDefault(f => IsLauncherFile(f.Path));
        }

        // -- DTOs (GitHub API) ------------------------------------------------

        [DataContract]
        public class GhRelease
        {
            [DataMember(Name = "tag_name")]         public string TagName;
            [DataMember(Name = "name")]             public string Name;
            [DataMember(Name = "target_commitish")] public string TargetCommitish;
            [DataMember(Name = "created_at")]       public string CreatedAt;
            [DataMember(Name = "prerelease")]       public bool   Prerelease;
            [DataMember(Name = "draft")]            public bool   Draft;
            [DataMember(Name = "assets")]           public List<GhAsset> Assets;
        }

        [DataContract]
        public class GhAsset
        {
            [DataMember(Name = "name")]                 public string Name;
            [DataMember(Name = "browser_download_url")] public string BrowserDownloadUrl;
        }

        [DataContract]
        public class GhBranch
        {
            [DataMember(Name = "name")] public string Name;
        }

        public class BuildInfo
        {
            public string Tag;
            public string Label;   // shown in the dropdown
        }

        // -- HTTP -------------------------------------------------------------

        private static readonly HttpClient _http = CreateHttpClient();
        private static HttpClient CreateHttpClient()
        {
            var c = new HttpClient();
            c.DefaultRequestHeaders.Add("User-Agent", "SilentHillPC-Launcher");
            c.Timeout = TimeSpan.FromMinutes(10); // zip downloads can be large
            return c;
        }

        // -- Public: update check ---------------------------------------------

        /// <summary>
        /// Fetches the manifest for the configured repo/branch/build and computes
        /// which local files differ. Does NOT download the zip in zip mode.
        /// </summary>
        public static async Task<UpdatePlan> CheckAsync(string installDir, LauncherSettings settings,
                                                        CancellationToken ct = default(CancellationToken))
        {
            string owner, repo;
            if (!settings.TryGetOwnerRepo(out owner, out repo))
                throw new Exception("Invalid repo URL in launcher settings: " + settings.RepoUrl);

            var src      = await ResolveManifestSourceAsync(owner, repo, settings, ct).ConfigureAwait(false);
            var manifest = await FetchManifestAsync(src.Url, ct).ConfigureAwait(false);

            string mode = string.IsNullOrWhiteSpace(manifest.Mode) ? "loose" : manifest.Mode.Trim().ToLowerInvariant();

            var plan = new UpdatePlan
            {
                RemoteVersion   = manifest.Version,
                BuildDate       = manifest.BuildDate,
                Mode            = mode,
                ZipUrl          = manifest.ZipUrl,
                RepoLabel       = src.Label,
                ChangelogUrl    = DeriveChangelogUrl(src.Url),
                LauncherVersion = manifest.LauncherVersion,
                MigrateToBranch = src.MigrateBranch,
                IsBeta          = src.IsBeta,
            };

            if (manifest.Files != null)
            {
                foreach (var entry in manifest.Files)
                {
                    if (entry == null || string.IsNullOrEmpty(entry.Path)) continue;
                    if (IsUserDataPath(entry.Path)) continue; // config.cfg / saves: never an update

                    string localPath;
                    if (!TryResolveInstallPath(installDir, entry.Path, out localPath))
                        continue; // path escapes the install dir -> ignore (malicious/broken manifest)

                    if (!File.Exists(localPath) || Sha256Of(localPath) != (entry.Sha256 ?? "").ToLowerInvariant())
                        plan.Changed.Add(entry);
                }
            }

            // Launcher self-update gate.
            var launcherEntry = plan.LauncherEntry;
            if (launcherEntry != null)
            {
                // NEVER self-update the launcher from a non-default (untrusted)
                // repo. A third-party repo may update game data, but replacing the
                // launcher exe from it would hand that repo arbitrary code
                // execution on every launch — legit forks should ship their own
                // launcher download instead.
                if (!settings.IsDefaultRepo)
                    plan.Changed.Remove(launcherEntry);
                // Otherwise keep it only if it's strictly newer than ours, so an
                // older/equal launcher never lands on top of this one.
                else if (LauncherSettings.IsLauncherNewer(manifest.LauncherVersion, LauncherSettings.OwnLauncherVersion()))
                    plan.LauncherIsNewer = true;
                else
                    plan.Changed.Remove(launcherEntry);
            }

            return plan;
        }

        // -- Public: apply ----------------------------------------------------

        public static async Task ApplyAsync(string installDir, UpdatePlan plan, Action<double, string> progress,
                                            CancellationToken ct = default(CancellationToken))
        {
            if (!plan.HasUpdate) return;
            if (plan.Mode == "zip") await ApplyZipAsync(installDir, plan, progress, ct).ConfigureAwait(false);
            else                    await ApplyLooseAsync(installDir, plan, progress, ct).ConfigureAwait(false);
        }

        // -- Public: dropdown data (Build Settings modal) ---------------------

        public static async Task<List<string>> ListBranchesAsync(LauncherSettings settings,
                                                                 CancellationToken ct = default(CancellationToken))
        {
            string owner, repo;
            if (!settings.TryGetOwnerRepo(out owner, out repo))
                throw new Exception("Invalid repo URL: " + settings.RepoUrl);

            var url = $"https://api.github.com/repos/{owner}/{repo}/branches?per_page=100";
            var branches = await GetApiJsonAsync<List<GhBranch>>(url, ct).ConfigureAwait(false);
            return (branches ?? new List<GhBranch>())
                   .Select(b => b?.Name).Where(n => !string.IsNullOrEmpty(n)).ToList();
        }

        /// <summary>Releases that target the given branch ("" / null = any), newest first.</summary>
        public static async Task<List<BuildInfo>> ListBuildsAsync(LauncherSettings settings, string branch,
                                                                  CancellationToken ct = default(CancellationToken))
        {
            string owner, repo;
            if (!settings.TryGetOwnerRepo(out owner, out repo))
                throw new Exception("Invalid repo URL: " + settings.RepoUrl);

            var rels = await ListReleasesAsync(owner, repo, ct).ConfigureAwait(false);
            IEnumerable<GhRelease> cand;
            if (string.IsNullOrWhiteSpace(branch))
                cand = rels.Where(r => !IsBetaRelease(r)); // alpha = the v* (non-beta) stream
            else
                cand = rels.Where(r => string.Equals(r.TargetCommitish, branch, StringComparison.OrdinalIgnoreCase));

            return cand.Select(r => new BuildInfo
            {
                Tag   = r.TagName,
                Label = $"{r.TagName}  ({ParseDate(r.CreatedAt):yyyy-MM-dd}){(IsBetaRelease(r) ? "  [beta]" : "")}"
            }).ToList();
        }

        // -- Public: cross-platform archives (Build Settings) -----------------

        /// <summary>Resolve the download URL for a cross-platform archive asset
        /// (LinuxArchiveName / MacArchiveName) on the given branch/build, or null
        /// if that release doesn't have one (older release, predates the
        /// cross-platform attach, or that platform's CI never succeeded).</summary>
        public static async Task<string> GetCrossPlatformAssetUrlAsync(LauncherSettings settings, string branch,
            string build, string assetName, CancellationToken ct = default(CancellationToken))
        {
            string owner, repo;
            if (!settings.TryGetOwnerRepo(out owner, out repo))
                throw new Exception("Invalid repo URL: " + settings.RepoUrl);

            var releases = await ListReleasesAsync(owner, repo, ct).ConfigureAwait(false);

            GhRelease target;
            if (!string.IsNullOrEmpty(build) && !build.Equals("latest", StringComparison.OrdinalIgnoreCase))
            {
                target = releases.FirstOrDefault(r => string.Equals(r.TagName, build, StringComparison.OrdinalIgnoreCase));
            }
            else
            {
                IEnumerable<GhRelease> cand = releases;
                cand = string.IsNullOrWhiteSpace(branch)
                    ? cand.Where(r => !IsBetaRelease(r))
                    : cand.Where(r => string.Equals(r.TargetCommitish, branch, StringComparison.OrdinalIgnoreCase));
                target = cand.FirstOrDefault();
            }
            return target != null ? AssetUrl(target, assetName) : null;
        }

        /// <summary>Downloads a URL straight to destPath (via a .part sidecar so a
        /// cancelled/failed download never leaves a corrupt file at destPath).</summary>
        public static async Task DownloadFileAsync(string url, string destPath, Action<double, string> progress,
            CancellationToken ct = default(CancellationToken))
        {
            string tmp = destPath + ".part";
            using (var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
            {
                resp.EnsureSuccessStatusCode();
                long? total = resp.Content.Headers.ContentLength;
                using (var fs = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false))
                {
                    var buf = new byte[81920];
                    long read = 0; int n;
                    while ((n = await stream.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
                    {
                        await fs.WriteAsync(buf, 0, n, ct).ConfigureAwait(false);
                        read += n;
                        if (total.HasValue && total.Value > 0)
                            progress?.Invoke((double)read / total.Value, $"{read / (1024 * 1024)}/{total.Value / (1024 * 1024)} MB");
                    }
                }
            }
            if (File.Exists(destPath)) File.Delete(destPath);
            File.Move(tmp, destPath);
        }

        // -- Startup housekeeping ---------------------------------------------

        /// <summary>Best-effort removal of leftover update temp dirs (if a prior
        /// run was killed mid-apply). Safe to call on startup.</summary>
        public static void CleanupStaleTemp()
        {
            try
            {
                var tmp = Path.GetTempPath();
                foreach (var d in Directory.GetDirectories(tmp, "sh-zip-*").Concat(Directory.GetDirectories(tmp, "sh-nightly-*")))
                {
                    try { Directory.Delete(d, true); } catch { /* in use / best effort */ }
                }
            }
            catch { /* best effort */ }
        }

        // -- Internals: manifest resolution -----------------------------------

        private class ManifestSource { public string Url; public string Label; public string Tag; public string MigrateBranch; public bool IsBeta; }

        private static async Task<ManifestSource> ResolveManifestSourceAsync(
            string owner, string repo, LauncherSettings s, CancellationToken ct)
        {
            // Default ("latest") on the official repo: track the newest release
            // across ALL branches (alpha v* and beta beta-*), chosen by PARSED
            // VERSION. GitHub's releases/latest is NOT trustworthy here: every
            // release in the nightly repo shares one created_at (bulk upload), so
            // GitHub's date-ordered "latest" is unreliable and won't surface a
            // newer beta even when betas are regular (non-prerelease) releases
            // (verified: releases/latest returns the alpha while a newer beta
            // exists). The winning branch is recorded so the dialog can flag a
            // cross-branch (beta) update.
            if (s.IsDefaultBranch && s.IsLatestBuild)
            {
                if (s.IsDefaultRepo)
                {
                    var all    = await ListReleasesAsync(owner, repo, ct).ConfigureAwait(false);
                    // Newest by version; on a tie (a same-day alpha + beta share a
                    // version) prefer the beta — it's the leading-edge stream, and
                    // this is the case where an alpha launcher-delivery release ties
                    // the beta we want users to migrate onto.
                    var newest = all.OrderByDescending(r => VersionFromTag(r.TagName))
                                    .ThenByDescending(r => IsBetaRelease(r))
                                    .FirstOrDefault();
                    if (newest != null)
                    {
                        bool   isBeta = IsBetaRelease(newest);
                        string vUrl   = AssetUrl(newest, "version.json")
                                        ?? $"https://github.com/{owner}/{repo}/releases/download/{newest.TagName}/version.json";
                        return new ManifestSource
                        {
                            Url    = vUrl,
                            Label  = $"{owner}/{repo} ({(isBeta ? "beta" : "alpha")}, latest: {newest.TagName})",
                            Tag    = newest.TagName,
                            IsBeta = isBeta,
                        };
                    }
                }

                // Third-party repo (or no releases found): GitHub releases/latest.
                return new ManifestSource
                {
                    Url   = $"https://github.com/{owner}/{repo}/releases/latest/download/version.json",
                    Label = $"{owner}/{repo} (latest)",
                    Tag   = null
                };
            }

            var releases = await ListReleasesAsync(owner, repo, ct).ConfigureAwait(false);
            string branch = s.IsDefaultBranch ? null : s.Branch;

            GhRelease target;
            if (!s.IsLatestBuild)
            {
                target = releases.FirstOrDefault(r => string.Equals(r.TagName, s.Build, StringComparison.OrdinalIgnoreCase));
                if (target == null)
                    throw new Exception($"Build '{s.Build}' not found in {owner}/{repo}.");
            }
            else
            {
                IEnumerable<GhRelease> cand = releases;
                if (branch != null)
                    cand = cand.Where(r => string.Equals(r.TargetCommitish, branch, StringComparison.OrdinalIgnoreCase));
                target = cand.FirstOrDefault();
                if (target == null)
                    throw new Exception($"No releases found for branch '{(branch ?? "default")}' in {owner}/{repo}.");
            }

            string url = AssetUrl(target, "version.json")
                         ?? $"https://github.com/{owner}/{repo}/releases/download/{target.TagName}/version.json";
            string label = $"{owner}/{repo} @ {(branch ?? "alpha")} ({target.TagName})";
            return new ManifestSource { Url = url, Label = label, Tag = target.TagName };
        }

        // If the beta branch has a release newer than the alpha-latest manifest at
        // alphaUrl, return a source pointing at that beta release, flagged to switch
        // the user's branch to beta on apply. Best-effort: returns null (stay on
        // alpha) on any error or when there's no newer beta.
        private static async Task<ManifestSource> TryResolveBetaMigrationAsync(
            string owner, string repo, string alphaUrl, CancellationToken ct)
        {
            try
            {
                // First page is newest-first; the latest beta is the newest beta here.
                var url  = $"https://api.github.com/repos/{owner}/{repo}/releases?per_page=100";
                var rels = await GetApiJsonAsync<List<GhRelease>>(url, ct).ConfigureAwait(false);
                if (rels == null) return null;

                GhRelease beta = rels.FirstOrDefault(r => r != null && !r.Draft && IsBetaRelease(r));
                if (beta == null) return null;

                string betaUrl = AssetUrl(beta, "version.json")
                                 ?? $"https://github.com/{owner}/{repo}/releases/download/{beta.TagName}/version.json";

                string betaVer = (await FetchManifestAsync(betaUrl, ct).ConfigureAwait(false))?.Version;
                if (string.IsNullOrWhiteSpace(betaVer)) return null;

                string alphaVer = null;
                try { alphaVer = (await FetchManifestAsync(alphaUrl, ct).ConfigureAwait(false))?.Version; }
                catch { /* no alpha-latest published -> beta wins */ }

                if (string.IsNullOrWhiteSpace(alphaVer) ||
                    LauncherSettings.CompareVersions(betaVer, alphaVer) > 0)
                {
                    return new ManifestSource
                    {
                        Url           = betaUrl,
                        Label         = $"{owner}/{repo} @ beta ({beta.TagName})",
                        Tag           = beta.TagName,
                        MigrateBranch = BetaBranch,
                    };
                }
            }
            catch { /* migration is best-effort; fall back to alpha */ }
            return null;
        }

        private static bool IsBetaRelease(GhRelease r)
        {
            return string.Equals(r.TargetCommitish, BetaBranch, StringComparison.OrdinalIgnoreCase) ||
                   (r.TagName != null && r.TagName.StartsWith("beta-", StringComparison.OrdinalIgnoreCase));
        }

        private static async Task<List<GhRelease>> ListReleasesAsync(string owner, string repo, CancellationToken ct)
        {
            // GitHub caps the releases API at 100 per page, so paginate to pull
            // ALL releases — the nightly repo has 140+ and users should be able
            // to reach the very oldest. Cap at 10 pages (1000) as a runaway guard.
            var all = new List<GhRelease>();
            for (int page = 1; page <= 10; page++)
            {
                var url  = $"https://api.github.com/repos/{owner}/{repo}/releases?per_page=100&page={page}";
                var rels = await GetApiJsonAsync<List<GhRelease>>(url, ct).ConfigureAwait(false);
                if (rels == null || rels.Count == 0) break;
                all.AddRange(rels);
                if (rels.Count < 100) break; // last page reached
            }
            return all.Where(r => r != null && !r.Draft)
                      .OrderByDescending(r => VersionFromTag(r.TagName)).ToList();
        }

        // -- Internals: apply (loose / zip) -----------------------------------

        private static async Task ApplyLooseAsync(string installDir, UpdatePlan plan, Action<double, string> progress, CancellationToken ct)
        {
            string tmpRoot = Path.Combine(Path.GetTempPath(), "sh-nightly-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(tmpRoot);
            try
            {
                int idx = 0;
                foreach (var entry in plan.Changed)
                {
                    progress?.Invoke((double)idx / plan.Changed.Count, $"Downloading {entry.Path}...");

                    string tmpFile = Path.Combine(tmpRoot, entry.Path.Replace('/', '_'));
                    using (var resp = await _http.GetAsync(entry.Url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
                    {
                        resp.EnsureSuccessStatusCode();
                        using (var fs = new FileStream(tmpFile, FileMode.Create, FileAccess.Write, FileShare.None))
                        using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false))
                            await stream.CopyToAsync(fs).ConfigureAwait(false);
                    }

                    string actualHash = Sha256Of(tmpFile);
                    if (actualHash != (entry.Sha256 ?? "").ToLowerInvariant())
                        throw new Exception(
                            $"Hash mismatch for {entry.Path}: expected {entry.Sha256}, got {actualHash}. " +
                            "Download corrupted or manifest stale. Aborting (no files replaced).");
                    idx++;
                }

                progress?.Invoke(0.95, "Installing files...");
                foreach (var entry in plan.Changed)
                {
                    string tmpFile = Path.Combine(tmpRoot, entry.Path.Replace('/', '_'));
                    string dst;
                    if (!TryResolveInstallPath(installDir, entry.Path, out dst))
                        throw new Exception($"Refusing to write '{entry.Path}' — it escapes the install directory. Aborting.");

                    if (IsUserDataPath(entry.Path) && File.Exists(dst)) continue;

                    var dstDir = Path.GetDirectoryName(dst);
                    if (!string.IsNullOrEmpty(dstDir) && !Directory.Exists(dstDir)) Directory.CreateDirectory(dstDir);

                    ReplaceFile(tmpFile, dst);
                }

                progress?.Invoke(1.0, $"Updated to {plan.RemoteVersion}");
            }
            finally
            {
                try { Directory.Delete(tmpRoot, true); } catch { }
            }
        }

        private static async Task ApplyZipAsync(string installDir, UpdatePlan plan, Action<double, string> progress, CancellationToken ct)
        {
            if (string.IsNullOrWhiteSpace(plan.ZipUrl))
                throw new Exception("This zip release has no zip_url in its manifest.");

            string tmpRoot    = Path.Combine(Path.GetTempPath(), "sh-zip-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            string zipPath    = Path.Combine(tmpRoot, "update.zip");
            string extractDir = Path.Combine(tmpRoot, "x");
            Directory.CreateDirectory(tmpRoot);
            try
            {
                progress?.Invoke(0.03, "Downloading update...");
                using (var resp = await _http.GetAsync(plan.ZipUrl, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
                {
                    resp.EnsureSuccessStatusCode();
                    long? total = resp.Content.Headers.ContentLength;
                    using (var fs = new FileStream(zipPath, FileMode.Create, FileAccess.Write, FileShare.None))
                    using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false))
                    {
                        var buf = new byte[81920];
                        long read = 0; int n;
                        while ((n = await stream.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
                        {
                            await fs.WriteAsync(buf, 0, n, ct).ConfigureAwait(false);
                            read += n;
                            if (total.HasValue && total.Value > 0)
                                progress?.Invoke(0.03 + 0.62 * ((double)read / total.Value),
                                                 $"Downloading update... {read / (1024 * 1024)}/{total.Value / (1024 * 1024)} MB");
                        }
                    }
                }

                // Extract ONLY the files named in the (hash-checked) manifest, each
                // to a validated path inside extractDir — never a blanket
                // ExtractToDirectory — so a malicious "../" zip entry can't write
                // outside our temp dir (zip-slip). Verify hashes before touching
                // the install dir (all-or-nothing).
                progress?.Invoke(0.68, "Extracting + verifying...");
                var staged = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                using (var archive = ZipFile.OpenRead(zipPath))
                {
                    foreach (var entry in plan.Changed)
                    {
                        var ze = FindZipEntry(archive, entry.Path);
                        if (ze == null)
                            throw new Exception($"{entry.Path} is missing from the update zip. Aborting.");

                        string tmpFile;
                        if (!TryResolveInstallPath(extractDir, entry.Path, out tmpFile))
                            throw new Exception($"Update zip entry '{entry.Path}' escapes the staging directory. Aborting.");

                        var tmpDir = Path.GetDirectoryName(tmpFile);
                        if (!string.IsNullOrEmpty(tmpDir) && !Directory.Exists(tmpDir)) Directory.CreateDirectory(tmpDir);
                        ze.ExtractToFile(tmpFile, true);

                        if (!string.IsNullOrEmpty(entry.Sha256) && Sha256Of(tmpFile) != entry.Sha256.ToLowerInvariant())
                            throw new Exception($"Hash mismatch for {entry.Path} in the zip. Aborting (no files replaced).");

                        staged[entry.Path] = tmpFile;
                    }
                }

                progress?.Invoke(0.92, "Installing files...");
                foreach (var entry in plan.Changed)
                {
                    string dst;
                    if (!TryResolveInstallPath(installDir, entry.Path, out dst))
                        throw new Exception($"Refusing to write '{entry.Path}' — it escapes the install directory. Aborting.");

                    if (IsUserDataPath(entry.Path) && File.Exists(dst)) continue;

                    var dstDir = Path.GetDirectoryName(dst);
                    if (!string.IsNullOrEmpty(dstDir) && !Directory.Exists(dstDir)) Directory.CreateDirectory(dstDir);

                    ReplaceFile(staged[entry.Path], dst);
                }

                progress?.Invoke(1.0, $"Updated to {plan.RemoteVersion}");
            }
            finally
            {
                try { Directory.Delete(tmpRoot, true); } catch { }
            }
        }

        // -- Helpers ----------------------------------------------------------

        public static bool IsLauncherFile(string relPath)
        {
            return relPath != null &&
                   relPath.Replace('\\', '/').Equals(LauncherFileName, StringComparison.OrdinalIgnoreCase);
        }

        // config.cfg and the save folder are user data: never counted as updates
        // and never overwritten on apply.
        private static bool IsUserDataPath(string relPath)
        {
            var p = (relPath ?? "").Replace('\\', '/');
            return p.Equals("config.cfg", StringComparison.OrdinalIgnoreCase)
                || p.StartsWith("gamedata/save/", StringComparison.OrdinalIgnoreCase);
        }

        private static ZipArchiveEntry FindZipEntry(ZipArchive archive, string relPath)
        {
            string want = NormalizeRel(relPath);
            return archive.Entries.FirstOrDefault(z =>
                string.Equals(NormalizeRel(z.FullName), want, StringComparison.OrdinalIgnoreCase));
        }

        private static string NormalizeRel(string p)
        {
            return (p ?? "").Replace('\\', '/').TrimStart('/');
        }

        /// <summary>
        /// Resolve relPath under baseDir and confirm it stays inside it. Rejects
        /// absolute paths and "../" traversal (zip-slip / manifest path-traversal):
        /// a manifest/zip can only ever write inside the install (or staging) dir.
        /// false = unsafe; caller skips or aborts.
        /// </summary>
        private static bool TryResolveInstallPath(string baseDir, string relPath, out string fullPath)
        {
            fullPath = null;
            if (string.IsNullOrEmpty(relPath)) return false;
            try
            {
                string baseFull = Path.GetFullPath(baseDir);
                string combined = Path.GetFullPath(Path.Combine(baseFull, relPath.Replace('/', Path.DirectorySeparatorChar)));
                string baseWithSep = baseFull.EndsWith(Path.DirectorySeparatorChar.ToString())
                    ? baseFull : baseFull + Path.DirectorySeparatorChar;
                if (!combined.StartsWith(baseWithSep, StringComparison.OrdinalIgnoreCase)) return false;
                fullPath = combined;
                return true;
            }
            catch { return false; }
        }

        /// <summary>
        /// Copy with a locked-target fallback. A running exe (the launcher
        /// updating itself) can't be overwritten, but Windows DOES allow renaming
        /// it — so move the live file aside to *.old and copy the fresh one in.
        /// The new exe is picked up on next launch; stale *.old files are deleted
        /// at launcher startup.
        /// </summary>
        private static void ReplaceFile(string src, string dst)
        {
            try
            {
                File.Copy(src, dst, overwrite: true);
            }
            catch (Exception) when (File.Exists(dst))
            {
                string old = dst + ".old";
                try { if (File.Exists(old)) File.Delete(old); }
                catch { old = dst + "." + DateTime.UtcNow.Ticks + ".old"; }
                File.Move(dst, old);
                File.Copy(src, dst, overwrite: true);
            }
        }

        private static async Task<Manifest> FetchManifestAsync(string url, CancellationToken ct)
        {
            using (var resp = await _http.GetAsync(url, ct).ConfigureAwait(false))
            {
                resp.EnsureSuccessStatusCode();
                var bytes = await resp.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
                return DeserializeJson<Manifest>(bytes);
            }
        }

        /// <summary>Resolve + fetch the changelog text for the configured
        /// repo/branch/build, to preview a build before installing it.</summary>
        public static async Task<string> GetChangelogTextAsync(LauncherSettings settings, CancellationToken ct = default(CancellationToken))
        {
            string owner, repo;
            if (!settings.TryGetOwnerRepo(out owner, out repo))
                throw new Exception("Invalid repo URL: " + settings.RepoUrl);
            var src = await ResolveManifestSourceAsync(owner, repo, settings, ct).ConfigureAwait(false);
            return await FetchTextAsync(DeriveChangelogUrl(src.Url), ct).ConfigureAwait(false);
        }

        /// <summary>Fetch a CHANGELOG.md asset's text (raw, BOM-stripped).</summary>
        public static async Task<string> FetchTextAsync(string url, CancellationToken ct = default(CancellationToken))
        {
            if (string.IsNullOrWhiteSpace(url)) return "";
            using (var resp = await _http.GetAsync(url, ct).ConfigureAwait(false))
            {
                resp.EnsureSuccessStatusCode();
                var bytes = await resp.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
                int start = 0;
                if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;
                return Encoding.UTF8.GetString(bytes, start, bytes.Length - start);
            }
        }

        // The changelog ships as a CHANGELOG.md asset next to version.json in the
        // same release, so swap the filename on the resolved manifest URL.
        private static string DeriveChangelogUrl(string manifestUrl)
        {
            if (string.IsNullOrEmpty(manifestUrl)) return manifestUrl;
            int i = manifestUrl.LastIndexOf("version.json", StringComparison.OrdinalIgnoreCase);
            return i >= 0 ? manifestUrl.Substring(0, i) + "CHANGELOG.md" : manifestUrl;
        }

        private static async Task<T> GetApiJsonAsync<T>(string url, CancellationToken ct)
        {
            using (var req = new HttpRequestMessage(HttpMethod.Get, url))
            {
                req.Headers.Add("Accept", "application/vnd.github+json");
                using (var resp = await _http.SendAsync(req, ct).ConfigureAwait(false))
                {
                    resp.EnsureSuccessStatusCode();
                    var bytes = await resp.Content.ReadAsByteArrayAsync().ConfigureAwait(false);
                    return DeserializeJson<T>(bytes);
                }
            }
        }

        private static T DeserializeJson<T>(byte[] bytes)
        {
            int start = 0;
            if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;
            var ser = new DataContractJsonSerializer(typeof(T));
            using (var ms = new MemoryStream(bytes, start, bytes.Length - start))
                return (T)ser.ReadObject(ms);
        }

        private static string AssetUrl(GhRelease r, string name)
        {
            if (r == null || r.Assets == null) return null;
            var a = r.Assets.FirstOrDefault(x => x != null && string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
            return a?.BrowserDownloadUrl;
        }

        // Parse the YYYY.MM.DD.N version embedded in a release tag (v2026.06.24.1
        // or beta-2026.06.24.2) so releases can be ordered by VERSION rather than
        // the nightly repo's useless (all-identical) created_at timestamps.
        private static Version VersionFromTag(string tag)
        {
            string t = (tag ?? "").Trim();
            if (t.StartsWith("beta-", StringComparison.OrdinalIgnoreCase)) t = t.Substring(5);
            else if (t.StartsWith("v", StringComparison.OrdinalIgnoreCase)) t = t.Substring(1);
            Version v;
            return Version.TryParse(t, out v) ? v : new Version(0, 0, 0, 0);
        }

        private static DateTime ParseDate(string s)
        {
            DateTime d;
            if (!string.IsNullOrEmpty(s) &&
                DateTime.TryParse(s, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal, out d))
                return d;
            return DateTime.MinValue;
        }

        private static string Sha256Of(string path)
        {
            using (var sha = SHA256.Create())
            using (var fs  = File.OpenRead(path))
            {
                var hash = sha.ComputeHash(fs);
                var sb = new StringBuilder(hash.Length * 2);
                foreach (var b in hash) sb.Append(b.ToString("x2"));
                return sb.ToString();
            }
        }
    }
}
