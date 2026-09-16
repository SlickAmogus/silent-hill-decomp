/* SPDX-License-Identifier: GPL-3.0-or-later */
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>One occurrence of a sample body in some bank on disk.</summary>
    internal sealed class SampleRef
    {
        public string Bank;    // file stem, as the loose-file loader keys it
        public int Index;      // 1-based sample index within that bank
        public string Path;    // the file the body was read from
        public int Length;
    }

    /// <summary>Where else every sample body in a set of banks occurs.
    ///
    /// The disc does not share sounds between banks by reference: a monster's cries
    /// are pasted, byte for byte, into the ambient bank of every map it appears in
    /// (the Groaner block sits in eight of them). The game loads one ambient bank per
    /// map, so a replacement made in MAP200 is silent in the school, which loads
    /// MAP201 and MAP001_2. Keying on the ADPCM bytes rather than on names finds
    /// those copies without needing to know what any sound is.</summary>
    internal sealed class SndSampleIndex
    {
        private readonly Dictionary<string, List<SampleRef>> _byKey = new Dictionary<string, List<SampleRef>>();
        private readonly HashSet<string> _banks = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        public readonly List<string> Dirs = new List<string>();
        public int BankCount { get { return _banks.Count; } }

        /// <summary>Length plus a digest of the body. Two samples of different length
        /// cannot be the same sound, and the length keeps a digest collision from ever
        /// mattering in practice.</summary>
        public static string Key(byte[] data, int offset, int length)
        {
            using (var md5 = MD5.Create())
            {
                byte[] h = md5.ComputeHash(data, offset, length);
                var sb = new StringBuilder(length.ToString());
                sb.Append(':');
                foreach (byte b in h) sb.Append(b.ToString("x2"));
                return sb.ToString();
            }
        }

        public static string Key(byte[] body)
        {
            return Key(body, 0, body.Length);
        }

        /// <summary>Index every .VAB in the given folders. Folders are scanned in order
        /// and the FIRST copy of a bank name wins, so pass the pristine extract before
        /// any folder that may hold edited copies: a half-modified bank still matches
        /// on the samples it kept, but the original is what the other banks were
        /// copied from.</summary>
        public static SndSampleIndex Build(IEnumerable<string> dirs)
        {
            var idx = new SndSampleIndex();
            foreach (string dir in dirs)
            {
                if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) continue;
                string full;
                try { full = Path.GetFullPath(dir); } catch { continue; }
                if (idx.Dirs.Contains(full)) continue;
                idx.Dirs.Add(full);

                string[] files;
                try { files = Directory.GetFiles(full, "*.vab"); } catch { continue; }
                Array.Sort(files, StringComparer.OrdinalIgnoreCase);

                foreach (string f in files)
                {
                    string stem = Path.GetFileNameWithoutExtension(f);
                    // The seven MAP twins of the MEP banks are never requested by the
                    // game, so telling a modder to update them would send them to a
                    // file that does nothing. Their MEP copies are listed instead.
                    if (VabFile.IsMapOnlyBank(stem)) continue;
                    if (idx._banks.Contains(stem)) continue;

                    string err;
                    VabFile v = VabFile.Load(f, out err);
                    if (v == null) continue;
                    idx._banks.Add(stem);

                    foreach (VabVag vag in v.Vags)
                    {
                        if (vag.Length <= 0) continue;
                        string key = Key(v.Raw, vag.Offset, vag.Length);
                        List<SampleRef> list;
                        if (!idx._byKey.TryGetValue(key, out list))
                        {
                            list = new List<SampleRef>();
                            idx._byKey[key] = list;
                        }
                        list.Add(new SampleRef { Bank = stem, Index = vag.Index, Path = f, Length = vag.Length });
                    }
                }
            }
            return idx;
        }

        /// <summary>Every other bank holding this exact body. The bank the body came
        /// from is left out; a bank that repeats a sample internally is not a
        /// "duplicate elsewhere" in the sense a modder cares about.</summary>
        public List<SampleRef> Others(byte[] body, string excludeBank)
        {
            var result = new List<SampleRef>();
            if (body == null || body.Length == 0) return result;
            List<SampleRef> list;
            if (!_byKey.TryGetValue(Key(body), out list)) return result;
            foreach (SampleRef r in list)
            {
                if (string.Equals(r.Bank, excludeBank, StringComparison.OrdinalIgnoreCase)) continue;
                result.Add(r);
            }
            return result;
        }

        public bool SameDirs(IEnumerable<string> dirs)
        {
            var want = new List<string>();
            foreach (string d in dirs)
            {
                if (string.IsNullOrEmpty(d) || !Directory.Exists(d)) continue;
                string full;
                try { full = Path.GetFullPath(d); } catch { continue; }
                if (!want.Contains(full)) want.Add(full);
            }
            if (want.Count != Dirs.Count) return false;
            for (int i = 0; i < want.Count; i++)
                if (!string.Equals(want[i], Dirs[i], StringComparison.OrdinalIgnoreCase)) return false;
            return true;
        }
    }

    /// <summary>One sample in a target bank that should receive a replacement body.</summary>
    internal sealed class DuplicateItem
    {
        public int TargetIndex;    // sample index in the target bank
        public int SourceIndex;    // sample index in the bank the user edited
        public byte[] Body;        // the replacement, already encoded
        public string OriginalKey; // key of the body being replaced, to detect prior edits
    }

    /// <summary>A bank other than the one open in the tool that holds copies of one or
    /// more replaced samples, and the plan for rewriting it.</summary>
    internal sealed class DuplicateTarget
    {
        public string Bank;
        public string SourcePath;     // bank read as the basis for the rewrite
        public string DestPath;       // where the rewritten bank is written
        public readonly List<DuplicateItem> Items = new List<DuplicateItem>();
        public bool Selected = true;

        /// <summary>How many of the items still hold the original body in SourcePath.
        /// Fewer than all means the source has already been edited at those spots.
        /// -1 when the source cannot be read as a bank.</summary>
        public int CountUntouched(out string error)
        {
            error = null;
            VabFile v = VabFile.Load(SourcePath, out error);
            if (v == null) return -1;
            int n = 0;
            foreach (DuplicateItem it in Items)
            {
                if (it.TargetIndex < 1 || it.TargetIndex > v.VagCount) continue;
                VabVag vag = v.Vags[it.TargetIndex - 1];
                if (SndSampleIndex.Key(v.Raw, vag.Offset, vag.Length) == it.OriginalKey) n++;
            }
            return n;
        }

        public string IndexList()
        {
            var parts = new List<string>();
            foreach (DuplicateItem it in Items) parts.Add("#" + it.TargetIndex);
            return string.Join(", ", parts.ToArray());
        }

        /// <summary>Read SourcePath, drop the replacement bodies in, write DestPath.</summary>
        public bool Write(out string error)
        {
            VabFile v = VabFile.Load(SourcePath, out error);
            if (v == null) return false;

            var map = new Dictionary<int, byte[]>();
            foreach (DuplicateItem it in Items)
            {
                if (it.TargetIndex < 1 || it.TargetIndex > v.VagCount)
                {
                    error = Bank + " has only " + v.VagCount + " samples; #" + it.TargetIndex + " does not exist in " + SourcePath;
                    return false;
                }
                map[it.TargetIndex] = it.Body;
            }

            byte[] rebuilt = v.Rebuild(map, out error);
            if (rebuilt == null) return false;

            try
            {
                string dir = Path.GetDirectoryName(DestPath);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
                File.WriteAllBytes(DestPath, rebuilt);
            }
            catch (Exception ex)
            {
                error = "Could not write " + DestPath + ":\n" + ex.Message;
                return false;
            }
            return true;
        }
    }
}
