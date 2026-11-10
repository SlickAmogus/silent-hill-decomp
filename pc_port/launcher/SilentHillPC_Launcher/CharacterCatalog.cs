using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
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
    public class CatalogCharacter
    {
        [DataMember(Name = "id")]       public string Id;
        [DataMember(Name = "name")]     public string Name;
        [DataMember(Name = "category")] public string Category;
        [DataMember(Name = "author")]   public string Author;
        [DataMember(Name = "version")]  public string Version;
        [DataMember(Name = "file")]     public string File;
        [DataMember(Name = "hero")]     public string Hero;
        [DataMember(Name = "size")]     public long   Size;
        [DataMember(Name = "sha256")]   public string Sha256;
    }

    [DataContract]
    public class CharacterCatalogFile
    {
        [DataMember(Name = "schema")]     public int Schema;
        [DataMember(Name = "characters")] public List<CatalogCharacter> Characters;
    }

    /// <summary>
    /// Additional Characters: characters.json on the qol-mods release, one zip per
    /// character holding load/CHARA/HERO_*.glb + .ini. The game's own Additional
    /// Characters menu reads the same catalog and writes the same record,
    /// gamedata/chara_downloads.txt (id, version, sha256, hero; tab-separated), so
    /// a character installed in either place shows as installed in the other.
    /// Recorded files live directly in gamedata/load/CHARA and win over any mod's
    /// copy of the same name: the deploy plan leaves them alone.
    /// </summary>
    public static class CharacterCatalog
    {
        public const string BaseUrl = "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/";
        public const string CatalogFile = "characters.json";

        public class Record
        {
            public string Id, Version, Sha256, Hero;
        }

        /// <summary>Before anything is uploaded: gamedata/chara_catalog_test holding
        /// characters.json and the zips stands in for the release (the game reads it too).</summary>
        public static string TestDir(string gameRoot)
        {
            string d = Path.Combine(gameRoot, "gamedata", "chara_catalog_test");
            return Directory.Exists(d) ? d : null;
        }

        public static async Task<CharacterCatalogFile> LoadAsync(string modsDir, CancellationToken ct)
        {
            string cache = Path.Combine(modsDir, CatalogFile);
            string test = TestDir(Path.GetDirectoryName(modsDir.TrimEnd('\\', '/')));
            if (test != null && System.IO.File.Exists(Path.Combine(test, CatalogFile)))
                return Parse(System.IO.File.ReadAllText(Path.Combine(test, CatalogFile)));
            try
            {
                string json = await UpdateChecker.FetchTextAsync(BaseUrl + CatalogFile, ct).ConfigureAwait(false);
                var cat = Parse(json);
                if (cat != null)
                {
                    try { Directory.CreateDirectory(modsDir); System.IO.File.WriteAllText(cache, json); } catch { }
                    return cat;
                }
            }
            catch { }
            try { return System.IO.File.Exists(cache) ? Parse(System.IO.File.ReadAllText(cache)) : null; }
            catch { return null; }
        }

        private static bool SafeName(string s)
        {
            return !string.IsNullOrEmpty(s) && s.IndexOfAny(new[] { '/', '\\' }) < 0 && !s.Contains("..");
        }

        private static CharacterCatalogFile Parse(string json)
        {
            using (var ms = new MemoryStream(Encoding.UTF8.GetBytes(json)))
            {
                var cat = (CharacterCatalogFile)new DataContractJsonSerializer(typeof(CharacterCatalogFile)).ReadObject(ms);
                if (cat == null || cat.Characters == null) return null;
                cat.Characters = cat.Characters.Where(c => c != null && !string.IsNullOrEmpty(c.Id) &&
                                                           SafeName(c.File) && SafeName(c.Hero)).ToList();
                foreach (var c in cat.Characters)
                {
                    if (string.IsNullOrEmpty(c.Category)) c.Category = "Other";
                    if (string.IsNullOrEmpty(c.Name)) c.Name = c.Hero;
                }
                return cat;
            }
        }

        private static string RecordsPath(string gameRoot)
        {
            return Path.Combine(gameRoot, "gamedata", "chara_downloads.txt");
        }

        private static string CharaDir(string gameRoot)
        {
            return Path.Combine(gameRoot, "gamedata", "load", "CHARA");
        }

        /// <summary>Downloads live in their own folder, after load\CHARA in the game's
        /// load order: a mod's or a hand-copied file of the same name is the one that
        /// loads. Only jojo670's old unversioned pack gives way (see the deploy plan).</summary>
        private static string DlDir(string gameRoot)
        {
            return Path.Combine(gameRoot, "gamedata", "load", "CHARA_dl");
        }

        /// <summary>Downloads went into load\CHARA before the load order. A recorded
        /// file there is the download's (the deploy plan kept mods off it), so it moves.</summary>
        private static void MigrateDownloads(string gameRoot, List<Record> recs)
        {
            foreach (var r in recs)
            {
                string to = Path.Combine(DlDir(gameRoot), r.Hero + ".glb");
                string from = Path.Combine(CharaDir(gameRoot), r.Hero + ".glb");
                if (System.IO.File.Exists(to) || !System.IO.File.Exists(from)) continue;
                try
                {
                    Directory.CreateDirectory(DlDir(gameRoot));
                    foreach (var ext in new[] { ".glb", ".ini" })
                    {
                        string f = Path.Combine(CharaDir(gameRoot), r.Hero + ext);
                        if (System.IO.File.Exists(f)) System.IO.File.Move(f, Path.Combine(DlDir(gameRoot), r.Hero + ext));
                    }
                }
                catch { }
            }
        }

        /// <summary>QoL character packs the game has moved into Additional Characters
        /// (gamedata\chara_migrated.txt, one Essential id per line): their characters are
        /// downloads now, so the pack itself leaves the library and is never updated
        /// again. Returns whether anything was removed.</summary>
        public static bool DropMigratedPacks(ModManager mgr)
        {
            string p = Path.Combine(mgr.GameRoot, "gamedata", "chara_migrated.txt");
            if (!System.IO.File.Exists(p)) return false;
            var ids = new HashSet<string>(System.IO.File.ReadAllLines(p).Select(l => l.Trim()).Where(l => l.Length > 0),
                                          StringComparer.OrdinalIgnoreCase);
            var gone = mgr.Mods.Where(m => !string.IsNullOrEmpty(m.EssentialId) && ids.Contains(m.EssentialId)).ToList();
            foreach (var m in gone) mgr.DeleteLibrary(m);
            if (gone.Count > 0) mgr.SaveState();
            return gone.Count > 0;
        }

        public static List<Record> ReadRecords(string gameRoot)
        {
            var list = ReadRecordsRaw(gameRoot);
            MigrateDownloads(gameRoot, list);
            return list;
        }

        private static List<Record> ReadRecordsRaw(string gameRoot)
        {
            var list = new List<Record>();
            string p = RecordsPath(gameRoot);
            if (!System.IO.File.Exists(p)) return list;
            foreach (var line in System.IO.File.ReadAllLines(p))
            {
                if (line.Length == 0 || line[0] == '#') continue;
                var t = line.Split('\t');
                if (t.Length < 4) continue;
                list.Add(new Record { Id = t[0], Version = t[1], Sha256 = t[2], Hero = t[3].Trim() });
            }
            return list;
        }

        private static void WriteRecords(string gameRoot, List<Record> recs)
        {
            var sb = new StringBuilder();
            sb.Append("# id\tversion\tsha256\thero  (written by the game and the launcher)\n");
            foreach (var r in recs) sb.Append(r.Id).Append('\t').Append(r.Version).Append('\t').Append(r.Sha256)
                                      .Append('\t').Append(r.Hero).Append('\n');
            Directory.CreateDirectory(Path.GetDirectoryName(RecordsPath(gameRoot)));
            System.IO.File.WriteAllText(RecordsPath(gameRoot), sb.ToString());
        }

        /// <summary>gamedata-relative paths (gamedata\load\CHARA\HERO_X.glb/.ini) a
        /// downloaded character would be shadowed by, for the deploy plan to keep jojo670's
        /// old pack off.</summary>
        public static HashSet<string> DownloadedRels(string gameRoot)
        {
            var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var r in ReadRecords(gameRoot))
            {
                set.Add(Path.Combine("gamedata", "load", "CHARA", r.Hero + ".glb"));
                set.Add(Path.Combine("gamedata", "load", "CHARA", r.Hero + ".ini"));
            }
            return set;
        }

        public static bool IsInstalled(string gameRoot, CatalogCharacter c, List<Record> recs = null)
        {
            recs = recs ?? ReadRecords(gameRoot);
            return recs.Any(r => string.Equals(r.Id, c.Id, StringComparison.OrdinalIgnoreCase) &&
                                 System.IO.File.Exists(Path.Combine(DlDir(gameRoot), r.Hero + ".glb")));
        }

        public static bool NeedsUpdate(CatalogCharacter c, List<Record> recs)
        {
            var r = recs.FirstOrDefault(x => string.Equals(x.Id, c.Id, StringComparison.OrdinalIgnoreCase));
            return r != null && (!string.Equals(r.Version, c.Version ?? "1") ||
                                 (!string.IsNullOrEmpty(c.Sha256) && r.Sha256 != "-" &&
                                  !string.Equals(r.Sha256, c.Sha256, StringComparison.OrdinalIgnoreCase)));
        }

        /// <summary>Download, verify and unpack one character into gamedata/load/CHARA,
        /// then record it. False if cancelled; throws on a failed download.</summary>
        public static bool Install(string gameRoot, CatalogCharacter c, Action<int, int, string> report, Func<bool> cancelled)
        {
            string tmpDir = Path.Combine(Path.GetTempPath(), "sh-chara-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(tmpDir);
            try
            {
                string zip = Path.Combine(tmpDir, c.File);
                string test = TestDir(gameRoot);
                if (test != null && System.IO.File.Exists(Path.Combine(test, c.File)))
                    System.IO.File.Copy(Path.Combine(test, c.File), zip, true);
                else
                using (var cts = new CancellationTokenSource())
                {
                    try
                    {
                        UpdateChecker.DownloadFileAsync(BaseUrl + c.File, zip, (frac, text) =>
                        {
                            if (cancelled()) cts.Cancel();
                            report((int)(frac * 1000), 1000, Loc.F("Downloading {0}  ({1})", c.Name, text));
                        }, cts.Token).GetAwaiter().GetResult();
                    }
                    catch (OperationCanceledException) { return false; }
                }
                if (cancelled()) return false;
                if (!string.IsNullOrEmpty(c.Sha256))
                {
                    string got;
                    using (var sha = SHA256.Create())
                    using (var fs = System.IO.File.OpenRead(zip))
                        got = BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "").ToLowerInvariant();
                    if (got != c.Sha256.ToLowerInvariant())
                        throw new Exception(Loc.F("{0}: the download is damaged (checksum mismatch). Try again.", c.Name));
                }

                string dir = DlDir(gameRoot);
                Directory.CreateDirectory(dir);
                string hero = c.Hero;
                using (var za = ZipFile.OpenRead(zip))
                {
                    foreach (var e in za.Entries)
                    {
                        string name = e.FullName.Replace('\\', '/');
                        if (e.Length == 0 && name.EndsWith("/")) continue;
                        int at = name.IndexOf("load/CHARA/", StringComparison.OrdinalIgnoreCase);
                        if (at < 0 || name.Contains("..")) continue;
                        string leaf = name.Substring(at + "load/CHARA/".Length);
                        if (leaf.Length == 0 || leaf.Contains("/")) continue;
                        e.ExtractToFile(Path.Combine(dir, leaf), true);
                        if (leaf.EndsWith(".glb", StringComparison.OrdinalIgnoreCase))
                            hero = Path.GetFileNameWithoutExtension(leaf);
                    }
                }

                var recs = ReadRecords(gameRoot);
                // An update that renamed the model leaves no stray file behind.
                foreach (var old in recs.Where(r => string.Equals(r.Id, c.Id, StringComparison.OrdinalIgnoreCase) &&
                                                    !string.Equals(r.Hero, hero, StringComparison.OrdinalIgnoreCase)))
                    foreach (var ext in new[] { ".glb", ".ini" })
                    {
                        string p = Path.Combine(dir, old.Hero + ext);
                        try { if (System.IO.File.Exists(p)) System.IO.File.Delete(p); } catch { }
                    }
                recs.RemoveAll(r => string.Equals(r.Id, c.Id, StringComparison.OrdinalIgnoreCase));
                recs.Add(new Record { Id = c.Id, Version = string.IsNullOrEmpty(c.Version) ? "1" : c.Version,
                                      Sha256 = string.IsNullOrEmpty(c.Sha256) ? "-" : c.Sha256, Hero = hero });
                WriteRecords(gameRoot, recs);
                return true;
            }
            finally
            {
                try { Directory.Delete(tmpDir, true); } catch { }
            }
        }

        public static void Remove(string gameRoot, CatalogCharacter c)
        {
            var recs = ReadRecords(gameRoot);
            foreach (var r in recs.Where(r => string.Equals(r.Id, c.Id, StringComparison.OrdinalIgnoreCase)))
            {
                foreach (var ext in new[] { ".glb", ".ini" })
                {
                    string p = Path.Combine(DlDir(gameRoot), r.Hero + ext);
                    try { if (System.IO.File.Exists(p)) System.IO.File.Delete(p); } catch { }
                }
            }
            recs.RemoveAll(r => string.Equals(r.Id, c.Id, StringComparison.OrdinalIgnoreCase));
            WriteRecords(gameRoot, recs);
        }
    }
}
