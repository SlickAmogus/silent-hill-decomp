using System;
using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Launcher-only settings, persisted in config.cfg under the "## Launcher"
    /// section as launcher_* keys. The game ignores unknown keys (pc_config.c),
    /// so the launcher and game share one config file.
    ///
    ///   launcher_repo_url - GitHub repo the launcher checks for updates against
    ///                       (full URL or "owner/repo").
    ///   launcher_branch   - branch to track. "" = the repo's default (alpha)
    ///                       stream (loose per-file releases). "beta" = the
    ///                       zip-release stream.
    ///   launcher_build    - "latest" (track the newest release on the branch)
    ///                       or a specific release tag (pinned build).
    /// </summary>
    public class LauncherSettings
    {
        public const string DefaultRepoUrl = "https://github.com/SlickAmogus/silent-hill-pc-nightly";

        public string RepoUrl = DefaultRepoUrl;
        public string Branch  = "";        // "" = alpha (repo default / non-prerelease stream)
        public string Build   = "latest";  // "latest" or a specific release tag

        // One-time acknowledgement: shown the first time the user pins an older
        // build (bugs / save-corruption / back-up-your-saves warning).
        public bool OldBuildWarned = false;

        public bool IsLatestBuild =>
            string.IsNullOrWhiteSpace(Build) || Build.Equals("latest", StringComparison.OrdinalIgnoreCase);

        public bool IsDefaultBranch => string.IsNullOrWhiteSpace(Branch);

        /// <summary>
        /// True when the configured repo resolves to the same owner/repo as the
        /// official default. Used to refuse risky operations (launcher
        /// self-update) when pointed at a third-party repo.
        /// </summary>
        public bool IsDefaultRepo
        {
            get
            {
                string o, r;
                if (!TryGetOwnerRepo(out o, out r)) return false;
                string o2, r2;
                if (!new LauncherSettings { RepoUrl = DefaultRepoUrl }.TryGetOwnerRepo(out o2, out r2)) return false;
                return string.Equals(o, o2, StringComparison.OrdinalIgnoreCase)
                    && string.Equals(r, r2, StringComparison.OrdinalIgnoreCase);
            }
        }

        public static LauncherSettings Load(ConfigManager cfg)
        {
            var s = new LauncherSettings();
            var url = cfg.Get("launcher_repo_url", "");
            if (!string.IsNullOrWhiteSpace(url)) s.RepoUrl = url.Trim();
            s.Branch = (cfg.Get("launcher_branch", "") ?? "").Trim();
            var build = cfg.Get("launcher_build", "latest");
            s.Build = string.IsNullOrWhiteSpace(build) ? "latest" : build.Trim();
            s.OldBuildWarned = cfg.Get("launcher_old_build_warned", "0").Trim() == "1";
            return s;
        }

        public void Save(ConfigManager cfg)
        {
            cfg.EnsureLauncherSection();
            cfg.Set("launcher_repo_url", string.IsNullOrWhiteSpace(RepoUrl) ? DefaultRepoUrl : RepoUrl.Trim());
            cfg.Set("launcher_branch", Branch ?? "");
            cfg.Set("launcher_build", IsLatestBuild ? "latest" : Build.Trim());
            cfg.Set("launcher_old_build_warned", OldBuildWarned ? "1" : "0");
            cfg.Save();
        }

        /// <summary>Parse "owner/repo" from a GitHub URL or a bare "owner/repo".</summary>
        public bool TryGetOwnerRepo(out string owner, out string repo)
        {
            owner = null; repo = null;
            var s = (RepoUrl ?? "").Trim();
            if (s.Length == 0) return false;

            int gh = s.IndexOf("github.com", StringComparison.OrdinalIgnoreCase);
            if (gh >= 0) s = s.Substring(gh + "github.com".Length);
            s = s.Trim('/', ' ');
            if (s.EndsWith(".git", StringComparison.OrdinalIgnoreCase))
                s = s.Substring(0, s.Length - 4);

            var parts = s.Split('/');
            if (parts.Length < 2 || parts[0].Length == 0 || parts[1].Length == 0) return false;
            owner = parts[0];
            repo  = parts[1];
            return true;
        }

        /// <summary>
        /// Launcher self-update ordering. True only if <paramref name="incoming"/>
        /// is STRICTLY NEWER than <paramref name="current"/> (date-based
        /// yyyy.M.d.rev). A missing/unparseable incoming version is treated as
        /// "not newer" so the launcher never downgrades or reinstalls itself.
        /// </summary>
        public static bool IsLauncherNewer(string incoming, string current)
        {
            if (string.IsNullOrWhiteSpace(incoming)) return false;
            Version vi;
            if (!Version.TryParse(incoming.Trim(), out vi)) return false;
            Version vc;
            if (string.IsNullOrWhiteSpace(current) || !Version.TryParse(current.Trim(), out vc))
                return true; // unknown current -> allow forward update
            return vi > vc;
        }

        /// <summary>The running launcher's own AssemblyFileVersion.</summary>
        public static string OwnLauncherVersion()
        {
            try
            {
                var loc = Assembly.GetExecutingAssembly().Location;
                return FileVersionInfo.GetVersionInfo(loc).FileVersion;
            }
            catch { return null; }
        }

        // --- Per-branch update tracking -------------------------------------
        // Update prompts are gated on the HIGHEST version ever installed from a
        // given repo+branch, not on what's currently on disk — so deliberately
        // running an old build doesn't nag you to "update". Each repo+branch gets
        // a stable identifier (same across uninstall/reinstall) and two config
        // values: launcher_seen_<id> (highest installed) and
        // launcher_dismissed_<id> (a version the user said "no" to).

        /// <summary>Stable identifier for this repo+branch (owner_repo_branch).</summary>
        public string BranchKey()
        {
            string owner, repo;
            if (!TryGetOwnerRepo(out owner, out repo)) { owner = "unknown"; repo = "repo"; }
            string br = string.IsNullOrWhiteSpace(Branch) ? "alpha" : Branch;
            return Sanitize(owner) + "_" + Sanitize(repo) + "_" + Sanitize(br);
        }

        private string SeenKey()      => "launcher_seen_" + BranchKey();
        private string DismissedKey() => "launcher_dismissed_" + BranchKey();

        public string GetSeen(ConfigManager cfg)      => cfg.Get(SeenKey(), "");
        public string GetDismissed(ConfigManager cfg) => cfg.Get(DismissedKey(), "");

        /// <summary>Record that <paramref name="version"/> was just installed: bump the
        /// highest-installed marker, and clear a dismissal once we've caught up to it.</summary>
        public void RecordInstalled(ConfigManager cfg, string version)
        {
            cfg.EnsureLauncherSection();
            if (CompareVersions(version, cfg.Get(SeenKey(), "")) > 0)
                cfg.Set(SeenKey(), version ?? "");
            string dis = cfg.Get(DismissedKey(), "");
            if (!string.IsNullOrEmpty(dis) && CompareVersions(version, dis) >= 0)
                cfg.Set(DismissedKey(), "");
            cfg.Save();
        }

        /// <summary>Bump the highest-installed marker when local already matches the
        /// latest (so a fresh install of the latest doesn't read as an update).</summary>
        public void RecordSeenAtLeast(ConfigManager cfg, string version)
        {
            if (CompareVersions(version, cfg.Get(SeenKey(), "")) > 0)
            {
                cfg.EnsureLauncherSection();
                cfg.Set(SeenKey(), version ?? "");
                cfg.Save();
            }
        }

        public void SetDismissed(ConfigManager cfg, string version)
        {
            cfg.EnsureLauncherSection();
            cfg.Set(DismissedKey(), version ?? "");
            cfg.Save();
        }

        /// <summary>Compare nightly version strings (yyyy.MM.dd.N). >0 if a newer than b.</summary>
        public static int CompareVersions(string a, string b)
        {
            Version va, vb;
            bool pa = Version.TryParse((a ?? "").Trim(), out va);
            bool pb = Version.TryParse((b ?? "").Trim(), out vb);
            if (pa && pb) return va.CompareTo(vb);
            if (pa) return 1;
            if (pb) return -1;
            return string.Compare((a ?? "").Trim(), (b ?? "").Trim(), StringComparison.OrdinalIgnoreCase);
        }

        private static string Sanitize(string s)
        {
            var sb = new StringBuilder();
            foreach (char c in (s ?? "").ToLowerInvariant())
                sb.Append(char.IsLetterOrDigit(c) ? c : '_');
            var r = sb.ToString().Trim('_');
            return r.Length == 0 ? "x" : r;
        }
    }
}
