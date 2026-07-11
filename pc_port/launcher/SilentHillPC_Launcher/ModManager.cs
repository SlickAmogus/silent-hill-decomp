using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;

namespace SilentHillPC_Launcher
{
    public enum ModType { Unknown, Texturemods, Load, Fmv }

    public class ModEntry
    {
        public string  Name;        // identity: library folder or .rar base name (also the texturemods subfolder + loadorder line)
        public ModType Type;
        public bool    Enabled;
        public string  DisplayName; // optional friendly label (cosmetic; identity stays Name)
        public string  Description; // optional user note
        public string  LibraryPath; // full path to the mod's folder, or its .rar file
        public bool    IsArchive;   // .rar: deployed as-is into texturemods/<Name>/, the game extracts it

        public string Label { get { return string.IsNullOrEmpty(DisplayName) ? Name : DisplayName; } }

        public string TypeLabel
        {
            get
            {
                switch (Type)
                {
                    case ModType.Texturemods: return IsArchive ? "Texture pack (RAR)" : "Texture pack";
                    case ModType.Load:        return "Load folder";
                    case ModType.Fmv:         return "FMV";
                    default:                  return "Unrecognized";
                }
            }
        }
    }

    [DataContract]
    public class ModStateDto
    {
        [DataMember] public string Name;
        [DataMember] public bool   Enabled;
        [DataMember] public string DisplayName;
        [DataMember] public string Description;
    }

    [DataContract]
    public class ModStateFile
    {
        [DataMember] public List<ModStateDto> Mods;
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
    ///
    /// Archives: .zip is expanded into a same-named folder on scan; .rar is kept
    /// as-is and (for texture packs) deployed into its subfolder for the game's
    /// own .rar extractor to unpack on launch.
    /// </summary>
    public class ModManager
    {
        private readonly string _gameRoot;
        private readonly ConfigManager _config;

        public string ModsDir         { get { return Path.Combine(_gameRoot, "mods"); } }
        private string GamedataDir    { get { return Path.Combine(_gameRoot, "gamedata"); } }
        private string TexturemodsDir { get { return Path.Combine(GamedataDir, "texturemods"); } }
        private string LoadDir        { get { return Path.Combine(GamedataDir, "load"); } }
        private string FmvDir         { get { return Path.Combine(GamedataDir, "FMV"); } }

        private string StatePath      { get { return Path.Combine(ModsDir, "modmanager.json"); } }
        private string ManifestPath   { get { return Path.Combine(ModsDir, "deployed.txt"); } }
        private string LoadOrderPath  { get { return Path.Combine(TexturemodsDir, "loadorder.txt"); } }

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

            var state = LoadState();
            var byName = new Dictionary<string, ModStateDto>(StringComparer.OrdinalIgnoreCase);
            foreach (var s in state.Mods)
                if (s.Name != null) byName[s.Name] = s;

            var found       = new List<ModEntry>();
            var folderNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var dir in Directory.GetDirectories(ModsDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(dir);
                if (name.StartsWith(".")) continue;
                folderNames.Add(name);
                found.Add(MakeEntry(name, dir, false, DetectType(dir), byName));
            }

            // .rar archives that don't already have an extracted folder of the same
            // name. The game unpacks .rar in gamedata/texturemods itself, so a .rar
            // mod deploys as the archive; treated as a texture pack.
            foreach (var rar in Directory.GetFiles(ModsDir, "*.rar", SearchOption.TopDirectoryOnly))
            {
                string name = Path.GetFileNameWithoutExtension(rar);
                if (folderNames.Contains(name)) continue;
                found.Add(MakeEntry(name, rar, true, ModType.Texturemods, byName));
            }

            // Preserve the saved order; append newly-found mods at the bottom.
            var ordered  = new List<ModEntry>();
            var lookup   = found.ToDictionary(m => m.Name, StringComparer.OrdinalIgnoreCase);
            foreach (var s in state.Mods)
            {
                if (s.Name != null && lookup.ContainsKey(s.Name))
                {
                    ordered.Add(lookup[s.Name]);
                    lookup.Remove(s.Name);
                }
            }
            ordered.AddRange(found.Where(m => lookup.ContainsKey(m.Name)));
            Mods = ordered;
        }

        private static ModEntry MakeEntry(string name, string libraryPath, bool isArchive,
                                          ModType type, Dictionary<string, ModStateDto> saved)
        {
            var e = new ModEntry
            {
                Name        = name,
                LibraryPath = libraryPath,
                IsArchive   = isArchive,
                Type        = type
            };
            ModStateDto s;
            if (saved.TryGetValue(name, out s))
            {
                e.Enabled     = s.Enabled;
                e.DisplayName = s.DisplayName;
                e.Description = s.Description;
            }
            return e;
        }

        private ModStateFile LoadState()
        {
            if (File.Exists(StatePath))
            {
                try
                {
                    using (var fs = File.OpenRead(StatePath))
                    {
                        var ser = new DataContractJsonSerializer(typeof(ModStateFile));
                        var f   = (ModStateFile)ser.ReadObject(fs);
                        if (f != null && f.Mods != null) return f;
                    }
                }
                catch { }
            }
            return new ModStateFile { Mods = new List<ModStateDto>() };
        }

        public void SaveState()
        {
            Directory.CreateDirectory(ModsDir);
            var f = new ModStateFile
            {
                Mods = Mods.Select(m => new ModStateDto
                {
                    Name        = m.Name,
                    Enabled     = m.Enabled,
                    DisplayName = m.DisplayName,
                    Description = m.Description
                }).ToList()
            };
            using (var fs = File.Create(StatePath))
            {
                var ser = new DataContractJsonSerializer(typeof(ModStateFile));
                ser.WriteObject(fs, f);
            }
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

        // --- import / remove --------------------------------------------------

        /// <summary>Copy a dropped folder or .zip/.rar archive into the library.</summary>
        public bool Import(string path)
        {
            try
            {
                if (Directory.Exists(path))
                {
                    string dest = Path.Combine(ModsDir, Path.GetFileName(path.TrimEnd('\\', '/')));
                    if (!Directory.Exists(dest)) CopyTree(path, dest);
                    return true;
                }
                if (File.Exists(path))
                {
                    string ext = Path.GetExtension(path).ToLowerInvariant();
                    if (ext == ".zip" || ext == ".rar")
                    {
                        Directory.CreateDirectory(ModsDir);
                        string dest = Path.Combine(ModsDir, Path.GetFileName(path));
                        if (!File.Exists(dest)) File.Copy(path, dest);
                        return true;
                    }
                }
            }
            catch { }
            return false;
        }

        /// <summary>Delete a mod's library files (folder + any sibling archive).</summary>
        public void RemoveMod(ModEntry m)
        {
            try
            {
                if (m.IsArchive) { if (File.Exists(m.LibraryPath)) File.Delete(m.LibraryPath); }
                else if (Directory.Exists(m.LibraryPath)) Directory.Delete(m.LibraryPath, true);

                foreach (var ext in new[] { ".zip", ".rar" })
                {
                    string sib = Path.Combine(ModsDir, m.Name + ext);
                    if (File.Exists(sib)) File.Delete(sib);
                }
            }
            catch { }
            Mods.Remove(m);
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
                string dst = Path.Combine(TexturemodsDir, m.Name);
                try
                {
                    if (m.IsArchive)
                    {
                        // Copy the .rar in; the game unpacks it into <dst>/*.rar.extracted/.
                        Directory.CreateDirectory(dst);
                        File.Copy(m.LibraryPath, Path.Combine(dst, Path.GetFileName(m.LibraryPath)), true);
                    }
                    else
                    {
                        CopyTree(DeploySourceRoot(m.LibraryPath, ModType.Texturemods), dst);
                    }
                    manifest.Add("D|" + Rel(dst));
                    result.Texture++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
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
                try
                {
                    result.Files += CopyTreeTracked(DeploySourceRoot(m.LibraryPath, ModType.Load), LoadDir, manifest);
                    result.Load++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
            }

            // FMV mods: flatten every .avi into gamedata/FMV. Same overwrite rule.
            var fmvMods = Mods.Where(m => m.Enabled && m.Type == ModType.Fmv).ToList();
            foreach (var m in Enumerable.Reverse(fmvMods))
            {
                try
                {
                    result.Files += CopyAvisFlat(m.LibraryPath, FmvDir, manifest);
                    result.Fmv++;
                }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
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
