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

        /// <summary>The installed mod this catalog entry became: matched by the id in its
        /// modinfo.txt, else by the name the download installs under.</summary>
        public static ModEntry FindInstalled(EssentialMod em, IEnumerable<ModEntry> mods)
        {
            string folder = Path.GetFileNameWithoutExtension(em.File);
            return mods.FirstOrDefault(m => !string.IsNullOrEmpty(m.ModInfoId) &&
                                            string.Equals(m.ModInfoId, em.Id, StringComparison.OrdinalIgnoreCase))
                ?? mods.FirstOrDefault(m => string.Equals(m.Name, em.File, StringComparison.OrdinalIgnoreCase) ||
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
    }
}
