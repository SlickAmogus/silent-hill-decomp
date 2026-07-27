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

        private static void LongDelete(string path)
        {
            string x = ToExtended(path);
            if (File.Exists(x)) File.Delete(x);
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
            string convArg = decode ? "-nologo -ft png -y -o o"
                                    : "-nologo -f BC7_UNORM -m 0 -y -o o";
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
    }
}
