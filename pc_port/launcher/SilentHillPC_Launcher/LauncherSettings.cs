using System;
using System.Diagnostics;
using System.Reflection;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Launcher-only settings, persisted in config.cfg under the "## Launcher"
    /// section as launcher_* keys. The game ignores unknown keys (pc_config.c),
    /// so the launcher and game share one config file.
    ///
    ///   launcher_repo_url - GitHub repo the launcher checks for updates against
    ///                       (full URL or "owner/repo").
    ///   launcher_branch   - branch to track. "" = the repo's default/stable
    ///                       stream (loose per-file releases). "beta" = the
    ///                       zip-release stream.
    ///   launcher_build    - "latest" (track the newest release on the branch)
    ///                       or a specific release tag (pinned build).
    /// </summary>
    public class LauncherSettings
    {
        public const string DefaultRepoUrl = "https://github.com/SlickAmogus/silent-hill-pc-nightly";

        public string RepoUrl = DefaultRepoUrl;
        public string Branch  = "";        // "" = stable / repo default branch
        public string Build   = "latest";  // "latest" or a specific release tag

        public bool IsLatestBuild =>
            string.IsNullOrWhiteSpace(Build) || Build.Equals("latest", StringComparison.OrdinalIgnoreCase);

        public bool IsDefaultBranch => string.IsNullOrWhiteSpace(Branch);

        public static LauncherSettings Load(ConfigManager cfg)
        {
            var s = new LauncherSettings();
            var url = cfg.Get("launcher_repo_url", "");
            if (!string.IsNullOrWhiteSpace(url)) s.RepoUrl = url.Trim();
            s.Branch = (cfg.Get("launcher_branch", "") ?? "").Trim();
            var build = cfg.Get("launcher_build", "latest");
            s.Build = string.IsNullOrWhiteSpace(build) ? "latest" : build.Trim();
            return s;
        }

        public void Save(ConfigManager cfg)
        {
            cfg.EnsureLauncherSection();
            cfg.Set("launcher_repo_url", string.IsNullOrWhiteSpace(RepoUrl) ? DefaultRepoUrl : RepoUrl.Trim());
            cfg.Set("launcher_branch", Branch ?? "");
            cfg.Set("launcher_build", IsLatestBuild ? "latest" : Build.Trim());
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
    }
}
