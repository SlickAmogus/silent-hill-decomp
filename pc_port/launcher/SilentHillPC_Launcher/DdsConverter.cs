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
    /// cutout — BC1/DXT1 would wreck it), and the FULL mip chain (-m 0) is
    /// MANDATORY: the game uploads the file's own mips level-by-level, and a
    /// single-level .dds pins GL_TEXTURE_MAX_LEVEL 0 and renders black.
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
        private static bool Run(string args, string workDir, out string output)
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
                    proc.WaitForExit();
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
        /// is PNG-&gt;BC7 .dds, decode=true is .dds-&gt;PNG. dstDirOf maps a source to
        /// its output directory. onConverted fires per successful source (used for
        /// delete-source). The output keeps the exact source basename with only the
        /// extension swapped — the engine matches textures by parsing that name.</summary>
        private static void ConvertMany(List<string> sources, Func<string, string> dstDirOf,
                                        bool decode, Action<int, int, string> report,
                                        Action<string> onConverted,
                                        out int converted, out int failed, out string firstError)
        {
            converted  = 0;
            failed     = 0;
            firstError = null;

            string inExt   = decode ? ".dds" : ".png";
            string outExt  = decode ? ".png" : ".dds";
            // --ignore-srgb is MANDATORY on the encode. DuckStation packs write PNGs
            // carrying an sRGB chunk (intent 0) + gAMA 45455, so WIC hands texconv a
            // *_UNORM_SRGB source; DirectXTex then does a metadata-driven sRGB->linear
            // conversion on the way into plain BC7_UNORM and every texture lands ~5x
            // too dark. The engine samples in gamma space (dds_load.c maps dxgi 98 to
            // GL_COMPRESSED_RGBA_BPTC_UNORM, and dxgi 99 to the _SRGB format, which
            // would re-darken at sample time), so the fix is to keep the output at
            // dxgi 98 and stop the conversion — NOT to tag the file _SRGB.
            string convArg = decode ? "-nologo -ft png -y -o o"
                                    : "-nologo -f BC7_UNORM -m 0 --ignore-srgb -y -o o";
            int done = 0;

            for (int start = 0; start < sources.Count; start += ChunkSize)
            {
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
                        report(done + 1, sources.Count,
                               (decode ? "Decoding " : "Encoding ") +
                               Path.GetFileName(jobs[0].Src) + "  (" + (done + 1) + "/" + sources.Count + ")");

                    string output;
                    Run(sb.ToString(), workDir, out output);

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
                            report(done, sources.Count,
                                   (decode ? "Decoding " : "Encoding ") +
                                   Path.GetFileName(job.Src) + "  (" + done + "/" + sources.Count + ")");
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

        /// <summary>Encode one PNG to a BC7 .dds (full mip chain) in <paramref
        /// name="outDir"/>. The base name is kept, so FOO.TIM.p00.png ->
        /// FOO.TIM.p00.dds. Returns false + a message on failure.</summary>
        public static bool EncodePng(string pngPath, string outDir, out string error)
        {
            int converted, failed;
            ConvertMany(new List<string> { pngPath }, _ => outDir, false, null, null,
                        out converted, out failed, out error);
            return failed == 0 && converted == 1;
        }

        /// <summary>Decode one .dds to a .png in <paramref name="outDir"/>.</summary>
        public static bool DecodeDds(string ddsPath, string outDir, out string error)
        {
            int converted, failed;
            ConvertMany(new List<string> { ddsPath }, _ => outDir, true, null, null,
                        out converted, out failed, out error);
            return failed == 0 && converted == 1;
        }

        /// <summary>Batch-encode every *.png under <paramref name="folder"/> to a BC7
        /// .dds beside it (recursive). When <paramref name="deleteSource"/>, each
        /// source .png that converted is removed — the game only takes the .dds fast
        /// path when the .png isn't also present. Returns (converted, failed);
        /// <paramref name="firstError"/> holds the first failure message.</summary>
        public static void EncodeFolder(string folder, bool deleteSource,
                                        Action<int, int, string> report,
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

            Action<string> onConverted = null;
            if (deleteSource)
                onConverted = src => { try { LongDelete(src); } catch { /* leave the png if locked */ } };

            ConvertMany(pngs, src => Path.GetDirectoryName(src), false, report, onConverted,
                        out converted, out failed, out firstError);
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
