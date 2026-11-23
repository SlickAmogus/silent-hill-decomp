using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SilentHillPC_Launcher
{
    [DataContract]
    public class EssentialMod
    {
        [DataMember(Name = "id")]          public string Id;
        [DataMember(Name = "name")]        public string Name;
        [DataMember(Name = "author")]      public string Author;
        [DataMember(Name = "description")] public string Description;
        [DataMember(Name = "file")]        public string File;
        [DataMember(Name = "url")]         public string Url;
        [DataMember(Name = "size")]        public long   Size;
        [DataMember(Name = "sha256")]      public string Sha256;
        [DataMember(Name = "kind")]        public string Kind; /* "character" = a HERO_* playable character */
        /* One "crc32 size path" line per file under load/, or absent. */
        [DataMember(Name = "verify")]      public string Verify;

        public bool IsCharacter
        {
            get { return string.Equals(Kind, "character", StringComparison.OrdinalIgnoreCase); }
        }
    }

    [DataContract]
    public class EssentialCatalog
    {
        [DataMember(Name = "schema")] public int Schema;
        [DataMember(Name = "mods")]   public List<EssentialMod> Mods;
    }

    /// <summary>
    /// The "Essential QoL Mods" catalog: a fixed GitHub release (tag qol-mods) holding
    /// the mod zips and essential-mods.json describing them. Android, iOS and the web
    /// build read the same catalog, so adding a mod there is only an upload. Each zip
    /// also carries a modinfo.txt, so an installed mod keeps its name, author and
    /// description even after the catalog changes.
    /// </summary>
    public static class EssentialMods
    {
        public const string CatalogUrl =
            "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/essential-mods.json";

        /// <summary>The catalog, or the last copy fetched when GitHub can't be reached
        /// (so the section still lists what is installed). Null if neither exists.</summary>
        public static async Task<EssentialCatalog> LoadAsync(string modsDir, CancellationToken ct)
        {
            string cache = Path.Combine(modsDir, "essential-mods.json");
            try
            {
                string json = await UpdateChecker.FetchTextAsync(CatalogUrl, ct).ConfigureAwait(false);
                var cat = Parse(json);
                if (cat != null)
                {
                    try { Directory.CreateDirectory(modsDir); File.WriteAllText(cache, json); } catch { }
                    return cat;
                }
            }
            catch { }
            try { return System.IO.File.Exists(cache) ? Parse(System.IO.File.ReadAllText(cache)) : null; }
            catch { return null; }
        }

        private static EssentialCatalog Parse(string json)
        {
            using (var ms = new MemoryStream(Encoding.UTF8.GetBytes(json)))
            {
                var cat = (EssentialCatalog)new DataContractJsonSerializer(typeof(EssentialCatalog)).ReadObject(ms);
                if (cat == null || cat.Mods == null) return null;
                cat.Mods = cat.Mods.Where(m => m != null && !string.IsNullOrEmpty(m.Url) &&
                                               !string.IsNullOrEmpty(m.File)).ToList();
                return cat;
            }
        }

        /// <summary>The installed mod this catalog entry became. First the one the Mod
        /// Manager itself installed for it (its saved state remembers the catalog id),
        /// then a copy downloaded by hand: matched by the id in its modinfo.txt, else by
        /// the name the download installs under. A mod installed for another catalog
        /// entry, or already matched (taken), never matches.</summary>
        public static ModEntry FindInstalled(EssentialMod em, IEnumerable<ModEntry> mods,
                                             ICollection<ModEntry> taken = null)
        {
            string folder = Path.GetFileNameWithoutExtension(em.File);
            var free = mods.Where(m => (taken == null || !taken.Contains(m)) &&
                                       (string.IsNullOrEmpty(m.EssentialId) ||
                                        string.Equals(m.EssentialId, em.Id, StringComparison.OrdinalIgnoreCase)))
                           .ToList();
            return free.FirstOrDefault(m => string.Equals(m.EssentialId, em.Id, StringComparison.OrdinalIgnoreCase))
                ?? free.FirstOrDefault(m => !string.IsNullOrEmpty(m.ModInfoId) &&
                                            string.Equals(m.ModInfoId, em.Id, StringComparison.OrdinalIgnoreCase))
                ?? free.FirstOrDefault(m => string.Equals(m.Name, em.File, StringComparison.OrdinalIgnoreCase) ||
                                            string.Equals(m.Name, folder, StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>Download, verify and install one mod. Returns the name it is listed
        /// under, or null if cancelled. Throws on a download or checksum failure.</summary>
        public static string Install(EssentialMod em, ModManager mgr, Action<int, int, string> report, Func<bool> cancelled)
        {
            string tmpDir = Path.Combine(Path.GetTempPath(), "sh-qolmod-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(tmpDir);
            try
            {
                string zip = Path.Combine(tmpDir, em.File);
                using (var cts = new CancellationTokenSource())
                {
                    try
                    {
                        UpdateChecker.DownloadFileAsync(em.Url, zip, (frac, text) =>
                        {
                            if (cancelled()) cts.Cancel();
                            report((int)(frac * 1000), 1000, Loc.F("Downloading {0}  ({1})", em.Name, text));
                        }, cts.Token).GetAwaiter().GetResult();
                    }
                    catch (OperationCanceledException) { return null; }
                }
                if (cancelled()) return null;

                if (!string.IsNullOrEmpty(em.Sha256))
                {
                    string got;
                    using (var sha = SHA256.Create())
                    using (var fs = System.IO.File.OpenRead(zip))
                        got = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "").ToLowerInvariant();
                    if (got != em.Sha256.ToLowerInvariant())
                        throw new Exception(Loc.F("{0}: the download is damaged (checksum mismatch). Try again.", em.Name));
                }
                return mgr.InstallDownloadedZip(zip, em.File, report, cancelled);
            }
            finally
            {
                try { Directory.Delete(tmpDir, true); } catch { }
            }
        }

        /// <summary>The mods the Essential list installed whose catalog zip has changed
        /// since, so an updated mod (a pack that gains characters, a fixed model) reaches
        /// everyone who has it. Where the catalog lists a mod's files ("verify"), the
        /// installed copy itself is compared and the recorded sha256 only follows the
        /// answer. Otherwise the record decides, and one installed before it was kept is
        /// taken as current (and recorded, so the next change is seen).</summary>
        public static List<EssentialMod> StaleMods(ModManager mgr, EssentialCatalog cat)
        {
            var stale = new List<EssentialMod>();
            bool adopted = false;
            foreach (var em in cat.Mods.Where(m => !string.IsNullOrEmpty(m.Sha256)))
            {
                var inst = mgr.Mods.FirstOrDefault(m => string.Equals(m.EssentialId, em.Id, StringComparison.OrdinalIgnoreCase));
                if (inst == null) continue;
                bool same = string.Equals(inst.EssentialSha, em.Sha256, StringComparison.OrdinalIgnoreCase);
                bool? files = VerifyInstalled(em, inst);
                if (files == false) stale.Add(em);
                else if (files == true || string.IsNullOrEmpty(inst.EssentialSha))
                {
                    if (!same) { inst.EssentialSha = em.Sha256; adopted = true; }
                }
                else if (!same) stale.Add(em);
            }
            if (adopted) mgr.SaveState();
            return stale;
        }

        /// <summary>The catalog's "verify" lines ("crc32 size path", one per file) against
        /// a mod in the library: true when every file is the catalog's, false when one
        /// differs or is missing, null when there is nothing to go by (no lines, or the
        /// mod is not a folder in mods/). Files are looked for at the mod's root and one
        /// folder down, as modinfo.txt is.</summary>
        public static bool? VerifyInstalled(EssentialMod em, ModEntry inst)
        {
            if (string.IsNullOrWhiteSpace(em.Verify) || inst.Source != ModSource.Library ||
                string.IsNullOrEmpty(inst.LibraryPath) || !Directory.Exists(inst.LibraryPath))
                return null;
            var roots = new List<string> { inst.LibraryPath };
            try { roots.AddRange(Directory.GetDirectories(inst.LibraryPath)); } catch { }

            int lines = 0;
            foreach (var raw in em.Verify.Split('\n'))
            {
                string[] parts = raw.Trim().Split(new[] { ' ' }, 3);
                uint crc; long size;
                if (parts.Length != 3 ||
                    !uint.TryParse(parts[0], System.Globalization.NumberStyles.HexNumber,
                                   System.Globalization.CultureInfo.InvariantCulture, out crc) ||
                    !long.TryParse(parts[1], out size))
                    continue;
                string rel = parts[2].Trim();
                // The path comes off the network and is joined to a folder.
                if (rel.Length == 0 || rel.Contains("..") || rel.Contains(":") || rel.StartsWith("/") || rel.Contains("\\"))
                    return null;
                lines++;
                string found = roots.Select(r => Path.Combine(r, rel.Replace('/', Path.DirectorySeparatorChar)))
                                    .FirstOrDefault(System.IO.File.Exists);
                if (found == null) return false;
                try
                {
                    if (new FileInfo(found).Length != size || Crc32(found) != crc) return false;
                }
                catch { return false; }
            }
            return lines > 0 ? (bool?)true : null;
        }

        private static uint[] _crcTable;

        private static uint Crc32(string path)
        {
            if (_crcTable == null)
            {
                var t = new uint[256];
                for (uint n = 0; n < 256; n++)
                {
                    uint c = n;
                    for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                _crcTable = t;
            }
            uint crc = 0xFFFFFFFFu;
            var buf = new byte[65536];
            using (var fs = System.IO.File.OpenRead(path))
            {
                int got;
                while ((got = fs.Read(buf, 0, buf.Length)) > 0)
                    for (int i = 0; i < got; i++)
                        crc = _crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
            }
            return crc ^ 0xFFFFFFFFu;
        }

        /// <summary>Download those again and deploy them. Returns the names updated; one
        /// that fails keeps its old sha256, so the next game update tries it again.</summary>
        public static List<string> UpdateMods(ModManager mgr, List<EssentialMod> stale, bool looseFiles,
                                              List<string> failures, Action<int, int, string> report,
                                              Func<bool> cancelled)
        {
            var done = new List<EssentialMod>();
            var wasOn = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            foreach (var em in stale)
            {
                if (cancelled()) break;
                try
                {
                    // A texture pack is unpacked beside its zip, and a disabled one keeps that
                    // copy to switch back on; either would outlive the new zip. Its on/off
                    // is read back from that copy, so it is remembered here and Apply
                    // unpacks the new zip again.
                    var inst = mgr.Mods.FirstOrDefault(x => string.Equals(x.EssentialId, em.Id, StringComparison.OrdinalIgnoreCase));
                    if (inst != null && inst.Type == ModType.Texturemods)
                    {
                        wasOn[em.Id] = inst.Enabled;
                        mgr.DeleteTexture(inst);
                    }
                    if (Install(em, mgr, report, cancelled) != null) done.Add(em);
                }
                catch (Exception ex) { failures.Add(em.Name + ": " + ex.Message); }
            }
            if (done.Count == 0) return new List<string>();
            mgr.Scan();
            foreach (var em in done)
            {
                var m = mgr.Mods.FirstOrDefault(x => string.Equals(x.EssentialId, em.Id, StringComparison.OrdinalIgnoreCase));
                if (m == null) continue;
                m.EssentialSha = em.Sha256;
                bool on;
                if (wasOn.TryGetValue(em.Id, out on)) m.Enabled = on;
            }
            mgr.SaveState();
            mgr.Apply(looseFiles, report, cancelled);
            return done.Select(em => em.Name).ToList();
        }
    }
}
