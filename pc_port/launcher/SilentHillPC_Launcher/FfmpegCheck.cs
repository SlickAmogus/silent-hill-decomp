using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The game loads ffmpeg at runtime (LoadLibrary by soname) to play
    /// mp4/mkv/webm/mov overrides out of gamedata/FMV. Nothing is bundled or
    /// linked, which keeps the LGPL/GPL redistribution duty off this project —
    /// but it means the user has to supply a runtime whose ABI generation the
    /// decoder was measured against, because fmv_player.cpp dereferences public
    /// libav structs.
    ///
    /// Get that wrong and the only symptom is one line in SilentHill.log and an
    /// FMV mod that silently does nothing. This class makes the requirement
    /// visible: it reports which of the three states the machine is in
    /// (<see cref="FfmpegState"/>), and can fetch a correct build on request.
    /// </summary>
    public enum FfmpegState
    {
        /// <summary>A complete, accepted ffmpeg generation resolves.</summary>
        Ok,
        /// <summary>No usable ffmpeg in the game folder or on PATH.</summary>
        Missing,
        /// <summary>ffmpeg is present, but no generation this build accepts.</summary>
        WrongVersion,
    }

    /// <summary>Result of <see cref="FfmpegCheck.Probe"/>. Never throws; worst case is Missing.</summary>
    public sealed class FfmpegStatus
    {
        public FfmpegState State;

        /// <summary>Accepted release that resolved, or the foreign one found. 0 = nothing.</summary>
        public int FoundRelease;

        /// <summary>How the found version is named to the user ("5", or "avcodec 63").</summary>
        public string FoundLabel;

        /// <summary>
        /// True when the problem is a half-installed or MIXED set (e.g. avcodec-62
        /// beside avutil-59) rather than a wholly unsupported ffmpeg. The five majors
        /// ship as a set, so a mix is never usable — but the fix is different, and the
        /// user has to be able to tell the two apart.
        /// </summary>
        public bool Incomplete;

        /// <summary>Generation the incomplete set came closest to.</summary>
        public int IncompleteRelease;

        /// <summary>Release the Download button would fetch.</summary>
        public int InstallRelease;

        /// <summary>Files of the closest incomplete generation, for diagnostics.</summary>
        public readonly List<string> MissingFiles = new List<string>();

        /// <summary>Files that satisfied an accepted generation (full paths).</summary>
        public readonly List<string> Resolved = new List<string>();

        public bool Actionable { get { return State != FfmpegState.Ok; } }

        /// <summary>
        /// The ONE LINE shown to the user. Deliberately a single sentence — a wall of
        /// explanatory text here confuses far more than it helps.
        /// </summary>
        public string Line
        {
            get
            {
                switch (State)
                {
                    case FfmpegState.Ok:
                        return "FFmpeg " + FoundRelease + ": ready.";
                    case FfmpegState.WrongVersion:
                        if (Incomplete)
                            return "FMV replacements need a complete ffmpeg " + IncompleteRelease + " — " +
                                   (MissingFiles.Count == 1
                                        ? MissingFiles[0] + " is missing."
                                        : MissingFiles.Count + " of 5 files are missing.");
                        return "FMV replacements need ffmpeg " + FfmpegCheck.AcceptedSpanText +
                               " (found: " + FoundLabel + ").";
                    default:
                        return "FMV replacements need ffmpeg " + InstallRelease + ".";
                }
            }
        }
    }

    /// <summary>The five soname majors that make up one ffmpeg generation.</summary>
    public struct FfmpegAbi
    {
        public int Release;    // marketing version: 6, 7, 8
        public int AvCodec, AvFormat, AvUtil, SwScale, SwResample;

        public FfmpegAbi(int release, int avcodec, int avformat, int avutil, int swscale, int swresample)
        {
            Release = release; AvCodec = avcodec; AvFormat = avformat;
            AvUtil = avutil; SwScale = swscale; SwResample = swresample;
        }

        public bool IsEmpty { get { return AvCodec == 0 && AvFormat == 0 && AvUtil == 0; } }

        /// <summary>Bare DLL names in the fixed order avcodec, avformat, avutil, swscale, swresample.</summary>
        public string[] FileNames
        {
            get
            {
                return new[]
                {
                    "avcodec-"    + AvCodec    + ".dll",
                    "avformat-"   + AvFormat   + ".dll",
                    "avutil-"     + AvUtil     + ".dll",
                    "swscale-"    + SwScale    + ".dll",
                    "swresample-" + SwResample + ".dll",
                };
            }
        }

        public int[] Majors
        {
            get { return new[] { AvCodec, AvFormat, AvUtil, SwScale, SwResample }; }
        }
    }

    public static class FfmpegCheck
    {
        // ====================================================================
        // ===================  VERSION SOURCE OF TRUTH  ======================
        //
        //  EDIT HERE and nowhere else when the accepted ffmpeg set changes.
        //
        //  This MUST stay in sync with the kFfmpegGens table in
        //      pc_port/src/fmv/fmv_player.cpp
        //  which is authoritative: each row there was validated by measuring
        //  offsetof against that generation's real headers, so a row exists only
        //  for a generation the decoder has actually been checked against.
        //  Adding a generation here without adding it there (or vice versa)
        //  reintroduces exactly the silent-failure bug this class exists to
        //  prevent, so change both together.
        //
        //  THREE LOCKED SETS, NOT PER-LIBRARY RANGES. A row is matched WHOLE:
        //  avcodec-62 always implies avutil-60, and a set mixed across rows is
        //  invalid, not "partially OK" — it is reported as a wrong version.
        //  Newest is preferred, matching the game's own order. 8.1 shares 8.0's
        //  sonames and so is covered by the 8 row.
        // ====================================================================

        private static readonly FfmpegAbi[] s_abiTable =
        {
            //             release  avcodec  avformat  avutil  swscale  swresample
            new FfmpegAbi(     8,       62,       62,      60,       9,          6 ),
            new FfmpegAbi(     7,       61,       61,      59,       8,          5 ),
            new FfmpegAbi(     6,       60,       60,      58,       7,          4 ),
        };

        // ====================  end version source of truth  =================

        /// <summary>Newest accepted release — what a fresh install gets offered.</summary>
        public static int PreferredRelease { get { return s_abiTable[0].Release; } }

        public static int AcceptedReleaseMin { get { return s_abiTable.Min(a => a.Release); } }
        public static int AcceptedReleaseMax { get { return s_abiTable.Max(a => a.Release); } }

        /// <summary>"8" when only one generation is accepted, "6-8" for a span.</summary>
        public static string AcceptedSpanText
        {
            get
            {
                return AcceptedReleaseMin == AcceptedReleaseMax
                    ? AcceptedReleaseMax.ToString()
                    : AcceptedReleaseMin + "-" + AcceptedReleaseMax;
            }
        }

        /// <summary>Base names in the same order as <see cref="FfmpegAbi.Majors"/>.</summary>
        private static readonly string[] s_libBases =
        {
            "avcodec", "avformat", "avutil", "swscale", "swresample"
        };

        private static readonly string[] s_containerPatterns = { "*.mp4", "*.mkv", "*.webm", "*.mov" };

        private const string GameExeName   = "SilentHillPC.exe";
        private const string BackupDirName = "ffmpeg-backup";

        // BtbN publishes explicit win64 + lgpl + shared archives pinned to a release
        // branch (ffmpeg-nX.Y-latest-win64-lgpl-shared-X.Y.zip). gyan.dev is GPLv3
        // and offers no LGPL variant, so it is not an option for us.
        private const string BtbNReleasesApi = "https://api.github.com/repos/BtbN/FFmpeg-Builds/releases";
        private const string BtbNLatestApi   = BtbNReleasesApi + "/tags/latest";
        private const string BtbNHumanUrl    = "https://github.com/BtbN/FFmpeg-Builds/releases";

        private static readonly HttpClient s_http = CreateHttpClient();
        private static HttpClient CreateHttpClient()
        {
            // github.com refuses anything below TLS 1.2. Don't inherit whatever the
            // process-wide default happens to be — under .NET Framework quirks that
            // can still be SSL3/TLS1.0, and the only symptom is an opaque
            // "An error occurred while sending the request."
            try
            {
                System.Net.ServicePointManager.SecurityProtocol |=
                    System.Net.SecurityProtocolType.Tls12;
            }
            catch { /* newer runtimes reject the assignment; their default is already fine */ }

            var c = new HttpClient();
            c.DefaultRequestHeaders.Add("User-Agent", "SilentHillPC-Launcher");
            c.Timeout = TimeSpan.FromMinutes(10);
            return c;
        }

        // -- Version table helpers -------------------------------------------

        public static FfmpegAbi AbiForRelease(int release)
        {
            foreach (var a in s_abiTable)
                if (a.Release == release) return a;
            return default(FfmpegAbi);
        }

        /// <summary>Accepted releases, newest first — the game's own preference order.</summary>
        public static IEnumerable<FfmpegAbi> AcceptedGenerations
        {
            get { return s_abiTable.OrderByDescending(a => a.Release); }
        }

        private static bool IsAcceptedAvcodecMajor(int major)
        {
            return s_abiTable.Any(a => a.AvCodec == major);
        }

        private static int ReleaseForAvcodecMajor(int major)
        {
            foreach (var a in s_abiTable)
                if (a.AvCodec == major) return a.Release;
            return 0;
        }

        /// <summary>
        /// Names an ffmpeg we do NOT accept, for the message only — "5" reads far
        /// better than the raw soname major 59. Anything off the end of this list
        /// is reported as the soname rather than guessed at.
        /// </summary>
        private static string DescribeAvcodecMajor(int major)
        {
            int rel = ReleaseForAvcodecMajor(major);
            if (rel != 0) return rel.ToString();

            // avcodec major has tracked release+54 since ffmpeg 3.
            int guess = major - 54;
            return guess >= 2 && guess <= 20 ? guess.ToString() : "avcodec " + major;
        }

        // -- Probing ---------------------------------------------------------

        /// <summary>
        /// True when gamedata/FMV holds at least one modern-container movie, i.e.
        /// an override only the ffmpeg path can play. Missing folder = false.
        /// </summary>
        public static bool HasModernFmvOverride(string gameRoot)
        {
            try
            {
                string fmvDir = Path.Combine(Path.Combine(gameRoot, "gamedata"), "FMV");
                if (!Directory.Exists(fmvDir)) return false;

                foreach (var pattern in s_containerPatterns)
                    if (Directory.GetFiles(fmvDir, pattern, SearchOption.AllDirectories).Length > 0)
                        return true;
            }
            catch { /* unreadable gamedata — treat as "no mod", never block */ }
            return false;
        }

        /// <summary>
        /// Works out which of the three states this machine is in. Mirrors the loader:
        /// each accepted generation is tried whole, newest first, looking in the exe's
        /// folder and then PATH; both the bare and 'lib'-prefixed spelling count.
        /// </summary>
        public static FfmpegStatus Probe(string gameRoot)
        {
            var st = new FfmpegStatus { InstallRelease = PreferredRelease };

            try
            {
                var dirs = SearchDirs(gameRoot);

                int bestPartial = 0;

                foreach (var abi in AcceptedGenerations)
                {
                    var names  = abi.FileNames;
                    var majors = abi.Majors;

                    var resolved = new List<string>();
                    var missing  = new List<string>();

                    for (int i = 0; i < names.Length; i++)
                    {
                        string hit = ResolveLib(dirs, s_libBases[i], majors[i]);
                        if (hit != null) resolved.Add(hit);
                        else             missing.Add(names[i]);
                    }

                    // A row only counts when it matches WHOLE: the five majors ship as a
                    // set, so handing avcodec-62 structs to avutil-59 code is never valid.
                    if (missing.Count == 0)
                    {
                        st.State        = FfmpegState.Ok;
                        st.FoundRelease = abi.Release;
                        st.FoundLabel   = abi.Release.ToString();
                        st.Resolved.AddRange(resolved);
                        return st;
                    }

                    // Remember the row that came closest, so a half-installed or mixed
                    // set can be reported as such instead of as "nothing installed".
                    // Newest wins ties because we walk newest-first.
                    if (resolved.Count > bestPartial)
                    {
                        bestPartial = resolved.Count;
                        st.IncompleteRelease = abi.Release;
                        st.MissingFiles.Clear();
                        st.MissingFiles.AddRange(missing);
                    }
                }

                // No accepted generation is complete. Distinguish "an ffmpeg we don't
                // support" from "a supported one that is only partly here".
                int foreign = FindForeignAvcodecMajor(dirs);
                if (foreign > 0)
                {
                    st.State        = FfmpegState.WrongVersion;
                    st.FoundRelease = ReleaseForAvcodecMajor(foreign);
                    st.FoundLabel   = DescribeAvcodecMajor(foreign);
                }
                else if (bestPartial > 0)
                {
                    st.State      = FfmpegState.WrongVersion;
                    st.Incomplete = true;
                    st.FoundLabel = "incomplete ffmpeg " + st.IncompleteRelease;
                }
                else
                {
                    st.State = FfmpegState.Missing;
                    st.MissingFiles.Clear();
                    st.MissingFiles.AddRange(AbiForRelease(PreferredRelease).FileNames);
                }
            }
            catch
            {
                st.State = FfmpegState.Missing;
            }

            return st;
        }

        /// <summary>
        /// Full path of a library at the exact major, or null. The Win32 version
        /// resource is checked too, so a file someone renamed to look like the
        /// version we want cannot pass — that would load and then crash the game.
        /// </summary>
        private static string ResolveLib(List<string> dirs, string baseName, int major)
        {
            string bare = baseName + "-" + major + ".dll";
            string libd = "lib" + bare;

            foreach (var dir in dirs)
            {
                foreach (var candidate in new[] { bare, libd })
                {
                    string full;
                    try { full = Path.Combine(dir, candidate); }
                    catch { continue; } // bad chars in a PATH entry
                    if (!FileExistsSafe(full)) continue;
                    if (VersionMajorMatches(full, major)) return full;
                }
            }
            return null;
        }

        /// <summary>
        /// False only when the file carries a version resource that disagrees.
        /// A build with no resource at all is accepted — some minimal builds ship
        /// none, and rejecting those would be worse than trusting the soname.
        /// </summary>
        private static bool VersionMajorMatches(string path, int major)
        {
            try
            {
                var vi = FileVersionInfo.GetVersionInfo(path);
                if (vi == null) return true;
                if (vi.FileMajorPart == 0 && vi.ProductMajorPart == 0) return true; // no resource
                if (vi.FileMajorPart != 0) return vi.FileMajorPart == major;
                return vi.ProductMajorPart == major;
            }
            catch { return true; }
        }

        /// <summary>Major of any avcodec present that no accepted generation uses, else 0.</summary>
        private static int FindForeignAvcodecMajor(List<string> dirs)
        {
            foreach (var dir in dirs)
            {
                try
                {
                    if (!Directory.Exists(dir)) continue;

                    foreach (var pattern in new[] { "avcodec-*.dll", "libavcodec-*.dll" })
                    {
                        foreach (var f in Directory.GetFiles(dir, pattern, SearchOption.TopDirectoryOnly))
                        {
                            int m = ParseMajorFromFileName(Path.GetFileName(f));
                            if (m > 0 && !IsAcceptedAvcodecMajor(m)) return m;
                        }
                    }
                }
                catch { /* unreachable share, denied dir — keep looking */ }
            }
            return 0;
        }

        private static int ParseMajorFromFileName(string name)
        {
            var m = Regex.Match(name, @"-(\d+)\.dll$", RegexOptions.IgnoreCase);
            return m.Success ? int.Parse(m.Groups[1].Value) : 0;
        }

        private static bool FileExistsSafe(string full)
        {
            try { return File.Exists(full); }
            catch { return false; }
        }

        private static List<string> SearchDirs(string gameRoot)
        {
            var dirs = new List<string>();
            if (!string.IsNullOrEmpty(gameRoot)) dirs.Add(gameRoot);

            string path = "";
            try { path = Environment.GetEnvironmentVariable("PATH") ?? ""; }
            catch { }

            foreach (var entry in path.Split(Path.PathSeparator))
            {
                string d = entry.Trim().Trim('"');
                if (d.Length > 0) dirs.Add(d);
            }
            return dirs;
        }

        // -- Download --------------------------------------------------------

        /// <summary>
        /// Fetches a Windows x64 LGPL *shared* ffmpeg and drops the five DLLs next to
        /// the game exe. Tries the accepted generations newest-first, so it still
        /// works when the newest has not been published (or has been retired).
        /// Anything it would overwrite is moved to ffmpeg-backup\ first.
        /// Throws with a user-readable message on failure.
        /// </summary>
        public static int DownloadAndInstall(string gameRoot, Action<int, int, string> report,
                                             out int installedRelease)
        {
            if (report == null) report = delegate { };

            report(0, 0, "Looking for an LGPL shared build of ffmpeg…");

            FfmpegAbi abi;
            string url = FindLgplSharedAssetUrl(out abi);
            installedRelease = abi.Release;

            return FetchAndInstall(gameRoot, url, abi, report);
        }

        /// <summary>Same, pinned to one release — used by tests and by an explicit choice.</summary>
        public static int DownloadAndInstall(string gameRoot, int release, FfmpegAbi abi,
                                             Action<int, int, string> report)
        {
            if (report == null) report = delegate { };

            report(0, 0, "Looking for an LGPL shared build of ffmpeg " + release + "…");
            string url = FindAssetForRelease(release);
            if (url == null)
                throw new Exception("No LGPL shared build of ffmpeg " + release +
                                    " is published any more.\n\n" + ManualAdvice(abi));

            return FetchAndInstall(gameRoot, url, abi, report);
        }

        private static int FetchAndInstall(string gameRoot, string url, FfmpegAbi abi,
                                           Action<int, int, string> report)
        {
            string tmp = Path.Combine(Path.GetTempPath(),
                                      "shpc-ffmpeg-" + Guid.NewGuid().ToString("N") + ".zip");
            try
            {
                DownloadFile(url, tmp, report);

                report(0, 0, "Verifying archive…");
                var payload = ReadDllsFromZip(tmp, abi);

                if (payload.Count != s_libBases.Length)
                    throw new Exception("The downloaded archive did not contain the expected " +
                                        s_libBases.Length + " libraries.");

                report(0, 0, "Installing…");
                return InstallDlls(gameRoot, payload, abi);
            }
            finally
            {
                try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
            }
        }

        private static string ManualAdvice(FfmpegAbi abi)
        {
            return "Install it manually: download a win64 LGPL shared archive and copy " +
                   string.Join(", ", abi.FileNames) + " next to " + GameExeName + ".\n" + BtbNHumanUrl;
        }

        /// <summary>
        /// Picks the newest accepted generation BtbN actually publishes. Always asks for
        /// a SPECIFIC release branch rather than "latest": a "latest" URL eventually
        /// rolls past the generations this build accepts and silently reintroduces the
        /// very mismatch we are fixing.
        /// </summary>
        private static string FindLgplSharedAssetUrl(out FfmpegAbi chosen)
        {
            // A dead network and "this version is no longer published" need different
            // advice, so never let a transport error masquerade as the latter.
            Exception transport = null;
            bool reachedServer = false;

            var urlsBySource = new List<List<string>>();
            foreach (var api in ApiSources())
            {
                try { urlsBySource.Add(AssetUrls(api)); reachedServer = true; }
                catch (Exception ex) { transport = ex; }
            }

            foreach (var abi in AcceptedGenerations)
            {
                var rx = AssetRegex(abi.Release);
                foreach (var urls in urlsBySource)
                {
                    var hit = urls.FirstOrDefault(u => rx.IsMatch(u));
                    if (hit != null) { chosen = abi; return hit; }
                }
            }

            chosen = AbiForRelease(PreferredRelease);

            if (!reachedServer)
                throw new Exception("Could not reach the ffmpeg build server" +
                                    (transport != null ? " (" + transport.Message + ")" : "") +
                                    ".\n\n" + ManualAdvice(chosen));

            throw new Exception("No LGPL shared build of ffmpeg " + AcceptedSpanText +
                                " is published any more.\n\n" + ManualAdvice(chosen));
        }

        private static string FindAssetForRelease(int release)
        {
            var rx = AssetRegex(release);
            foreach (var api in ApiSources())
            {
                try
                {
                    var hit = AssetUrls(api).FirstOrDefault(u => rx.IsMatch(u));
                    if (hit != null) return hit;
                }
                catch { }
            }
            return null;
        }

        private static IEnumerable<string> ApiSources()
        {
            yield return BtbNLatestApi;
            for (int page = 1; page <= 3; page++)
                yield return BtbNReleasesApi + "?per_page=100&page=" + page;
        }

        // ffmpeg-n<release>.<minor>-latest-win64-lgpl-shared-<release>.<minor>.zip
        private static Regex AssetRegex(int release)
        {
            return new Regex(@"/ffmpeg-n" + release + @"\.[0-9]+-[^/""]*win64-lgpl-shared[^/""]*\.zip$",
                             RegexOptions.IgnoreCase);
        }

        private static List<string> AssetUrls(string apiUrl)
        {
            string json = s_http.GetStringAsync(apiUrl).GetAwaiter().GetResult();

            var list = new List<string>();
            foreach (Match m in Regex.Matches(json, @"""browser_download_url""\s*:\s*""([^""]+)"""))
                list.Add(m.Groups[1].Value);
            return list;
        }

        private static void DownloadFile(string url, string destPath, Action<int, int, string> report)
        {
            using (var resp = s_http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead)
                                    .GetAwaiter().GetResult())
            {
                resp.EnsureSuccessStatusCode();

                long total  = resp.Content.Headers.ContentLength ?? 0;
                int totalKb = (int)(total / 1024);

                using (var src = resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
                using (var dst = new FileStream(destPath, FileMode.Create, FileAccess.Write))
                {
                    var buf = new byte[81920];
                    long done = 0;
                    int n, tick = 0;

                    while ((n = src.Read(buf, 0, buf.Length)) > 0)
                    {
                        dst.Write(buf, 0, n);
                        done += n;

                        if (++tick % 8 == 0)
                        {
                            int doneKb = (int)(done / 1024);
                            report(doneKb, totalKb,
                                   totalKb > 0
                                       ? "Downloading ffmpeg… " + (doneKb / 1024) + " / " + (totalKb / 1024) + " MB"
                                       : "Downloading ffmpeg… " + (doneKb / 1024) + " MB");
                        }
                    }
                }
            }
        }

        /// <summary>
        /// Opens the archive (which also proves it is a well-formed zip) and pulls the
        /// five DLLs out of its bin\ folder into memory.
        /// </summary>
        private static Dictionary<string, byte[]> ReadDllsFromZip(string zipPath, FfmpegAbi wanted)
        {
            var want = new HashSet<string>(wanted.FileNames, StringComparer.OrdinalIgnoreCase);
            var got  = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);

            using (var zip = ZipFile.OpenRead(zipPath))
            {
                foreach (var e in zip.Entries)
                {
                    if (string.IsNullOrEmpty(e.Name)) continue;
                    if (!want.Contains(e.Name)) continue;
                    if (got.ContainsKey(e.Name)) continue;

                    using (var s = e.Open())
                    using (var ms = new MemoryStream())
                    {
                        s.CopyTo(ms);
                        got[e.Name] = ms.ToArray();
                    }
                }
            }
            return got;
        }

        /// <summary>
        /// Writes the DLLs into the game folder, moving anything of the same name into
        /// ffmpeg-backup\ first. Each file is verified against its version resource
        /// after landing; a mismatch rolls the whole install back.
        /// </summary>
        private static int InstallDlls(string gameRoot, Dictionary<string, byte[]> payload, FfmpegAbi wanted)
        {
            string backupDir = Path.Combine(gameRoot, BackupDirName);
            var written  = new List<string>();
            var backedUp = new List<KeyValuePair<string, string>>(); // backup path -> original path

            try
            {
                Directory.CreateDirectory(gameRoot);

                foreach (var kv in payload)
                {
                    string dest = Path.Combine(gameRoot, kv.Key);

                    if (File.Exists(dest))
                    {
                        Directory.CreateDirectory(backupDir);
                        string bak = Path.Combine(backupDir, kv.Key);
                        if (File.Exists(bak)) File.Delete(bak);
                        File.Move(dest, bak);
                        backedUp.Add(new KeyValuePair<string, string>(bak, dest));
                    }

                    File.WriteAllBytes(dest, kv.Value);
                    written.Add(dest);
                }

                var majors = wanted.Majors;
                var names  = wanted.FileNames;
                for (int i = 0; i < names.Length; i++)
                {
                    string dest = Path.Combine(gameRoot, names[i]);
                    if (!File.Exists(dest))
                        throw new Exception(names[i] + " was not installed.");
                    if (!VersionMajorMatches(dest, majors[i]))
                        throw new Exception(names[i] + " is not really version " + majors[i] + ".");
                }

                return backedUp.Count;
            }
            catch
            {
                foreach (var f in written) { try { File.Delete(f); } catch { } }
                foreach (var b in backedUp) { try { File.Move(b.Key, b.Value); } catch { } }
                throw;
            }
        }

        // -- UI --------------------------------------------------------------

        /// <summary>
        /// Runs the whole detect → offer → download → recheck flow against
        /// <paramref name="owner"/>. Returns true if ffmpeg ended up correct.
        /// </summary>
        public static bool PromptAndFix(IWin32Window owner, string gameRoot)
        {
            var st = Probe(gameRoot);
            if (st.State == FfmpegState.Ok) return true;

            if (MessageBox.Show(owner, st.Line + " Download it now?", "FMV support",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                return false;

            int backed = 0, release = 0;
            try
            {
                ProgressDialog.Run(owner, "FFmpeg",
                    report => backed = DownloadAndInstall(gameRoot, report, out release));
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, ex.Message, "FFmpeg",
                                MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }

            var after = Probe(gameRoot);
            MessageBox.Show(owner,
                after.State == FfmpegState.Ok
                    ? "FFmpeg " + after.FoundRelease + " installed." +
                      (backed > 0 ? " Your previous DLLs are in " + BackupDirName + "\\." : "")
                    : "Installed, but ffmpeg still does not check out — see " + BtbNHumanUrl,
                "FFmpeg", MessageBoxButtons.OK, MessageBoxIcon.Information);

            return after.State == FfmpegState.Ok;
        }

        /// <summary>
        /// Startup notice: only speaks up when a modern-container FMV mod is actually
        /// installed and ffmpeg would not play it. Advisory — never blocks Play.
        /// </summary>
        public static void WarnIfNeeded(IWin32Window owner, string gameRoot)
        {
            try
            {
                if (!HasModernFmvOverride(gameRoot)) return;
                PromptAndFix(owner, gameRoot);
            }
            catch { /* advisory only — never keep the user from launching */ }
        }
    }

    /// <summary>
    /// One-line ffmpeg status with a Download button, for the Mod Manager — where
    /// FMV mods are managed and therefore where the requirement has to be visible.
    /// Shows nothing but a short "ready" when a good version is already there.
    /// </summary>
    public sealed class FfmpegStatusRow : Panel
    {
        private readonly string _gameRoot;
        private readonly Label  _label;
        private readonly Button _btn;

        public FfmpegStatusRow(string gameRoot)
        {
            _gameRoot = gameRoot;

            _label = new Label
            {
                Location     = new Point(0, 5),
                Size         = new Size(400, 18),
                AutoSize     = false,
                AutoEllipsis = true,
            };

            _btn = new Button
            {
                Text    = "Download",
                Size    = new Size(84, 26),
                Visible = false,
            };
            _btn.Click += (s, e) =>
            {
                FfmpegCheck.PromptAndFix(FindForm(), _gameRoot);
                RefreshStatus();
            };

            Controls.Add(_label);
            Controls.Add(_btn);

            Layout += (s, e) =>
            {
                _btn.Location = new Point(Math.Max(0, Width - _btn.Width), 0);
                _label.Size   = new Size(Math.Max(40, Width - _btn.Width - 8), 18);
            };

            RefreshStatus();
        }

        public void RefreshStatus()
        {
            try
            {
                var st = FfmpegCheck.Probe(_gameRoot);

                _label.Text      = st.Line;
                _label.ForeColor = st.State == FfmpegState.Ok ? SystemColors.GrayText : Color.Firebrick;
                _btn.Visible     = st.Actionable;
            }
            catch
            {
                _label.Text  = "";
                _btn.Visible = false;
            }
        }
    }
}
