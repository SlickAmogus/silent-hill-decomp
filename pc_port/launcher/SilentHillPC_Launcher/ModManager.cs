using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

namespace SilentHillPC_Launcher
{
    public enum ModType { Unknown, Texturemods, Load, Fmv }

    public class ModEntry
    {
        public string  Name;      // library folder name under mods/
        public ModType Type;
        public bool    Enabled;

        public string TypeLabel
        {
            get
            {
                switch (Type)
                {
                    case ModType.Texturemods: return "Texture pack";
                    case ModType.Load:        return "Load folder";
                    case ModType.Fmv:         return "FMV";
                    default:                  return "Unrecognized";
                }
            }
        }
    }

    /// <summary>
    /// Organizes user mods in a self-owned <c>mods/</c> library and deploys the
    /// enabled ones into the game's additive override dirs
    /// (<c>gamedata/texturemods</c>, <c>gamedata/load</c>, <c>gamedata/FMV</c>).
    /// Nothing here ever touches original game data — deploy = copy, undeploy =
    /// delete tracked files — so removing a mod always reverts cleanly.
    ///
    /// Load order (list order, index 0 = highest priority, wins conflicts):
    ///  - texture packs: each mod → its own texturemods/&lt;name&gt;/ subfolder;
    ///    priority is written to texturemods/loadorder.txt (highest first) and
    ///    resolved by the game's compositor (tex_pack.c).
    ///  - load/FMV: files merge into shared dirs, so higher priority is copied
    ///    LAST (overwrites lower).
    /// </summary>
    public class ModManager
    {
        private readonly string _gameRoot;
        private readonly ConfigManager _config;

        public string ModsDir        { get { return Path.Combine(_gameRoot, "mods"); } }
        private string GamedataDir   { get { return Path.Combine(_gameRoot, "gamedata"); } }
        private string TexturemodsDir{ get { return Path.Combine(GamedataDir, "texturemods"); } }
        private string LoadDir       { get { return Path.Combine(GamedataDir, "load"); } }
        private string FmvDir        { get { return Path.Combine(GamedataDir, "FMV"); } }

        private string StatePath     { get { return Path.Combine(ModsDir, "modmanager.txt"); } }
        private string ManifestPath  { get { return Path.Combine(ModsDir, "deployed.txt"); } }
        private string LoadOrderPath { get { return Path.Combine(TexturemodsDir, "loadorder.txt"); } }

        public List<ModEntry> Mods = new List<ModEntry>();

        public ModManager(string gameRoot, ConfigManager config)
        {
            _gameRoot = gameRoot;
            _config   = config;
        }

        // --- scan / state -----------------------------------------------------

        /// <summary>Extract loose .zip archives, index the library, merge saved state.</summary>
        public void Scan()
        {
            Directory.CreateDirectory(ModsDir);

            // A .zip dropped in the library is expanded into a same-named folder
            // once (non-destructive: the archive is kept, and a pre-existing
            // folder of the same name is never overwritten).
            foreach (var zip in Directory.GetFiles(ModsDir, "*.zip", SearchOption.TopDirectoryOnly))
            {
                string dest = Path.Combine(ModsDir, Path.GetFileNameWithoutExtension(zip));
                if (Directory.Exists(dest)) continue;
                try { ZipFile.ExtractToDirectory(zip, dest); }
                catch { /* corrupt/locked archive — leave it for the user to see */ }
            }

            var prev  = LoadState();
            var found = new List<ModEntry>();

            foreach (var dir in Directory.GetDirectories(ModsDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(dir);
                if (name.StartsWith(".")) continue;

                var e = new ModEntry
                {
                    Name    = name,
                    Type    = DetectType(dir),
                    Enabled = prev.ContainsKey(name) && prev[name]
                };
                found.Add(e);
            }

            // Preserve the saved order; append newly-found mods at the bottom.
            var ordered   = new List<ModEntry>();
            var byName     = found.ToDictionary(m => m.Name, StringComparer.OrdinalIgnoreCase);
            foreach (var name in LoadStateOrder())
            {
                if (byName.ContainsKey(name)) { ordered.Add(byName[name]); byName.Remove(name); }
            }
            ordered.AddRange(byName.Values);
            Mods = ordered;
        }

        private Dictionary<string, bool> LoadState()
        {
            var map = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            if (!File.Exists(StatePath)) return map;
            foreach (var line in File.ReadAllLines(StatePath))
            {
                var parts = line.Split('|');
                if (parts.Length >= 2) map[parts[1]] = parts[0] == "1";
            }
            return map;
        }

        private List<string> LoadStateOrder()
        {
            var order = new List<string>();
            if (!File.Exists(StatePath)) return order;
            foreach (var line in File.ReadAllLines(StatePath))
            {
                var parts = line.Split('|');
                if (parts.Length >= 2) order.Add(parts[1]);
            }
            return order;
        }

        public void SaveState()
        {
            var sb = new StringBuilder();
            foreach (var m in Mods)
                sb.AppendLine((m.Enabled ? "1" : "0") + "|" + m.Name);
            Directory.CreateDirectory(ModsDir);
            File.WriteAllText(StatePath, sb.ToString());
        }

        // --- type detection ---------------------------------------------------

        private static IEnumerable<string> SafeFiles(string dir)
        {
            try { return Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories); }
            catch { return Enumerable.Empty<string>(); }
        }

        private static ModType DetectType(string dir)
        {
            bool hasAvi  = false;
            bool hasFile = false;

            foreach (var f in SafeFiles(dir))
            {
                hasFile = true;
                string n = Path.GetFileName(f).ToLowerInvariant();
                if (n.EndsWith(".png") && (n.StartsWith("texupload-") || n.StartsWith("texpage-")))
                    return ModType.Texturemods;
                if (n == "config.yaml")
                    return ModType.Texturemods;
                if (n.EndsWith(".avi"))
                    hasAvi = true;
            }

            if (hasAvi) return ModType.Fmv;
            if (FindDirNamed(dir, "load") != null) return ModType.Load;
            if (hasFile) return ModType.Load; // disc-structured assets at the mod root
            return ModType.Unknown;
        }

        /// <summary>First directory (self or descendant) whose name matches, case-insensitive.</summary>
        private static string FindDirNamed(string root, string name)
        {
            try
            {
                if (string.Equals(Path.GetFileName(root), name, StringComparison.OrdinalIgnoreCase))
                    return root;
                foreach (var d in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
                    if (string.Equals(Path.GetFileName(d), name, StringComparison.OrdinalIgnoreCase))
                        return d;
            }
            catch { }
            return null;
        }

        /// <summary>The subtree inside a mod whose contents map onto the target dir.</summary>
        private static string DeploySourceRoot(string modDir, ModType type)
        {
            string wrapper =
                type == ModType.Texturemods ? "texturemods" :
                type == ModType.Load        ? "load"        :
                type == ModType.Fmv         ? "FMV"         : null;

            if (wrapper != null)
            {
                string found = FindDirNamed(modDir, wrapper);
                if (found != null) return found;
            }
            return modDir;
        }

        // --- deploy -----------------------------------------------------------

        public class ApplyResult
        {
            public int Texture, Load, Fmv, Files;
            public bool LooseEnabled;
            public List<string> Warnings = new List<string>();
        }

        /// <summary>
        /// Undeploy everything previously deployed, then deploy the enabled mods
        /// in priority order. <paramref name="looseFileSupport"/> drives
        /// allow_loose_files; it is force-enabled when a load mod is active.
        /// </summary>
        public ApplyResult Apply(bool looseFileSupport)
        {
            var result = new ApplyResult();

            Undeploy();

            var manifest = new List<string>();

            // Texture packs: each to its own subfolder; loadorder.txt ranks them.
            var texMods = Mods.Where(m => m.Enabled && m.Type == ModType.Texturemods).ToList();
            foreach (var m in texMods)
            {
                string modDir = Path.Combine(ModsDir, m.Name);
                string src    = DeploySourceRoot(modDir, ModType.Texturemods);
                string dst    = Path.Combine(TexturemodsDir, m.Name);
                try
                {
                    CopyTree(src, dst);
                    manifest.Add("D|" + Rel(dst));
                    result.Texture++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Name + ": " + ex.Message); }
            }
            if (texMods.Count > 0)
            {
                Directory.CreateDirectory(TexturemodsDir);
                // Highest priority first — matches tex_pack.c LoadOrder_Priority.
                File.WriteAllLines(LoadOrderPath, texMods.Select(m => m.Name));
                _config.Set("texture_packs", "1");
            }
            else if (File.Exists(LoadOrderPath))
            {
                try { File.Delete(LoadOrderPath); } catch { }
            }

            // Load-folder mods: merge into gamedata/load. Higher priority copied
            // LAST so it overwrites, so walk the enabled list in reverse.
            var loadMods = Mods.Where(m => m.Enabled && m.Type == ModType.Load).ToList();
            foreach (var m in Enumerable.Reverse(loadMods))
            {
                string modDir = Path.Combine(ModsDir, m.Name);
                string src    = DeploySourceRoot(modDir, ModType.Load);
                try
                {
                    result.Files += CopyTreeTracked(src, LoadDir, manifest);
                    result.Load++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Name + ": " + ex.Message); }
            }

            // FMV mods: flatten every .avi into gamedata/FMV. Same overwrite rule.
            var fmvMods = Mods.Where(m => m.Enabled && m.Type == ModType.Fmv).ToList();
            foreach (var m in Enumerable.Reverse(fmvMods))
            {
                string modDir = Path.Combine(ModsDir, m.Name);
                try
                {
                    result.Files += CopyAvisFlat(modDir, FmvDir, manifest);
                    result.Fmv++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Name + ": " + ex.Message); }
            }

            WriteManifest(manifest);

            // A load-folder mod is inert unless the game scans gamedata/load.
            bool loose = looseFileSupport || loadMods.Count > 0;
            _config.Set("allow_loose_files", loose ? "1" : "0");
            _config.Save();
            result.LooseEnabled = loose;

            SaveState();
            return result;
        }

        private void Undeploy()
        {
            if (File.Exists(ManifestPath))
            {
                foreach (var line in File.ReadAllLines(ManifestPath))
                {
                    if (line.Length < 3 || line[1] != '|') continue;
                    string full = Path.Combine(_gameRoot, line.Substring(2));
                    try
                    {
                        if (line[0] == 'D' && Directory.Exists(full)) Directory.Delete(full, true);
                        else if (line[0] == 'F' && File.Exists(full))  File.Delete(full);
                    }
                    catch { }
                }
                try { File.Delete(ManifestPath); } catch { }
            }
            PruneEmptyDirs(LoadDir);
            PruneEmptyDirs(FmvDir);
            PruneEmptyDirs(TexturemodsDir);
        }

        private void WriteManifest(List<string> manifest)
        {
            Directory.CreateDirectory(ModsDir);
            File.WriteAllLines(ManifestPath, manifest);
        }

        private string Rel(string full)
        {
            string root = _gameRoot.EndsWith("\\") ? _gameRoot : _gameRoot + "\\";
            return full.StartsWith(root, StringComparison.OrdinalIgnoreCase)
                ? full.Substring(root.Length)
                : full;
        }

        private static void CopyTree(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (var dir in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(dir.Replace(src, dst));
            foreach (var file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
                File.Copy(file, file.Replace(src, dst), true);
        }

        private int CopyTreeTracked(string src, string dstRoot, List<string> manifest)
        {
            int n = 0;
            Directory.CreateDirectory(dstRoot);
            foreach (var file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string rel = file.Substring(src.Length).TrimStart('\\', '/');
                string dst = Path.Combine(dstRoot, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Copy(file, dst, true);
                manifest.Add("F|" + Rel(dst));
                n++;
            }
            return n;
        }

        private int CopyAvisFlat(string src, string dstRoot, List<string> manifest)
        {
            int n = 0;
            Directory.CreateDirectory(dstRoot);
            foreach (var file in SafeFiles(src))
            {
                if (!file.EndsWith(".avi", StringComparison.OrdinalIgnoreCase)) continue;
                string dst = Path.Combine(dstRoot, Path.GetFileName(file));
                File.Copy(file, dst, true);
                manifest.Add("F|" + Rel(dst));
                n++;
            }
            return n;
        }

        private static void PruneEmptyDirs(string root)
        {
            if (!Directory.Exists(root)) return;
            foreach (var dir in Directory.GetDirectories(root, "*", SearchOption.AllDirectories)
                                          .OrderByDescending(d => d.Length))
            {
                try
                {
                    if (!Directory.EnumerateFileSystemEntries(dir).Any())
                        Directory.Delete(dir, false);
                }
                catch { }
            }
        }

        public void OpenModsFolder()
        {
            Directory.CreateDirectory(ModsDir);
            try { System.Diagnostics.Process.Start("explorer.exe", "\"" + ModsDir + "\""); }
            catch { }
        }
    }
}
