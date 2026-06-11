using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Pulls version.json from the nightly-builds repo, compares per-file
    /// SHA-256 hashes against local files, and downloads only the changed
    /// ones. See tools/release-nightly.ps1 for the manifest format.
    ///
    /// Manifest URL is constructed from REPO_OWNER + REPO_NAME + the
    /// "releases/latest/download/version.json" GitHub stable redirect, so
    /// no per-release URL hardcoding needed.
    /// </summary>
    public static class UpdateChecker
    {
        // -- Configuration ----------------------------------------------------

        private const string RepoOwner    = "SlickAmogus";
        private const string RepoName     = "silent-hill-pc-nightly";
        private const string ManifestName = "version.json";

        private static string ManifestUrl =>
            $"https://github.com/{RepoOwner}/{RepoName}/releases/latest/download/{ManifestName}";

        // -- DTOs -------------------------------------------------------------

        [DataContract]
        public class Manifest
        {
            [DataMember(Name = "version")]    public string Version;
            [DataMember(Name = "tag")]        public string Tag;
            [DataMember(Name = "build_date")] public string BuildDate;
            [DataMember(Name = "files")]      public List<FileEntry> Files;
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
            public string  RemoteVersion;
            public string  BuildDate;
            public List<FileEntry> Changed = new List<FileEntry>();
            public bool    HasUpdate => Changed.Count > 0;
        }

        // -- HTTP -------------------------------------------------------------

        private static readonly HttpClient _http = CreateHttpClient();
        private static HttpClient CreateHttpClient()
        {
            var c = new HttpClient();
            c.DefaultRequestHeaders.Add("User-Agent", "SilentHillPC-Launcher");
            c.Timeout = TimeSpan.FromSeconds(30);
            return c;
        }

        // -- Public API -------------------------------------------------------

        /// <summary>
        /// Fetches the remote manifest and computes which local files differ.
        /// </summary>
        /// <param name="installDir">Directory containing SilentHillPC.exe.</param>
        public static async Task<UpdatePlan> CheckAsync(string installDir)
        {
            var manifest = await FetchManifestAsync().ConfigureAwait(false);
            var plan = new UpdatePlan
            {
                RemoteVersion = manifest.Version,
                BuildDate     = manifest.BuildDate,
            };

            foreach (var entry in manifest.Files)
            {
                var localPath = Path.Combine(installDir, entry.Path.Replace('/', Path.DirectorySeparatorChar));
                if (!File.Exists(localPath) || Sha256Of(localPath) != entry.Sha256.ToLowerInvariant())
                {
                    plan.Changed.Add(entry);
                }
            }
            return plan;
        }

        /// <summary>
        /// Downloads each changed file into a temp dir, verifies the hash,
        /// then atomically replaces the local copies. Reports progress via
        /// the callback (0..1 = fraction of total bytes downloaded).
        /// </summary>
        public static async Task ApplyAsync(string installDir, UpdatePlan plan, Action<double, string> progress)
        {
            if (!plan.HasUpdate) return;

            string tmpRoot = Path.Combine(Path.GetTempPath(), "sh-nightly-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(tmpRoot);

            try
            {
                // First pass: download all files into tmp, verify hashes.
                // We don't know each file's content-length until we start the
                // GET, so weight by file index for "approx" progress.
                int idx = 0;
                foreach (var entry in plan.Changed)
                {
                    progress?.Invoke((double)idx / plan.Changed.Count, $"Downloading {entry.Path}...");

                    string tmpFile = Path.Combine(tmpRoot, entry.Path.Replace('/', '_'));
                    using (var resp = await _http.GetAsync(entry.Url, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
                    {
                        resp.EnsureSuccessStatusCode();
                        using (var fs = new FileStream(tmpFile, FileMode.Create, FileAccess.Write, FileShare.None))
                        using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false))
                        {
                            await stream.CopyToAsync(fs).ConfigureAwait(false);
                        }
                    }

                    string actualHash = Sha256Of(tmpFile);
                    if (actualHash != entry.Sha256.ToLowerInvariant())
                    {
                        throw new Exception(
                            $"Hash mismatch for {entry.Path}: expected {entry.Sha256}, got {actualHash}. " +
                            "Download corrupted or manifest stale. Aborting (no files replaced).");
                    }
                    idx++;
                }

                // Second pass: all verified, now move into place.
                progress?.Invoke(0.95, "Installing files...");
                foreach (var entry in plan.Changed)
                {
                    string tmpFile = Path.Combine(tmpRoot, entry.Path.Replace('/', '_'));
                    string dst = Path.Combine(installDir, entry.Path.Replace('/', Path.DirectorySeparatorChar));

                    var dstDir = Path.GetDirectoryName(dst);
                    if (!string.IsNullOrEmpty(dstDir) && !Directory.Exists(dstDir))
                        Directory.CreateDirectory(dstDir);

                    // config.cfg: don't clobber user settings on update.
                    if (entry.Path.Equals("config.cfg", StringComparison.OrdinalIgnoreCase) && File.Exists(dst))
                    {
                        // Skip — let the user keep their existing config.
                        // Future enhancement: merge new keys from the remote default.
                        continue;
                    }

                    // File.Move fails across drives (temp dir vs install dir may differ).
                    // Copy then delete is safe on all configurations.
                    ReplaceFile(tmpFile, dst);
                }

                progress?.Invoke(1.0, $"Updated to {plan.RemoteVersion}");
            }
            finally
            {
                try { Directory.Delete(tmpRoot, true); } catch { /* best effort */ }
            }
        }

        /// <summary>
        /// Bootstrap helper: download ONE file by its manifest path (the
        /// launcher fetching SH1Updater.exe when it's missing). Verifies the
        /// hash before installing, like the full flow.
        /// </summary>
        public static async Task DownloadSingleFileAsync(string installDir, string manifestPath)
        {
            var manifest = await FetchManifestAsync().ConfigureAwait(false);

            FileEntry entry = null;
            foreach (var f in manifest.Files)
            {
                if (string.Equals(f.Path, manifestPath, StringComparison.OrdinalIgnoreCase))
                {
                    entry = f;
                    break;
                }
            }
            if (entry == null)
                throw new Exception($"{manifestPath} is not in the update manifest.");

            string tmpFile = Path.Combine(Path.GetTempPath(),
                "sh-boot-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".tmp");
            try
            {
                using (var resp = await _http.GetAsync(entry.Url, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
                {
                    resp.EnsureSuccessStatusCode();
                    using (var fs = new FileStream(tmpFile, FileMode.Create, FileAccess.Write, FileShare.None))
                    using (var stream = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false))
                    {
                        await stream.CopyToAsync(fs).ConfigureAwait(false);
                    }
                }

                string actualHash = Sha256Of(tmpFile);
                if (actualHash != entry.Sha256.ToLowerInvariant())
                    throw new Exception($"Hash mismatch for {manifestPath}: expected {entry.Sha256}, got {actualHash}.");

                string dst = Path.Combine(installDir, manifestPath.Replace('/', Path.DirectorySeparatorChar));
                ReplaceFile(tmpFile, dst);
            }
            finally
            {
                try { File.Delete(tmpFile); } catch { /* best effort */ }
            }
        }

        // -- Helpers ----------------------------------------------------------

        /// <summary>
        /// Copy with a locked-target fallback. A running exe (the launcher or
        /// SH1Updater updating itself) can't be overwritten, but Windows DOES
        /// allow renaming it — so move the live file aside to *.old and copy
        /// the fresh one into place. The new exe is picked up on next launch;
        /// stale *.old files are deleted on the updater's next startup.
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

        private static async Task<Manifest> FetchManifestAsync()
        {
            using (var resp = await _http.GetAsync(ManifestUrl).ConfigureAwait(false))
            {
                resp.EnsureSuccessStatusCode();
                var bytes = await resp.Content.ReadAsByteArrayAsync().ConfigureAwait(false);

                // Strip UTF-8 BOM if present (PS5.1 Set-Content -Encoding UTF8 adds one;
                // DataContractJsonSerializer doesn't skip it and chokes on the first byte).
                int start = 0;
                if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
                    start = 3;

                var ser = new DataContractJsonSerializer(typeof(Manifest));
                using (var ms = new MemoryStream(bytes, start, bytes.Length - start))
                {
                    return (Manifest)ser.ReadObject(ms);
                }
            }
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
