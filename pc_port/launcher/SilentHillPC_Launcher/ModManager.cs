using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;

namespace SilentHillPC_Launcher
{
    public enum ModType   { Unknown, Texturemods, Load, Fmv, Gameplay, TotalConversion }
    public enum ModSource { Library, TextureMods }

    public class ModEntry
    {
        public string     Name;        // identity (folder / .zip / .rar filename); loadorder + deploy key
        public ModType    Type;
        public ModSource  Source;
        public bool       Enabled;     // Library: deploy?  TextureMods: active?
        public string     DisplayName;
        public string     Description;
        public string     LibraryPath; // current on-disk path (folder / .zip / .rar / .7z; may be *.disabled)
        public bool       IsArchive;   // texture mod backed by a .zip/.rar/.7z (vs a loose folder)
        public bool       IsRar;       // .rar: extracted via UnRAR.dll
        public bool       IsSevenZip;  // .7z: extracted via 7za.exe
        public bool       IsInPlaceZip; // fully-stored .zip the game reads in place (miniz), never extracted

        /// <summary>Texture archives are materialized to a sibling &lt;name&gt;.extracted/
        /// folder the game reads as a loose folder — EXCEPT a fully-stored .zip, which the
        /// game reads straight from the file (miniz), so it toggles like a loose entry
        /// (&lt;name&gt;.zip ↔ &lt;name&gt;.zip.disabled), not via an extracted folder.</summary>
        public bool IsExtractedArchive { get { return IsArchive && !IsInPlaceZip; } }

        public string Label { get { return string.IsNullOrEmpty(DisplayName) ? Name : DisplayName; } }

        public string TypeLabel
        {
            get
            {
                switch (Type)
                {
                    case ModType.Texturemods:
                        return IsRar        ? "Texture pack (.rar)"
                             : IsSevenZip   ? "Texture pack (.7z)"
                             : IsInPlaceZip ? "Texture pack (.zip, in place)"
                             : IsArchive    ? "Texture pack (.zip)"
                                            : "Texture pack (folder)";
                    case ModType.Load:            return "Data Overlay (load/)";
                    case ModType.Fmv:             return "FMV Video";
                    case ModType.Gameplay:        return "Gameplay (Code / DLL)";
                    case ModType.TotalConversion: return "Total Conversion";
                    default:                      return "Unrecognized";
                }
            }
        }

        public string StateLabel
        {
            get { return Enabled ? "Enabled" : "Disabled"; }
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
        [DataMember] public bool DllWarningAck; /* "Don't show me again" on the DLL-mod install warning */
    }

    /// <summary>
    /// Manages mods across two homes, both additive (nothing touches the disc image):
    ///  - TEXTURE mods live in gamedata/texturemods/ as a loose folder or an archive
    ///    (.zip/.rar/.7z). Every archive is EXTRACTED to a sibling &lt;name&gt;.extracted/
    ///    folder that the game reads as a loose folder (.rar via UnRAR.dll, .zip/.7z
    ///    via 7za.exe — 7-Zip decodes every compression method, unlike the in-place
    ///    miniz reader which only did Store/Deflate). "Enable"/"disable" renames that
    ///    folder to/from a ".disabled" suffix, which the game skips (Name_IsDisabled).
    ///    A loose folder is toggled the same way. (The game still reads a hand-dropped
    ///    .zip in place if no .extracted sibling exists — a launcher-free fallback.)
    ///  - LOAD / FMV mods live in the mods/ library and deploy into gamedata/load
    ///    and gamedata/FMV on Apply, tracked by a manifest for clean removal.
    /// Load order (list order, index 0 = highest priority): texture packs via
    /// texturemods/loadorder.txt; load/FMV copied highest-last so it overwrites.
    /// </summary>
    public class ModManager
    {
        private readonly string _gameRoot;
        private readonly ConfigManager _config;

        public  string ModsDir         { get { return Path.Combine(_gameRoot, "mods"); } }
        private string GamedataDir     { get { return Path.Combine(_gameRoot, "gamedata"); } }
        private string TexturemodsDir  { get { return Path.Combine(GamedataDir, "texturemods"); } }
        private string LoadDir         { get { return Path.Combine(GamedataDir, "load"); } }
        private string FmvDir          { get { return Path.Combine(GamedataDir, "FMV"); } }

        private string StatePath       { get { return Path.Combine(ModsDir, "modmanager.json"); } }
        private string ManifestPath    { get { return Path.Combine(ModsDir, "deployed.txt"); } }
        private string LoadOrderPath   { get { return Path.Combine(TexturemodsDir, "loadorder.txt"); } }
        public  string BackupDir       { get { return Path.Combine(ModsDir, ".backup"); } }

        public List<ModEntry> Mods = new List<ModEntry>();

        /// <summary>"Don't show me again" on the DLL-mod install warning (persisted).</summary>
        public bool DllWarningAck;

        public ModManager(string gameRoot, ConfigManager config)
        {
            _gameRoot = gameRoot;
            _config   = config;
        }

        // --- helpers ----------------------------------------------------------

        public static bool IsRar(string p)
        {
            return Path.GetExtension(StripDisabled(p)).Equals(".rar", StringComparison.OrdinalIgnoreCase);
        }

        private static bool IsZip(string p)
        {
            return Path.GetExtension(StripDisabled(p)).Equals(".zip", StringComparison.OrdinalIgnoreCase);
        }

        public static bool IsSevenZip(string p)
        {
            return Path.GetExtension(StripDisabled(p)).Equals(".7z", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>Any texture archive the launcher materializes to a folder.</summary>
        private static bool IsArchiveFile(string p)
        {
            return IsZip(p) || IsRar(p) || IsSevenZip(p);
        }

        private static bool IsDisabled(string p) { return p.EndsWith(".disabled", StringComparison.OrdinalIgnoreCase); }

        private static string StripDisabled(string p)
        {
            return IsDisabled(p) ? p.Substring(0, p.Length - ".disabled".Length) : p;
        }

        /// <summary>The folder an archive (.zip/.rar/.7z) is extracted into (the game
        /// reads it as a loose folder mod). <paramref name="archivePath"/> may be the
        /// enabled or *.disabled archive path.</summary>
        private static string ArchiveActiveFolder(string archivePath)   { return StripDisabled(archivePath) + ".extracted"; }
        private static string ArchiveDisabledFolder(string archivePath) { return ArchiveActiveFolder(archivePath) + ".disabled"; }

        /// <summary>True if EVERY file entry in the zip is STORED (uncompressed): its
        /// bytes sit plainly in the file, so the game's miniz reader loads it in place
        /// with no extraction and no disk copy. Compressed (deflate/LZMA/…) or unreadable
        /// → false, so it falls back to the extract path. (miniz also decodes deflate, so
        /// a false positive here is only ever conservative — it extracts something that
        /// would have read fine.)</summary>
        private static bool IsFullyStoredZip(string zipPath)
        {
            try
            {
                using (var za = ZipFile.OpenRead(zipPath))
                {
                    bool anyFile = false;
                    foreach (var en in za.Entries)
                    {
                        if (en.FullName.EndsWith("/", StringComparison.Ordinal)) continue; // directory
                        anyFile = true;
                        if (en.CompressedLength != en.Length) return false;                 // compressed
                    }
                    return anyFile;
                }
            }
            catch { return false; }
        }

        /// <summary>Extract a texture archive to <paramref name="dest"/>, dispatching by
        /// extension: .rar → UnRAR.dll, .zip/.7z → 7za.exe. Returns success — a cancelled
        /// run reports failure, so the caller drops the half-written folder.</summary>
        private static bool ExtractArchive(string archivePath, string dest, Action<int, int, string> report,
                                           Func<bool> cancelled = null)
        {
            // Extract the literal file on disk (a legacy "<name>.disabled" archive
            // keeps that suffix); the extension under any .disabled picks the tool.
            if (IsRar(archivePath)) return RarExtractor.Extract(archivePath, dest, report, cancelled);
            return SevenZipExtractor.Extract(archivePath, dest, report, cancelled); // .zip and .7z
        }

        private static IEnumerable<string> SafeFiles(string dir)
        {
            try { return Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories); }
            catch { return Enumerable.Empty<string>(); }
        }

        // --- scan / state -----------------------------------------------------

        /// <summary>An archive sitting in the mod folders that has not been materialized to a
        /// folder yet. <see cref="Cheap"/> marks one whose unpacking is a plain byte copy with
        /// no decompression, so it can run without asking the user first.</summary>
        public class PendingItem
        {
            public string Path;         // the archive on disk
            public bool   IsLibraryZip; // mods/<name>.zip → mods/<name>/ (vs a texture pack)
            public bool   Cheap;        // fully-STORED zip: unpacking is a copy, not a decompress

            public string Name { get { return System.IO.Path.GetFileName(Path); } }
        }

        /// <summary>Everything <see cref="Prepare"/> would unpack right now. Nothing here is
        /// touched until the caller passes items back in — opening the manager must not start
        /// unpacking on its own.</summary>
        public List<PendingItem> PendingWork()
        {
            var list = new List<PendingItem>();
            foreach (var z in PendingLibraryZips())
                list.Add(new PendingItem { Path = z, IsLibraryZip = true, Cheap = IsFullyStoredZip(z) });
            // Texture side: a fully-stored .zip is already excluded by PendingArchives()
            // (the game reads it in place), so every entry left here really decompresses.
            foreach (var a in PendingArchives())
                list.Add(new PendingItem { Path = a, IsLibraryZip = false, Cheap = false });
            return list;
        }

        private string[] PendingLibraryZips()
        {
            if (!Directory.Exists(ModsDir)) return new string[0];
            return Directory.GetFiles(ModsDir, "*.zip", SearchOption.TopDirectoryOnly)
                            .Where(z => !Directory.Exists(Path.Combine(ModsDir, Path.GetFileNameWithoutExtension(z))))
                            .ToArray();
        }

        /// <summary>Texture archives (.zip/.rar/.7z) in texturemods/ not yet materialized
        /// to an <c>.extracted</c> folder (enabled or disabled). These get extracted so
        /// the game can read them as folder mods. Skips already-disabled archive files.</summary>
        private string[] PendingArchives()
        {
            if (!Directory.Exists(TexturemodsDir)) return new string[0];
            return Directory.GetFiles(TexturemodsDir, "*", SearchOption.TopDirectoryOnly)
                            .Where(a => IsArchiveFile(a) && !IsDisabled(a))
                            .Where(a => !Directory.Exists(ArchiveActiveFolder(a)) &&
                                        !Directory.Exists(ArchiveDisabledFolder(a)))
                            // A fully-stored .zip is read in place by the game — don't extract it.
                            .Where(a => !(a.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && IsFullyStoredZip(a)))
                            .ToArray();
        }

        /// <summary>Unpack the given <see cref="PendingWork"/> items, with progress. Cancel is
        /// cooperative and leaves nothing behind: the archive itself is never modified, and a
        /// folder that was only partly written is deleted, so the next scan sees exactly the
        /// state this one started from.</summary>
        public void Prepare(IEnumerable<PendingItem> items, Action<int, int, string> report, Func<bool> cancelled)
        {
            Directory.CreateDirectory(ModsDir);
            foreach (var it in items)
            {
                if (cancelled != null && cancelled()) return;

                string dest = it.IsLibraryZip
                    ? Path.Combine(ModsDir, Path.GetFileNameWithoutExtension(it.Path))
                    : ArchiveActiveFolder(it.Path);

                bool ok;
                try
                {
                    ok = it.IsLibraryZip ? ExtractZip(it.Path, dest, report, cancelled)
                                         : ExtractArchive(it.Path, dest, report, cancelled);
                }
                catch { ok = false; }

                if (!ok && Directory.Exists(dest))
                {
                    try { Directory.Delete(dest, true); } catch { } // partial/failed/cancelled → drop it
                }
            }
        }

        public void Scan()
        {
            Directory.CreateDirectory(ModsDir);
            var state  = LoadState();
            DllWarningAck = state.DllWarningAck;
            var byName = new Dictionary<string, ModStateDto>(StringComparer.OrdinalIgnoreCase);
            foreach (var s in state.Mods) if (s.Name != null) byName[s.Name] = s;

            var found = new List<ModEntry>();
            found.AddRange(ScanTextureMods(byName));
            found.AddRange(ScanLibraryMods(byName));

            // Preserve saved order; append newly-found mods at the bottom.
            var ordered = new List<ModEntry>();
            var lookup  = new Dictionary<string, ModEntry>(StringComparer.OrdinalIgnoreCase);
            foreach (var m in found) lookup[m.Source + "/" + m.Name] = m;
            foreach (var s in state.Mods)
            {
                var keyT = ModSource.TextureMods + "/" + s.Name;
                var keyL = ModSource.Library + "/" + s.Name;
                if (lookup.ContainsKey(keyT)) { ordered.Add(lookup[keyT]); lookup.Remove(keyT); }
                else if (lookup.ContainsKey(keyL)) { ordered.Add(lookup[keyL]); lookup.Remove(keyL); }
            }
            ordered.AddRange(found.Where(m => lookup.ContainsKey(m.Source + "/" + m.Name)));
            Mods = ordered;
        }

        private IEnumerable<ModEntry> ScanTextureMods(Dictionary<string, ModStateDto> saved)
        {
            var list = new List<ModEntry>();
            if (!Directory.Exists(TexturemodsDir)) return list;

            // Every archive (.zip/.rar/.7z) is materialized to <name>.extracted/ and
            // read by the game as a loose folder; "active" = that folder exists.
            foreach (var f in Directory.GetFiles(TexturemodsDir))
            {
                string real = StripDisabled(f);
                if (!IsArchiveFile(real)) continue;
                string name = Path.GetFileName(real);

                // Fully-stored .zip with no .extracted sibling: the game reads it in place
                // (miniz), so it toggles like a loose entry — f is <name>.zip when enabled,
                // <name>.zip.disabled when disabled — and is never extracted.
                if (real.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) &&
                    !Directory.Exists(ArchiveActiveFolder(real)) &&
                    !Directory.Exists(ArchiveDisabledFolder(real)) &&
                    IsFullyStoredZip(f))
                {
                    list.Add(MakeTexture(name, f, true, false, false, true, !IsDisabled(f), saved));
                    continue;
                }

                if (IsDisabled(f)) continue; // a disabled non-in-place archive file: not expected

                bool active = Directory.Exists(ArchiveActiveFolder(f));
                list.Add(MakeTexture(name, f, true, IsRar(f), IsSevenZip(f), false, active, saved));
            }

            // Loose top-level folders (each its own pack). Skip a legacy *.extracted
            // companion of a listed archive (the archive is read in place now).
            var archiveBases = new HashSet<string>(
                list.Where(m => m.IsArchive).Select(m => m.Name),
                StringComparer.OrdinalIgnoreCase);
            foreach (var d in Directory.GetDirectories(TexturemodsDir))
            {
                string dname = Path.GetFileName(d);
                string enabled = StripDisabled(dname);
                if (enabled.EndsWith(".extracted", StringComparison.OrdinalIgnoreCase) &&
                    archiveBases.Contains(enabled.Substring(0, enabled.Length - ".extracted".Length)))
                    continue;
                list.Add(MakeTexture(enabled, d, false, false, false, false, !IsDisabled(dname), saved));
            }
            return list;
        }

        private static ModEntry MakeTexture(string name, string path, bool isArchive, bool isRar, bool isSevenZip,
                                            bool isInPlaceZip, bool enabled, Dictionary<string, ModStateDto> saved)
        {
            var e = new ModEntry
            {
                Name         = name,
                Type         = ModType.Texturemods,
                Source       = ModSource.TextureMods,
                LibraryPath  = path,
                IsArchive    = isArchive,
                IsRar        = isRar,
                IsSevenZip   = isSevenZip,
                IsInPlaceZip = isInPlaceZip,
                Enabled      = enabled
            };
            ModStateDto s;
            if (saved.TryGetValue(name, out s)) { e.DisplayName = s.DisplayName; e.Description = s.Description; }
            return e;
        }

        private IEnumerable<ModEntry> ScanLibraryMods(Dictionary<string, ModStateDto> saved)
        {
            var list = new List<ModEntry>();
            foreach (var dir in Directory.GetDirectories(ModsDir).OrderBy(d => d, StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(dir);
                if (name.StartsWith(".")) continue;
                ModType t = DetectType(dir);
                if (t == ModType.Texturemods) continue; // texture mods belong in texturemods/, not the library
                list.Add(MakeLibrary(name, dir, t, saved));
            }
            return list;
        }

        private static ModEntry MakeLibrary(string name, string path, ModType type,
                                            Dictionary<string, ModStateDto> saved)
        {
            var e = new ModEntry
            {
                Name        = name,
                Type        = type,
                Source      = ModSource.Library,
                LibraryPath = path,
                IsArchive   = false
            };
            ModStateDto s;
            if (saved.TryGetValue(name, out s))
            {
                e.Enabled = s.Enabled; e.DisplayName = s.DisplayName; e.Description = s.Description;
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
                        var f = (ModStateFile)new DataContractJsonSerializer(typeof(ModStateFile)).ReadObject(fs);
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
                    Name = m.Name, Enabled = m.Enabled, DisplayName = m.DisplayName, Description = m.Description
                }).ToList(),
                DllWarningAck = DllWarningAck
            };
            using (var fs = File.Create(StatePath))
                new DataContractJsonSerializer(typeof(ModStateFile)).WriteObject(fs, f);
        }

        // --- type detection ---------------------------------------------------

        /// <summary>Video containers the FMV player can override with — the game
        /// plays .avi natively and mp4/mkv/webm/mov/m4v through its ffmpeg path.
        /// An archive or folder holding any of these is treated as an FMV mod.</summary>
        private static readonly string[] VideoExts =
            { ".avi", ".mp4", ".mkv", ".webm", ".mov", ".m4v" };

        private static bool IsVideoFile(string name)
        {
            foreach (var e in VideoExts)
                if (name.EndsWith(e, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        private static ModType DetectType(string dir)
        {
            bool hasDll = false, hasVideo = false, hasTexture = false, hasLoad = false, hasFile = false;
            foreach (var f in SafeFiles(dir))
            {
                hasFile = true;
                string n = Path.GetFileName(f).ToLowerInvariant();
                string ext = Path.GetExtension(f).ToLowerInvariant();

                if (ext == ".dll") hasDll = true;
                if (n.EndsWith(".png") && (n.StartsWith("texupload-") || n.StartsWith("texpage-"))) hasTexture = true;
                if (n == "config.yaml") hasTexture = true;
                if (IsVideoFile(n)) hasVideo = true;
                if (f.Replace('\\', '/').ToLowerInvariant().Contains("/load/")) hasLoad = true;
            }

            if (FindDirNamed(dir, "load") != null) hasLoad = true;
            if (FindDirNamed(dir, "maps") != null) hasDll = true;
            if (FindDirNamed(dir, "plugins") != null) hasDll = true;

            if (hasDll && (hasLoad || hasTexture || hasVideo)) return ModType.TotalConversion;
            if (hasDll) return ModType.Gameplay;
            if (hasVideo && !hasLoad && !hasTexture) return ModType.Fmv;
            if (hasTexture && !hasLoad && !hasVideo) return ModType.Texturemods;
            if (hasLoad || hasFile) return ModType.Load;
            return ModType.Unknown;
        }

        /// <summary>Detect a dropped path's type, peeking inside .zip archives.</summary>
        internal static ModType DetectDroppedType(string path)
        {
            if (Directory.Exists(path)) return DetectType(path);
            string ext = Path.GetExtension(path).ToLowerInvariant();
            if (ext == ".rar" || ext == ".7z")
            {
                string fn = Path.GetFileName(path).ToLowerInvariant();
                if (fn.Contains("gameplay") || fn.Contains("plugin") || fn.Contains("code") || fn.Contains("dll"))
                    return ModType.Gameplay;
                return ModType.Texturemods;
            }
            if (ext == ".zip")
            {
                try
                {
                    using (var za = ZipFile.OpenRead(path))
                    {
                        bool hasDll = false, hasVideo = false, hasTexture = false, hasLoad = false;
                        foreach (var en in za.Entries)
                        {
                            string n = Path.GetFileName(en.FullName).ToLowerInvariant();
                            string e = Path.GetExtension(en.FullName).ToLowerInvariant();

                            if (e == ".dll") hasDll = true;
                            if (n.EndsWith(".png") && (n.StartsWith("texupload-") || n.StartsWith("texpage-"))) hasTexture = true;
                            if (n == "config.yaml") hasTexture = true;
                            if (IsVideoFile(n)) hasVideo = true;
                                        if (en.FullName.Replace('\\', '/').ToLowerInvariant().Contains("load/")) hasLoad = true;
                            if (en.FullName.Replace('\\', '/').ToLowerInvariant().Contains("maps/")) hasDll = true;
                        }

                        if (hasDll && (hasLoad || hasTexture || hasVideo)) return ModType.TotalConversion;
                        if (hasDll) return ModType.Gameplay;
                        if (hasVideo && !hasLoad && !hasTexture) return ModType.Fmv;
                        if (hasTexture && !hasLoad && !hasVideo) return ModType.Texturemods;
                        if (hasLoad) return ModType.Load;
                    }
                }
                catch { }
                return ModType.Texturemods; // default
            }
            return ModType.Unknown;
        }

        private static string FindDirNamed(string root, string name)
        {
            try
            {
                if (string.Equals(Path.GetFileName(root), name, StringComparison.OrdinalIgnoreCase)) return root;
                foreach (var d in Directory.EnumerateDirectories(root, "*", SearchOption.AllDirectories))
                    if (string.Equals(Path.GetFileName(d), name, StringComparison.OrdinalIgnoreCase)) return d;
            }
            catch { }
            return null;
        }

        private static string DeploySourceRoot(string modDir, ModType type)
        {
            string wrapper = type == ModType.Load ? "load" : type == ModType.Fmv ? "FMV" : null;
            if (wrapper != null) { string f = FindDirNamed(modDir, wrapper); if (f != null) return f; }
            return modDir;
        }

        // --- import (drag & drop) --------------------------------------------

        public enum ImportResult { Added, Skipped }

        /// <summary>Route a dropped folder/.zip/.rar/.7z to texturemods/ (texture) or mods/ (load/FMV).
        /// A cancelled folder copy deletes the half-copied destination and reports Skipped; the
        /// dropped original is never touched.</summary>
        public ImportResult Import(string path, Action<int, int, string> report, Func<bool> cancelled = null)
        {
            try
            {
                ModType t = DetectDroppedType(path);
                if (t == ModType.Unknown) return ImportResult.Skipped;

                string home = t == ModType.Texturemods ? TexturemodsDir : ModsDir;
                Directory.CreateDirectory(home);

                if (Directory.Exists(path))
                {
                    string dest = Path.Combine(home, Path.GetFileName(path.TrimEnd('\\', '/')));
                    if (!Directory.Exists(dest) &&
                        !CopyTree(path, dest, report, "Importing " + Path.GetFileName(dest), cancelled))
                    {
                        try { Directory.Delete(dest, true); } catch { }
                        return ImportResult.Skipped;
                    }
                }
                else
                {
                    // An archive (.zip/.rar/.7z) — copy it in; Prepare() unpacks it after.
                    string dest = Path.Combine(home, Path.GetFileName(path));
                    if (report != null) report(0, 0, "Copying " + Path.GetFileName(path));
                    if (!File.Exists(dest)) File.Copy(path, dest);
                }
                return ImportResult.Added;
            }
            catch { return ImportResult.Skipped; }
        }

        // --- texture enable / disable / delete --------------------------------

        private static void MovePath(string from, string to, bool isDir)
        {
            if (isDir)
            {
                if (Directory.Exists(to)) Directory.Delete(from, true);
                else Directory.Move(from, to);
            }
            else
            {
                if (File.Exists(to)) File.Delete(from);
                else File.Move(from, to);
            }
        }

        /// <summary>Activate a texture pack so the game loads it. Archive (.zip/.rar/.7z):
        /// make its <c>.extracted</c> folder active, extracting on first use. Loose folder:
        /// drop the .disabled suffix.</summary>
        public bool EnableTexture(ModEntry m, Action<int, int, string> report, Func<bool> cancelled = null)
        {
            try
            {
                if (m.IsExtractedArchive)
                {
                    string active = ArchiveActiveFolder(m.LibraryPath);
                    string off    = ArchiveDisabledFolder(m.LibraryPath);
                    if (!Directory.Exists(active))
                    {
                        if (Directory.Exists(off)) Directory.Move(off, active);
                        else if (!ExtractArchive(m.LibraryPath, active, report, cancelled))
                        {
                            if (Directory.Exists(active)) { try { Directory.Delete(active, true); } catch { } }
                            return false;
                        }
                    }
                    m.Enabled = true;
                    return true;
                }

                if (IsDisabled(m.LibraryPath))
                {
                    string on = StripDisabled(m.LibraryPath);
                    MovePath(m.LibraryPath, on, !m.IsInPlaceZip); // loose folder (dir) or in-place .zip (file)
                    m.LibraryPath = on;
                }
                m.Enabled = true;
                return true;
            }
            catch { return false; }
        }

        /// <summary>Deactivate a texture pack so the game skips it. Archive (.zip/.rar/.7z):
        /// rename its <c>.extracted</c> folder .disabled (kept, so re-enable needn't
        /// re-extract). Loose folder: rename the folder .disabled.</summary>
        public void DisableTexture(ModEntry m)
        {
            try
            {
                if (m.IsExtractedArchive)
                {
                    string active = ArchiveActiveFolder(m.LibraryPath);
                    string off    = ArchiveDisabledFolder(m.LibraryPath);
                    if (Directory.Exists(active))
                    {
                        if (Directory.Exists(off)) Directory.Delete(active, true);
                        else Directory.Move(active, off);
                    }
                    m.Enabled = false;
                    return;
                }

                if (!IsDisabled(m.LibraryPath))
                {
                    string offp = m.LibraryPath + ".disabled";
                    MovePath(m.LibraryPath, offp, !m.IsInPlaceZip); // loose folder (dir) or in-place .zip (file)
                    m.LibraryPath = offp;
                }
                m.Enabled = false;
            }
            catch { }
        }

        /// <summary>Delete a texture mod entirely: the folder/.zip/.rar/.7z (enabled or
        /// disabled) plus, for an archive, its extracted folder (active or disabled).</summary>
        public void DeleteTexture(ModEntry m)
        {
            try
            {
                string enabled = StripDisabled(m.LibraryPath);
                foreach (var p in new[] { enabled, enabled + ".disabled" })
                {
                    if (m.IsArchive) { if (File.Exists(p)) File.Delete(p); }
                    else             { if (Directory.Exists(p)) Directory.Delete(p, true); }
                }
                if (m.IsExtractedArchive)
                {
                    foreach (var f in new[] { enabled + ".extracted", enabled + ".extracted.disabled" })
                        if (Directory.Exists(f)) Directory.Delete(f, true);
                }
            }
            catch { }
            Mods.Remove(m);
        }

        /// <summary>Delete a library (load/FMV) mod's files, plus any sibling .zip.</summary>
        public void DeleteLibrary(ModEntry m)
        {
            try
            {
                if (Directory.Exists(m.LibraryPath)) Directory.Delete(m.LibraryPath, true);
                string sib = Path.Combine(ModsDir, m.Name + ".zip");
                if (File.Exists(sib)) File.Delete(sib);
            }
            catch { }
            Mods.Remove(m);
        }

        /// <summary>Is this texture pack currently active on disk (game will load it)?</summary>
        private static bool IsTextureActive(ModEntry m)
        {
            if (m.IsExtractedArchive) return Directory.Exists(ArchiveActiveFolder(m.LibraryPath));
            return !IsDisabled(m.LibraryPath);
        }

        /// <summary>Top-level texturemods name the game indexes for an active pack
        /// (loadorder.txt). An archive is read via its extracted folder
        /// (&lt;name&gt;.extracted); a loose folder is read under its own name.</summary>
        private static string TexturePackFolderName(ModEntry m)
        {
            return m.IsExtractedArchive ? m.Name + ".extracted" : m.Name;
        }

        // --- apply ------------------------------------------------------------

        public class ApplyResult
        {
            public int Texture, Load, Fmv, Gameplay, Files;
            public bool LooseEnabled;
            public List<string> Warnings = new List<string>();
        }

        public ApplyResult Apply(bool looseFileSupport, Action<int, int, string> report, Func<bool> cancelled = null)
        {
            var result = new ApplyResult();

            // 1) Reconcile texture packs (enable checked / disable unchecked).
            var texMods = Mods.Where(m => m.Source == ModSource.TextureMods).ToList();
            foreach (var m in texMods)
            {
                bool activeNow = IsTextureActive(m);
                if (m.Enabled && !activeNow)
                {
                    if (report != null) report(0, 0, "Enabling " + m.Label);
                    if (!EnableTexture(m, report, cancelled))
                    {
                        m.Enabled = false;
                        result.Warnings.Add(m.Label + (cancelled != null && cancelled()
                                                       ? ": unpack cancelled, left off"
                                                       : ": enable failed"));
                    }
                }
                else if (!m.Enabled && activeNow)
                {
                    if (report != null) report(0, 0, "Disabling " + m.Label);
                    DisableTexture(m);
                }
            }

            // 2) loadorder.txt for the active texture packs, highest priority first.
            var active = Mods.Where(m => m.Source == ModSource.TextureMods && m.Enabled).ToList();
            if (active.Count > 0)
            {
                Directory.CreateDirectory(TexturemodsDir);
                File.WriteAllLines(LoadOrderPath, active.Select(TexturePackFolderName));
                _config.Set("texture_packs", "1");
                result.Texture = active.Count;
            }
            else if (File.Exists(LoadOrderPath))
            {
                try { File.Delete(LoadOrderPath); } catch { }
            }

            // 3) Deploy library mods (Gameplay, Load, FMV, TotalConversion, Preset)
            if (report != null) report(0, 0, "Deploying mods…");
            Undeploy();
            var manifest = new List<string>();

            // Gameplay & TotalConversion Mods (Code & DLLs)
            var codeMods = Mods.Where(m => m.Source == ModSource.Library && m.Enabled && 
                                         (m.Type == ModType.Gameplay || m.Type == ModType.TotalConversion)).ToList();
            foreach (var m in Enumerable.Reverse(codeMods))
            {
                try
                {
                    result.Files += CopyGameplayTracked(m.LibraryPath, _gameRoot, manifest, result.Warnings);
                    result.Gameplay++;

                    string loadSub = FindDirNamed(m.LibraryPath, "load");
                    if (loadSub != null && Directory.Exists(loadSub))
                    {
                        result.Files += CopyTreeTracked(loadSub, LoadDir, manifest);
                        result.Load++;
                    }

                    string fmvSub = FindDirNamed(m.LibraryPath, "FMV");
                    if (fmvSub != null && Directory.Exists(fmvSub))
                    {
                        result.Files += CopyVideosFlat(fmvSub, FmvDir, manifest);
                        result.Fmv++;
                    }
                }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
            }

            // Data Overlay (Load) Mods
            var loadMods = Mods.Where(m => m.Source == ModSource.Library && m.Enabled && m.Type == ModType.Load).ToList();
            foreach (var m in Enumerable.Reverse(loadMods))
            {
                try { result.Files += CopyTreeTracked(DeploySourceRoot(m.LibraryPath, ModType.Load), LoadDir, manifest); result.Load++; }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
            }

            // FMV Mods
            var fmvMods = Mods.Where(m => m.Source == ModSource.Library && m.Enabled && m.Type == ModType.Fmv).ToList();
            foreach (var m in Enumerable.Reverse(fmvMods))
            {
                try { result.Files += CopyVideosFlat(m.LibraryPath, FmvDir, manifest); result.Fmv++; }
                catch (Exception ex) { result.Warnings.Add(m.Label + ": " + ex.Message); }
            }

            WriteManifest(manifest);

            bool loose = looseFileSupport || loadMods.Count > 0 || codeMods.Count > 0;
            _config.Set("allow_loose_files", loose ? "1" : "0");

            _config.Save();
            result.LooseEnabled = loose;

            SaveState();
            return result;
        }

        // --- deploy plumbing (load/FMV/gameplay) -----------------------------

        private void Undeploy()
        {
            if (File.Exists(ManifestPath))
            {
                foreach (var line in File.ReadAllLines(ManifestPath))
                {
                    if (line.Length < 3 || line[1] != '|') continue;
                    char op = line[0];
                    string rel = line.Substring(2);
                    string full = Path.Combine(_gameRoot, rel);
                    try
                    {
                        if (op == 'D' && Directory.Exists(full))
                        {
                            Directory.Delete(full, true);
                        }
                        else if (op == 'F' && File.Exists(full))
                        {
                            File.Delete(full);
                        }
                        else if (op == 'B') // Backed up file: restore from .backup/
                        {
                            string backupFile = Path.Combine(BackupDir, rel);
                            if (File.Exists(backupFile))
                            {
                                Directory.CreateDirectory(Path.GetDirectoryName(full));
                                File.Copy(backupFile, full, true);
                                File.Delete(backupFile);
                            }
                            else if (File.Exists(full))
                            {
                                File.Delete(full);
                            }
                        }
                    }
                    catch { }
                }
                try { File.Delete(ManifestPath); } catch { }
            }

            if (Directory.Exists(BackupDir))
            {
                try { Directory.Delete(BackupDir, true); } catch { }
            }

            PruneEmptyDirs(LoadDir);
            PruneEmptyDirs(FmvDir);
            PruneEmptyDirs(Path.Combine(_gameRoot, "maps"));
            PruneEmptyDirs(Path.Combine(_gameRoot, "plugins"));
        }

        private void WriteManifest(List<string> manifest)
        {
            Directory.CreateDirectory(ModsDir);
            File.WriteAllLines(ManifestPath, manifest);
        }

        private string Rel(string full)
        {
            string root = _gameRoot.EndsWith("\\") ? _gameRoot : _gameRoot + "\\";
            return full.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? full.Substring(root.Length) : full;
        }

        /// <summary>Returns false if cancelled part-way (the caller deletes <paramref name="dest"/>).
        /// The probe is checked between entries, never during one, so no file is left truncated.</summary>
        private static bool ExtractZip(string zip, string dest, Action<int, int, string> report,
                                       Func<bool> cancelled = null)
        {
            using (var za = ZipFile.OpenRead(zip))
            {
                int total = za.Entries.Count, i = 0;
                string fullDest = Path.GetFullPath(dest);
                Directory.CreateDirectory(dest);
                foreach (var entry in za.Entries)
                {
                    if (cancelled != null && cancelled()) return false;
                    i++;
                    if (report != null) report(i, total, string.Format("Extracting {0}  ({1}/{2})",
                        Path.GetFileName(zip), i, total));
                    string outPath = Path.GetFullPath(Path.Combine(dest, entry.FullName));
                    if (!outPath.StartsWith(fullDest, StringComparison.OrdinalIgnoreCase)) continue; // zip-slip guard
                    if (string.IsNullOrEmpty(entry.Name)) { Directory.CreateDirectory(outPath); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(outPath));
                    entry.ExtractToFile(outPath, true);
                }
            }
            return true;
        }

        /// <summary>Returns false if cancelled part-way (checked between files, so no file is
        /// left half-copied); the caller deletes <paramref name="dst"/>.</summary>
        private static bool CopyTree(string src, string dst, Action<int, int, string> report = null,
                                     string label = null, Func<bool> cancelled = null)
        {
            Directory.CreateDirectory(dst);
            foreach (var dir in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(dir.Replace(src, dst));
            var files = Directory.GetFiles(src, "*", SearchOption.AllDirectories);
            for (int i = 0; i < files.Length; i++)
            {
                if (cancelled != null && cancelled()) return false;
                if (report != null) report(i + 1, files.Length, string.Format("{0}  ({1}/{2})", label ?? "Copying", i + 1, files.Length));
                File.Copy(files[i], files[i].Replace(src, dst), true);
            }
            return true;
        }

        /// <summary>Deploy a gameplay mod's DLLs. WHITELISTED: only maps/*.dll ever
        /// reaches the game folder -- a gameplay mod must never be able to overwrite
        /// the game or launcher executables, config, or anything else in the root.
        /// (A plugins/ runtime channel was reviewed and cut: no consumer, pure
        /// attack surface.) load/ and FMV/ content is deployed by its own tracked
        /// copiers; everything not on the whitelist is skipped and reported.</summary>
        private int CopyGameplayTracked(string src, string dstRoot, List<string> manifest, List<string> warnings)
        {
            int n = 0;
            Directory.CreateDirectory(BackupDir);
            foreach (var file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string rel = file.Substring(src.Length).TrimStart('\\', '/');
                string relLower = rel.ToLowerInvariant().Replace('\\', '/');

                if (relLower == "modmanager.json" || relLower == "mod.json" || relLower == "readme.txt")
                    continue;
                if (relLower.StartsWith("load/") || relLower.StartsWith("fmv/"))
                    continue; // deployed by the load/FMV copiers

                bool inMaps    = relLower.StartsWith("maps/");
                bool inPlugins = relLower.StartsWith("plugins/");
                bool isDll     = relLower.EndsWith(".dll");
                if (relLower.Contains("..") || !(inMaps || inPlugins) || !isDll)
                {
                    warnings.Add("skipped (only maps/ and plugins/ DLLs deploy): " + rel);
                    continue;
                }
                if (relLower.Count(c => c == '/') != 1)
                {
                    warnings.Add("skipped (nested path): " + rel);
                    continue;
                }

                string dst = Path.Combine(dstRoot, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));

                if (File.Exists(dst))
                {
                    string backupFile = Path.Combine(BackupDir, rel);
                    if (!File.Exists(backupFile))
                    {
                        Directory.CreateDirectory(Path.GetDirectoryName(backupFile));
                        File.Copy(dst, backupFile, true);
                    }
                    manifest.Add("B|" + Rel(dst));
                }
                else
                {
                    manifest.Add("F|" + Rel(dst));
                }

                File.Copy(file, dst, true);
                n++;
            }
            return n;
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

        private int CopyVideosFlat(string src, string dstRoot, List<string> manifest)
        {
            int n = 0;
            Directory.CreateDirectory(dstRoot);
            foreach (var file in SafeFiles(src))
            {
                if (!IsVideoFile(file)) continue;
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
                try { if (!Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir, false); }
                catch { }
            }
        }

        public void OpenModsFolder()
        {
            Directory.CreateDirectory(ModsDir);
            try { System.Diagnostics.Process.Start("explorer.exe", "\"" + ModsDir + "\""); } catch { }
        }

        public void OpenTextureModsFolder()
        {
            Directory.CreateDirectory(TexturemodsDir);
            try { System.Diagnostics.Process.Start("explorer.exe", "\"" + TexturemodsDir + "\""); } catch { }
        }
    }
}
