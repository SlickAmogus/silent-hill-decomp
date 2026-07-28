using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// PNG &lt;-&gt; BC7 .dds conversion for texture mods, via Microsoft's texconv
    /// (DirectXTex, MIT). texconv.exe is EMBEDDED in the launcher and materialized
    /// to a temp path on first use, then run as a separate PROCESS — the same
    /// pattern as SevenZipExtractor/7za.exe, so there is no managed-DLL load
    /// surface to fail on a user's machine.
    ///
    /// BC7 keeps a real 8-bit alpha (needed for the override shader's alpha&lt;0.5
    /// cutout — BC1/DXT1 would wreck it).
    ///
    /// MIP CHAINS ARE PER-ENTRY, not global. glGenerateMipmap is invalid on a
    /// compressed texture (it fails silently black), so a file that uploads
    /// COMPRESSED must carry its own chain — that is the whole-cover fast path in
    /// TexPack_Compose, and those get -m 0. Everything else is composited: the
    /// sub-rect blitter cannot paste BC7 blocks, so tex_pack.c hands the file to
    /// Dds_DecodeRgba, which decodes LEVEL 0 ONLY and ignores the rest. A mip chain
    /// on a sub-rect entry is dead weight — a measured 25% of the file — so those
    /// get -m 1. A single-level file is safe even if it somehow did reach the
    /// compressed uploader: dds_load.c pins GL_TEXTURE_MAX_LEVEL 0 and drops
    /// MIN_FILTER to the non-mipmap form, which renders correctly, just unfiltered
    /// at distance.
    ///
    /// PATH HANDLING: DuckStation packs pair ~110-char texupload-* hash names
    /// with users' deeply nested extract folders, so full paths routinely pass
    /// MAX_PATH, and a real pack holds 12000+ PNGs in ONE directory — batching
    /// them onto a texconv command line blows CreateProcess's 32K argument cap
    /// ("The filename or extension is too long"). Every conversion therefore
    /// goes through a short-temp shuffle: sources are copied (\\?\-prefixed
    /// File.Copy, immune to MAX_PATH) to sequential short names in %TEMP%,
    /// texconv runs THERE with relative arguments in bounded chunks, and each
    /// result is moved (\\?\ again) to its long destination beside the source.
    /// texconv itself never sees a user path.
    /// </summary>
    public static class DdsConverter
    {
        private const string EmbeddedName = "SilentHillPC_Launcher.texconv.exe";

        /// <summary>Files per texconv invocation. Temp names are ~14 chars, so a
        /// chunk's relative-arg command line stays a few KB — far under the 32K
        /// CreateProcess limit — while keeping process spawns low.</summary>
        private const int ChunkSize = 128;

        private static readonly object _lock = new object();
        private static bool   _tried;
        private static string _exePath;

        /// <summary>Materialize the embedded texconv.exe (once). Returns whether a
        /// usable texconv is available.</summary>
        public static bool IsAvailable()
        {
            return ResolveExe() != null;
        }

        private static string ResolveExe()
        {
            lock (_lock)
            {
                if (_tried) return _exePath;
                _tried = true;

                try
                {
                    string dir = Path.Combine(Path.GetTempPath(), "SilentHillPC_Launcher");
                    Directory.CreateDirectory(dir);
                    string path = Path.Combine(dir, "texconv.exe");

                    byte[] bytes = ReadEmbedded();
                    if (bytes != null)
                    {
                        bool needWrite = !File.Exists(path) || new FileInfo(path).Length != bytes.Length;
                        if (needWrite)
                        {
                            try { File.WriteAllBytes(path, bytes); }
                            catch { /* a prior run may hold it open; use what's there */ }
                        }
                        if (File.Exists(path)) { _exePath = path; return _exePath; }
                    }

                    // Fallbacks: a copy beside the launcher, or texconv on PATH.
                    string beside = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "texconv.exe");
                    if (File.Exists(beside)) { _exePath = beside; return _exePath; }
                    string onPath = FindOnPath("texconv.exe");
                    if (onPath != null) { _exePath = onPath; return _exePath; }
                }
                catch { _exePath = null; }
                return _exePath;
            }
        }

        private static string FindOnPath(string exe)
        {
            try
            {
                string pathEnv = Environment.GetEnvironmentVariable("PATH") ?? "";
                foreach (var p in pathEnv.Split(Path.PathSeparator))
                {
                    if (string.IsNullOrWhiteSpace(p)) continue;
                    string cand;
                    try { cand = Path.Combine(p.Trim(), exe); } catch { continue; }
                    if (File.Exists(cand)) return cand;
                }
            }
            catch { }
            return null;
        }

        private static byte[] ReadEmbedded()
        {
            try
            {
                var asm = Assembly.GetExecutingAssembly();
                using (var s = asm.GetManifestResourceStream(EmbeddedName))
                {
                    if (s == null) return null;
                    using (var ms = new MemoryStream())
                    {
                        s.CopyTo(ms);
                        return ms.ToArray();
                    }
                }
            }
            catch { return null; }
        }

        // ------------------------------------------------------------------
        // Long-path plumbing. \\?\-prefixed absolute paths bypass MAX_PATH for
        // every System.IO file op regardless of the OS LongPathsEnabled policy
        // (App.config additionally pins the 4.6.2+ path-handling switches).
        // ------------------------------------------------------------------

        private static string ToExtended(string path)
        {
            if (string.IsNullOrEmpty(path)) return path;
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) return path;
            string full = Path.IsPathRooted(path) ? path : Path.GetFullPath(path);
            if (full.StartsWith(@"\\", StringComparison.Ordinal))
                return @"\\?\UNC\" + full.Substring(2);
            return @"\\?\" + full;
        }

        private static string FromExtended(string path)
        {
            if (string.IsNullOrEmpty(path)) return path;
            if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                return @"\\" + path.Substring(8);
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal))
                return path.Substring(4);
            return path;
        }

        private static void LongCopy(string src, string dst)
        {
            File.Copy(ToExtended(src), ToExtended(dst), true);
        }

        /// <summary>Move (overwriting) a short temp file to a possibly-long final
        /// path. MoveFileW copies+deletes across volumes for files, but fall back
        /// to an explicit copy+delete if a filesystem refuses the move.</summary>
        private static void LongMoveOver(string src, string dst)
        {
            string xdst = ToExtended(dst);
            if (File.Exists(xdst)) File.Delete(xdst);
            try { File.Move(ToExtended(src), xdst); }
            catch (IOException)
            {
                File.Copy(ToExtended(src), xdst, true);
                try { File.Delete(ToExtended(src)); } catch { }
            }
        }

        /// <summary>Delete a source file, preferring the Recycle Bin so a mistaken
        /// "delete the sources" run is recoverable.
        ///
        /// LONG PATHS: the shell delete is SHFileOperation underneath. It does not
        /// understand the \\?\ prefix and is hard-capped at MAX_PATH, and the VB
        /// wrapper normalizes the path first, so a >260-char name throws
        /// (PathTooLongException/ArgumentException) instead of deleting. Those files
        /// fall back to the PERMANENT extended-path delete on purpose: a leftover
        /// .png keeps the engine on the PNG path forever (it only takes the .dds when
        /// the .png is absent), so silently not deleting is the worse failure. Pack
        /// paths routinely exceed MAX_PATH, so this branch is normal, not exotic —
        /// the delete-source prompt says so.</summary>
        private static void LongDelete(string path)
        {
            string x = ToExtended(path);
            if (!File.Exists(x)) return;

            string plain = FromExtended(x);
            if (plain.Length < 260)
            {
                try
                {
                    Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(
                        plain,
                        Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                        Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
                    return;
                }
                catch (PathTooLongException)   { /* shell's limit is stricter; fall through  */ }
                catch (FileNotFoundException)  { /* VB looked with a non-long-aware probe   */ }
                catch (ArgumentException)      { /* path shape the shell won't take         */ }
                catch (NotSupportedException)  { /* \\?\ form, or no shell (session 0)      */ }
            }

            File.Delete(x);
        }

        /// <summary>Run texconv with the given args; returns exit 0 and captures any
        /// output for diagnostics. stdout+stderr are drained asynchronously so a
        /// full pipe never deadlocks the wait.</summary>
        private static bool Run(string args, string workDir, out string output, Func<bool> cancelled = null)
        {
            output = "";
            string exe = ResolveExe();
            if (exe == null) { output = "texconv.exe not available"; return false; }

            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName               = exe,
                    Arguments              = args,
                    UseShellExecute        = false,
                    CreateNoWindow         = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError  = true
                };
                if (!string.IsNullOrEmpty(workDir)) psi.WorkingDirectory = workDir;

                var sb = new StringBuilder();
                using (var proc = new Process { StartInfo = psi })
                {
                    DataReceivedEventHandler sink = (s, e) =>
                    {
                        if (!string.IsNullOrEmpty(e.Data)) lock (sb) sb.AppendLine(e.Data);
                    };
                    proc.OutputDataReceived += sink;
                    proc.ErrorDataReceived  += sink;
                    proc.Start();
                    proc.BeginOutputReadLine();
                    proc.BeginErrorReadLine();

                    // Killing texconv only ever discards work in the private temp dir the
                    // caller wipes; it writes nothing into the pack itself.
                    while (!proc.WaitForExit(150))
                    {
                        if (cancelled == null || !cancelled()) continue;
                        try { proc.Kill(); } catch { }
                        try { proc.WaitForExit(5000); } catch { }
                        output = sb.ToString();
                        return false;
                    }
                    proc.WaitForExit(); // timed waits don't drain the async readers; this does

                    output = sb.ToString();
                    return proc.ExitCode == 0;
                }
            }
            catch (Exception ex) { output = ex.Message; return false; }
        }

        // ------------------------------------------------------------------
        // Shuffle conversion core.
        // ------------------------------------------------------------------

        private sealed class Job
        {
            public string Src;      // user's source file (may exceed MAX_PATH)
            public string DstDir;   // destination directory (may exceed MAX_PATH)
            public string TempIn;   // short temp copy fed to texconv
            public string TempOut;  // short temp result texconv writes
            public string Final;    // DstDir + source basename + new extension
            public string Error;
            public bool   Ok;
        }

        /// <summary>Convert sources via the short-temp shuffle, chunked. decode=false
        /// is PNG-&gt;BC7 .dds, decode=true is .dds-&gt;PNG. fullMips picks -m 0 vs -m 1
        /// (see the class remarks). dstDirOf maps a source to its output directory.
        /// onConverted fires per successful source (used for delete-source). The
        /// output keeps the exact source basename with only the extension swapped —
        /// the engine matches textures by parsing that name.
        ///
        /// doneBase/totalOverride let a caller run several passes behind ONE progress
        /// bar; pass 0/0 for a standalone run.
        ///
        /// cancelled is polled per chunk AND handed to texconv. A cancelled chunk is
        /// abandoned whole — its results never leave the temp dir, so no .dds is placed
        /// half-written and no source is deleted for a conversion that didn't land.
        /// Chunks that already finished keep their output; re-running converts the rest.</summary>
        private static void ConvertMany(List<string> sources, Func<string, string> dstDirOf,
                                        bool decode, bool fullMips,
                                        Action<int, int, string> report,
                                        Action<string> onConverted,
                                        int doneBase, int totalOverride,
                                        Func<bool> cancelled,
                                        out int converted, out int failed, out string firstError)
        {
            converted  = 0;
            failed     = 0;
            firstError = null;

            string inExt   = decode ? ".dds" : ".png";
            string outExt  = decode ? ".png" : ".dds";
            int    total   = totalOverride > 0 ? totalOverride : sources.Count;
            // --ignore-srgb is MANDATORY on the encode. DuckStation packs write PNGs
            // carrying an sRGB chunk (intent 0) + gAMA 45455, so WIC hands texconv a
            // *_UNORM_SRGB source; DirectXTex then does a metadata-driven sRGB->linear
            // conversion on the way into plain BC7_UNORM and every texture lands ~5x
            // too dark. The engine samples in gamma space (dds_load.c maps dxgi 98 to
            // GL_COMPRESSED_RGBA_BPTC_UNORM, and dxgi 99 to the _SRGB format, which
            // would re-darken at sample time), so the fix is to keep the output at
            // dxgi 98 and stop the conversion — NOT to tag the file _SRGB.
            string convArg = decode ? "-nologo -ft png -y -o o"
                                    : "-nologo -f BC7_UNORM " + (fullMips ? "-m 0" : "-m 1") +
                                      " --ignore-srgb -y -o o";
            int done = 0;

            for (int start = 0; start < sources.Count; start += ChunkSize)
            {
                if (cancelled != null && cancelled()) return;

                int count = Math.Min(ChunkSize, sources.Count - start);
                string workDir = Path.Combine(Path.GetTempPath(), "SilentHillPC_Launcher",
                                              "dds-" + Guid.NewGuid().ToString("N").Substring(0, 8));
                string inDir  = Path.Combine(workDir, "i");
                string outDir = Path.Combine(workDir, "o");
                var    jobs   = new List<Job>(count);

                try
                {
                    Directory.CreateDirectory(inDir);
                    Directory.CreateDirectory(outDir);

                    var sb = new StringBuilder(convArg);
                    for (int k = 0; k < count; k++)
                    {
                        string src = sources[start + k];
                        var job = new Job
                        {
                            Src     = src,
                            DstDir  = dstDirOf(src),
                            TempIn  = Path.Combine(inDir,  k.ToString("D6") + inExt),
                            TempOut = Path.Combine(outDir, k.ToString("D6") + outExt)
                        };
                        job.Final = Path.Combine(job.DstDir,
                                                 Path.GetFileNameWithoutExtension(src) + outExt);
                        jobs.Add(job);

                        try
                        {
                            LongCopy(src, job.TempIn);
                            sb.Append(" i\\").Append(k.ToString("D6")).Append(inExt);
                        }
                        catch (Exception ex)
                        {
                            job.Error = "copy failed for " + Path.GetFileName(src) + ": " + ex.Message;
                        }
                    }

                    if (report != null && jobs.Count > 0)
                        report(doneBase + done + 1, total,
                               (decode ? "Decoding " : "Encoding ") + Path.GetFileName(jobs[0].Src) +
                               "  (" + (doneBase + done + 1) + "/" + total + ")");

                    string output;
                    Run(sb.ToString(), workDir, out output, cancelled);

                    // Cancelled mid-chunk: drop this chunk's results untouched (the finally
                    // wipes the temp dir) rather than place a partially-encoded batch.
                    if (cancelled != null && cancelled()) return;

                    foreach (var job in jobs)
                    {
                        done++;
                        if (job.Error == null)
                        {
                            if (File.Exists(job.TempOut))
                            {
                                try
                                {
                                    Directory.CreateDirectory(ToExtended(job.DstDir));
                                    LongMoveOver(job.TempOut, job.Final);
                                    job.Ok = true;
                                }
                                catch (Exception ex)
                                {
                                    job.Error = "could not place " + Path.GetFileName(job.Final) + ": " + ex.Message;
                                }
                            }
                            else
                            {
                                job.Error = "texconv failed for " + Path.GetFileName(job.Src) +
                                            (string.IsNullOrEmpty(output) ? "" : ":\n" + output.Trim());
                            }
                        }

                        if (job.Ok)
                        {
                            converted++;
                            if (onConverted != null) onConverted(job.Src);
                        }
                        else
                        {
                            failed++;
                            if (firstError == null) firstError = job.Error;
                        }

                        if (report != null)
                            report(doneBase + done, total,
                                   (decode ? "Decoding " : "Encoding ") + Path.GetFileName(job.Src) +
                                   "  (" + (doneBase + done) + "/" + total + ")");
                    }
                }
                catch (Exception ex)
                {
                    // Whole-chunk failure (temp dir creation etc.) — charge every
                    // job in the chunk that hasn't been accounted yet.
                    foreach (var job in jobs)
                        if (!job.Ok && job.Error == null) job.Error = ex.Message;
                    int unaccounted = count - jobs.Count;
                    failed += unaccounted;
                    done   += unaccounted;
                    if (firstError == null) firstError = ex.Message;
                }
                finally
                {
                    try { Directory.Delete(workDir, true); } catch { }
                }
            }
        }

        /// <summary>Encode one PNG to a BC7 .dds in <paramref name="outDir"/>. The base
        /// name is kept, so FOO.TIM.p00.png -> FOO.TIM.p00.dds. Returns false + a
        /// message on failure.
        ///
        /// Always the FULL mip chain: a hand-picked file is typically a loose override
        /// that uploads compressed, and nothing here knows otherwise.</summary>
        public static bool EncodePng(string pngPath, string outDir, out string error)
        {
            int converted, failed;
            ConvertMany(new List<string> { pngPath }, _ => outDir, false, true, null, null, 0, 0, null,
                        out converted, out failed, out error);
            return failed == 0 && converted == 1;
        }

        /// <summary>Decode one .dds to a .png in <paramref name="outDir"/>.</summary>
        public static bool DecodeDds(string ddsPath, string outDir, out string error)
        {
            int converted, failed;
            ConvertMany(new List<string> { ddsPath }, _ => outDir, true, true, null, null, 0, 0, null,
                        out converted, out failed, out error);
            return failed == 0 && converted == 1;
        }

        /// <summary>Batch-encode every *.png under <paramref name="folder"/> to a BC7
        /// .dds beside it (recursive). When <paramref name="deleteSource"/>, each
        /// source .png that converted is removed — the game only takes the .dds fast
        /// path when the .png isn't also present. Returns (converted, failed);
        /// <paramref name="firstError"/> holds the first failure message.
        ///
        /// <paramref name="wholeCoverPngs"/> is the subset that can upload COMPRESSED
        /// and therefore needs a real mip chain (PackAnalysis.WholeCoverPngs).
        /// Everything else is only ever CPU-decoded at level 0, so it is encoded
        /// single-level. Pass null when the folder was not analysed — then every file
        /// keeps the full chain, which is never wrong, only bigger.</summary>
        public static void EncodeFolder(string folder, bool deleteSource,
                                        ICollection<string> wholeCoverPngs,
                                        Action<int, int, string> report, Func<bool> cancelled,
                                        out int converted, out int failed, out string firstError)
        {
            converted = 0;
            failed = 0;
            firstError = null;

            var pngs = new List<string>();
            try
            {
                // Enumerate through the extended prefix so files nested past
                // MAX_PATH are found at all; strip it back off for the jobs.
                foreach (var f in Directory.EnumerateFiles(ToExtended(folder), "*.png",
                                                           SearchOption.AllDirectories))
                    pngs.Add(FromExtended(f));
            }
            catch (Exception ex) { firstError = ex.Message; return; }

            if (wholeCoverPngs == null || wholeCoverPngs.Count == 0)
            {
                EncodeFiles(pngs, deleteSource, true, report, cancelled, out converted, out failed, out firstError);
                return;
            }

            var whole  = new HashSet<string>(wholeCoverPngs, StringComparer.OrdinalIgnoreCase);
            var mipped = new List<string>();
            var flat   = new List<string>();
            foreach (var p in pngs)
            {
                if (whole.Contains(p)) mipped.Add(p);
                else                   flat.Add(p);
            }

            // texconv takes one -m per invocation, so this is two passes sharing one
            // progress bar. Failures accumulate; the first message wins.
            int c1, f1, c2 = 0, f2 = 0;
            string e1, e2 = null;
            EncodePass(mipped, deleteSource, true, report, 0, pngs.Count, cancelled, out c1, out f1, out e1);
            if (flat.Count > 0 && (cancelled == null || !cancelled()))
                EncodePass(flat, deleteSource, false, report, mipped.Count, pngs.Count, cancelled,
                           out c2, out f2, out e2);

            converted  = c1 + c2;
            failed     = f1 + f2;
            firstError = e1 ?? e2;
        }

        /// <summary>Batch-encode an explicit list of .png sources to a BC7 .dds beside
        /// each one. <paramref name="deleteSource"/> removes ONLY the sources that
        /// actually converted — a file that was never in <paramref name="sources"/>,
        /// or that failed, is never touched. This is what makes the whole-texture-only
        /// pack run safe: every sub-rect .png the caller left out survives.
        ///
        /// <paramref name="fullMips"/> must be true for any source that could upload
        /// compressed (a whole-cover pack entry or a loose override).</summary>
        public static void EncodeFiles(List<string> sources, bool deleteSource, bool fullMips,
                                       Action<int, int, string> report, Func<bool> cancelled,
                                       out int converted, out int failed, out string firstError)
        {
            EncodePass(sources, deleteSource, fullMips, report, 0, 0, cancelled,
                       out converted, out failed, out firstError);
        }

        private static void EncodePass(List<string> sources, bool deleteSource, bool fullMips,
                                       Action<int, int, string> report, int doneBase, int totalOverride,
                                       Func<bool> cancelled,
                                       out int converted, out int failed, out string firstError)
        {
            Action<string> onConverted = null;
            if (deleteSource)
                onConverted = src => { try { LongDelete(src); } catch { /* leave the png if locked */ } };

            ConvertMany(sources, src => Path.GetDirectoryName(src), false, fullMips, report,
                        onConverted, doneBase, totalOverride, cancelled,
                        out converted, out failed, out firstError);
        }

        // ------------------------------------------------------------------
        // DuckStation pack analysis.
        //
        // A DuckStation pack is NOT a set of whole-texture replacements: most of
        // its files patch a small sub-rect of one VRAM upload, and the engine
        // composites them onto an upscaled copy of the native texture
        // (TexPack_Compose, pc_port/src/tex_pack.c). That compositor is an RGBA
        // blitter, so a sub-rect .dds is decoded to RGBA on the CPU first
        // (Dds_DecodeRgba) and then blitted like any .png — it loads fine, it just
        // cannot stay compressed. Only a single entry covering the whole native
        // texture takes the compressed whole-upload fast path, so only those save
        // VRAM and only those need a mip chain.
        //
        // The classification therefore drives the mip decision and the informational
        // counts, NOT whether a file works. The parser is a LITERAL port of
        // tex_pack.c's ParseName/Mode_Bpp and of the whole-cover test in
        // TexPack_Compose. It must stay a mirror: if the engine grammar changes,
        // change it here too and re-run AnalyzeSelfTest.
        // ------------------------------------------------------------------

        /// <summary>What a folder of pack files actually contains.
        /// WholeCover + SubRect + Unparsed == Total.</summary>
        public sealed class PackAnalysis
        {
            /// <summary>.png/.dds files whose name the engine would even look at.</summary>
            public int Total;
            /// <summary>Entries replacing an entire native texture — these upload
            /// compressed, so they save VRAM and need a full mip chain.</summary>
            public int WholeCover;
            /// <summary>Entries patching a sub-rect — composited, so a .dds is decoded
            /// to RGBA (level 0 only) before it is blitted.</summary>
            public int SubRect;
            /// <summary>Distinct (source texture, bpp, palette) uploads the pack touches.</summary>
            public int Groups;
            /// <summary>Groups that can take the compressed whole-upload path once
            /// converted, i.e. the ones that actually save VRAM: the fast path needs
            /// matchCount == 1, so EVERY entry in the group must be whole-cover. One
            /// entry qualifies outright; several qualify only because TexPack_Compose's
            /// DDS-first pass collapses whole-cover twins (Mode_Bpp masks the ST bit,
            /// so a P4 and its STP4 twin are one group). A group holding even one
            /// sub-rect is composited and costs full RGBA either way.</summary>
            public int UsableGroups;
            /// <summary>Named like a pack file but rejected by the grammar.</summary>
            public int Unparsed;
            /// <summary>The whole-cover entries that are still .png, i.e. the exact set
            /// a whole-texture-only conversion may touch.</summary>
            public readonly List<string> WholeCoverPngs = new List<string>();
            /// <summary>Sources a conversion could actually consume. Total counts pack
            /// ENTRIES whatever their extension, so an already-converted folder still
            /// reports thousands of files while having nothing left to encode.</summary>
            public int Pngs;
        }

        private static readonly string[] ModeNames =
            { "P4", "P8", "C16", "C16", "STP4", "STP8", "STC16", "STC16" };

        private struct PackEntryName
        {
            public ulong  SrcHash;
            public ulong  PalHash;
            public ushort SrcW, SrcH;
            public ushort OffX, OffY;
            public ushort SubW, SubH;
            public byte   Mode;
        }

        private sealed class GroupInfo
        {
            public int Count;
            public int WholeCount;
        }

        /// <summary>tex_pack.c Mode_Bpp. The ST bit is masked off, so P4 and STP4
        /// (and P8/STP8, C16/STC16) land in the same group.</summary>
        private static int ModeBpp(byte mode)
        {
            switch (mode & 3)
            {
                case 0:  return 4;
                case 1:  return 8;
                default: return 16;
            }
        }

        private static int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        /// <summary>Exactly 16 hex digits, not 15 and not 17 — DuckStation always
        /// writes 16 and tex_pack.c's ParseHex64 rejects any other run length.</summary>
        private static bool ParseHex64(string s, ref int p, out ulong v)
        {
            v = 0;
            if (p + 16 > s.Length) return false;
            for (int i = 0; i < 16; i++)
            {
                int d = HexVal(s[p + i]);
                if (d < 0) return false;
                v = (v << 4) | (ulong)(uint)d;
            }
            if (p + 16 < s.Length && HexVal(s[p + 16]) >= 0) return false;
            p += 16;
            return true;
        }

        private static bool ParseDec16(string s, ref int p, out ushort v)
        {
            uint acc   = 0;
            int  start = p;

            v = 0;
            while (p < s.Length && s[p] >= '0' && s[p] <= '9')
            {
                acc = acc * 10 + (uint)(s[p] - '0');
                if (acc > 0xFFFF) return false;
                p++;
            }
            if (p == start) return false;
            v = (ushort)acc;
            return true;
        }

        private static bool Expect(string s, ref int p, char c)
        {
            if (p >= s.Length || s[p] != c) return false;
            p++;
            return true;
        }

        /// <summary>tex_pack.c ParseName. 16bpp names omit the palette hash and the
        /// P-range. Anchored: any trailing character (an upscaler tool's suffix, a
        /// " (1)" copy marker) rejects the name, exactly as the engine rejects it.</summary>
        private static bool ParseEntryName(string title, out PackEntryName e)
        {
            e = new PackEntryName();

            if (title.StartsWith("texpage-", StringComparison.Ordinal)) return false;
            if (!title.StartsWith("texupload-", StringComparison.Ordinal)) return false;
            int p = 10;

            int dash = title.IndexOf('-', p);
            if (dash < 0) return false;
            int modeLen = dash - p;
            if (modeLen == 0 || modeLen >= 8) return false;
            string modeTok = title.Substring(p, modeLen);
            p = dash + 1;

            e.Mode = 0xFF;
            for (int m = 0; m < ModeNames.Length; m++)
            {
                if (string.Equals(modeTok, ModeNames[m], StringComparison.Ordinal))
                {
                    e.Mode = (byte)m;
                    break;
                }
            }
            if (e.Mode == 0xFF) return false;

            if (!ParseHex64(title, ref p, out e.SrcHash)) return false;

            if (ModeBpp(e.Mode) != 16)
            {
                ushort palMin, palMax;
                if (!Expect(title, ref p, '-') || !ParseHex64(title, ref p, out e.PalHash)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.SrcW)) return false;
                if (!Expect(title, ref p, 'x') || !ParseDec16(title, ref p, out e.SrcH)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.OffX)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.OffY)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.SubW)) return false;
                if (!Expect(title, ref p, 'x') || !ParseDec16(title, ref p, out e.SubH)) return false;
                if (!Expect(title, ref p, '-') || !Expect(title, ref p, 'P') ||
                    !ParseDec16(title, ref p, out palMin)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out palMax)) return false;
                if (palMin > 255) return false;
            }
            else
            {
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.SrcW)) return false;
                if (!Expect(title, ref p, 'x') || !ParseDec16(title, ref p, out e.SrcH)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.OffX)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.OffY)) return false;
                if (!Expect(title, ref p, '-') || !ParseDec16(title, ref p, out e.SubW)) return false;
                if (!Expect(title, ref p, 'x') || !ParseDec16(title, ref p, out e.SubH)) return false;
                e.PalHash = 0;
            }

            if (p != title.Length) return false;

            return e.SrcW != 0 && e.SrcH != 0 && e.SubW != 0 && e.SubH != 0;
        }

        /// <summary>tex_pack.c FileTitle: basename minus a final .png/.dds. The 160-char
        /// cap is the engine's title buffer — a longer name is not indexed at all.</summary>
        private static bool EntryTitle(string name, out string title, out bool isDds)
        {
            title = null;
            isDds = false;

            int len = name.Length;
            if (len < 5 || len >= 160) return false;
            string ext = name.Substring(len - 4);
            isDds = string.Equals(ext, ".dds", StringComparison.OrdinalIgnoreCase);
            if (!isDds && !string.Equals(ext, ".png", StringComparison.OrdinalIgnoreCase)) return false;
            title = name.Substring(0, len - 4);
            return true;
        }

        /// <summary>Classify every pack file under <paramref name="folder"/> (recursive).
        /// Read-only: nothing is written, moved or deleted.</summary>
        public static PackAnalysis AnalyzePack(string folder)
        {
            var a      = new PackAnalysis();
            var groups = new Dictionary<string, GroupInfo>(StringComparer.Ordinal);

            try
            {
                foreach (var f in Directory.EnumerateFiles(ToExtended(folder), "*",
                                                           SearchOption.AllDirectories))
                {
                    string path = FromExtended(f);
                    string name = Path.GetFileName(path);
                    string title;
                    bool   isDds;

                    if (!EntryTitle(name, out title, out isDds)) continue;
                    a.Total++;
                    if (!isDds) a.Pngs++;

                    PackEntryName e;
                    if (!ParseEntryName(title, out e)) { a.Unparsed++; continue; }

                    // The name's WxH is in VRAM 16-bit WORDS, not texels: a 4bpp
                    // "64x256" upload is 256 texels wide. Comparing subW to srcW
                    // instead of nativeW silently reclassifies whole covers as
                    // sub-rects (and vice versa).
                    int bpp     = ModeBpp(e.Mode);
                    int nativeW = e.SrcW * (16 / bpp);

                    bool whole = e.OffX == 0 && e.OffY == 0 &&
                                 e.SubW == nativeW && e.SubH == e.SrcH;
                    if (whole)
                    {
                        a.WholeCover++;
                        if (!isDds) a.WholeCoverPngs.Add(path);
                    }
                    else
                    {
                        a.SubRect++;
                    }

                    string key = e.SrcHash.ToString("X16") + "|" + bpp + "|" + e.PalHash.ToString("X16");
                    GroupInfo g;
                    if (!groups.TryGetValue(key, out g)) { g = new GroupInfo(); groups[key] = g; }
                    g.Count++;
                    if (whole) g.WholeCount++;
                }
            }
            catch { /* unreadable subtree — report what was classified */ }

            a.Groups = groups.Count;
            foreach (var g in groups.Values)
                if (g.WholeCount == g.Count) a.UsableGroups++;

            return a;
        }

        /// <summary>Grammar regression gate. Run against the reference DuckStation pack
        /// (SLES-01514 .../replacements, 12,227 files): these numbers were derived from
        /// tex_pack.c by hand and any drift means the port stopped mirroring the engine.
        /// If it trips, fix the port — do NOT re-baseline the numbers.</summary>
        public static bool AnalyzeSelfTest(string packFolder, out string report)
        {
            var a  = AnalyzePack(packFolder);
            var sb = new StringBuilder();
            bool ok = true;

            Action<string, int, int> check = (what, got, want) =>
            {
                if (got != want) ok = false;
                sb.AppendLine((got == want ? "PASS " : "FAIL ") + what +
                              ": got " + got + ", expected " + want);
            };

            check("total",        a.Total,        12227);
            check("wholeCover",   a.WholeCover,   121);
            check("subRect",      a.SubRect,      12105);
            check("groups",       a.Groups,       9487);
            check("usableGroups", a.UsableGroups, 86);
            check("unparsed",     a.Unparsed,     1);

            report = sb.ToString();
            return ok;
        }

        // ------------------------------------------------------------------
        // Regression self-test (no test project in this solution — call
        // RoundTripSelfTest directly from a harness or a debug menu item).
        // ------------------------------------------------------------------

        /// <summary>Encode <paramref name="pngPath"/> to BC7 .dds, decode it straight
        /// back to PNG, and report the mean absolute error per channel against the
        /// source plus the .dds DXGI format.
        ///
        /// PASS = mae &lt; 1.5 AND dxgiFormat == 98 (DXGI_FORMAT_BC7_UNORM). This
        /// guards the gamma bug: without --ignore-srgb, DirectXTex converts the
        /// PNG's sRGB metadata to linear on the way in and the round trip scores
        /// ~39/255 (about 5x too dark). dxgi 99 (BC7_UNORM_SRGB) is equally wrong —
        /// the engine would re-apply the darkening at sample time.
        ///
        /// The 1.5 gate is chosen against measured data, not taste: 11 real pack
        /// files across P4/P8/STP4/STP8 scored 0.03-0.92 correct vs 15-36 broken,
        /// so the two populations are two orders of magnitude apart. 1.0 would sit
        /// only 0.08 above the worst correct sample and could flake on a CPU-fallback
        /// encoder or noisier alpha-heavy content.
        ///
        /// NEVER assert byte-equality: BC7 block selection differs between texconv's
        /// DirectCompute and CPU fallback paths, so the same input legitimately
        /// yields different-but-valid blocks on different machines.</summary>
        public static bool RoundTripSelfTest(string pngPath, out double mae, out int dxgiFormat,
                                             out string error)
        {
            mae         = double.NaN;
            dxgiFormat  = 0;
            error       = null;

            string work = Path.Combine(Path.GetTempPath(), "SilentHillPC_Launcher",
                                       "selftest-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            try
            {
                string encDir = Path.Combine(work, "enc");
                string decDir = Path.Combine(work, "dec");
                Directory.CreateDirectory(encDir);
                Directory.CreateDirectory(decDir);

                if (!EncodePng(pngPath, encDir, out error)) return false;
                string dds = Path.Combine(encDir,
                                          Path.GetFileNameWithoutExtension(pngPath) + ".dds");
                if (!File.Exists(dds)) { error = "no .dds produced"; return false; }

                // DDS_HEADER_DXT10.dxgiFormat: 4 magic + 124 header.
                using (var fs = new FileStream(dds, FileMode.Open, FileAccess.Read))
                {
                    var hdr = new byte[132];
                    if (fs.Read(hdr, 0, hdr.Length) != hdr.Length) { error = "short .dds"; return false; }
                    dxgiFormat = BitConverter.ToInt32(hdr, 128);
                }

                if (!DecodeDds(dds, decDir, out error)) return false;
                string back = Path.Combine(decDir,
                                           Path.GetFileNameWithoutExtension(pngPath) + ".png");
                if (!File.Exists(back)) { error = "no decoded .png produced"; return false; }

                mae = MeanAbsoluteError(pngPath, back, out error);
                if (double.IsNaN(mae)) return false;

                if (dxgiFormat != 98)
                {
                    error = "dxgiFormat is " + dxgiFormat + ", expected 98 (BC7_UNORM)";
                    return false;
                }
                if (mae >= 1.5)
                {
                    error = "mean absolute error " + mae.ToString("F2") + "/255 (expected < 1.5)";
                    return false;
                }
                return true;
            }
            catch (Exception ex) { error = ex.Message; return false; }
            finally { try { Directory.Delete(work, true); } catch { } }
        }

        /// <summary>Mean absolute error per channel (R,G,B,A) between two images, in
        /// 0-255 units. NaN + a message if they can't be compared.</summary>
        private static double MeanAbsoluteError(string aPath, string bPath, out string error)
        {
            error = null;
            try
            {
                using (var a = new System.Drawing.Bitmap(aPath))
                using (var b = new System.Drawing.Bitmap(bPath))
                {
                    if (a.Width != b.Width || a.Height != b.Height)
                    {
                        error = "size mismatch: " + a.Width + "x" + a.Height +
                                " vs " + b.Width + "x" + b.Height;
                        return double.NaN;
                    }

                    var rect = new System.Drawing.Rectangle(0, 0, a.Width, a.Height);
                    var fmt  = System.Drawing.Imaging.PixelFormat.Format32bppArgb;
                    var la = a.LockBits(rect, System.Drawing.Imaging.ImageLockMode.ReadOnly, fmt);
                    var lb = b.LockBits(rect, System.Drawing.Imaging.ImageLockMode.ReadOnly, fmt);
                    try
                    {
                        long   sum = 0;
                        long   n   = (long)a.Width * a.Height * 4;
                        var    ra  = new byte[a.Width * 4];
                        var    rb  = new byte[a.Width * 4];
                        for (int y = 0; y < a.Height; y++)
                        {
                            System.Runtime.InteropServices.Marshal.Copy(
                                la.Scan0 + y * la.Stride, ra, 0, ra.Length);
                            System.Runtime.InteropServices.Marshal.Copy(
                                lb.Scan0 + y * lb.Stride, rb, 0, rb.Length);
                            for (int i = 0; i < ra.Length; i++)
                                sum += Math.Abs(ra[i] - rb[i]);
                        }
                        return (double)sum / n;
                    }
                    finally { a.UnlockBits(la); b.UnlockBits(lb); }
                }
            }
            catch (Exception ex) { error = ex.Message; return double.NaN; }
        }
    }
}
