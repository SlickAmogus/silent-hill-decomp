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
    /// </summary>
    public static class DdsConverter
    {
        private const string EmbeddedName = "SilentHillPC_Launcher.texconv.exe";

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

        /// <summary>Run texconv with the given args; returns exit 0 and captures any
        /// output for diagnostics. stdout+stderr are drained asynchronously so a
        /// full pipe never deadlocks the wait.</summary>
        private static bool Run(string args, out string output)
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

        private static string Quote(string s) { return "\"" + s + "\""; }

        /// <summary>Encode one PNG to a BC7 .dds (full mip chain) in <paramref
        /// name="outDir"/>. texconv keeps the base name, so FOO.TIM.p00.png ->
        /// FOO.TIM.p00.dds. Returns false + a message on failure.</summary>
        public static bool EncodePng(string pngPath, string outDir, out string error)
        {
            error = null;
            Directory.CreateDirectory(outDir);
            string output;
            // -f BC7_UNORM: 8-bit alpha kept; -m 0: full mip chain (mandatory);
            // -y: overwrite; -o: output dir. DDS is texconv's default output type.
            bool ok = Run("-nologo -f BC7_UNORM -m 0 -y -o " + Quote(outDir) + " " + Quote(pngPath), out output);
            string outFile = Path.Combine(outDir, Path.GetFileNameWithoutExtension(pngPath) + ".dds");
            if (!ok || !File.Exists(outFile))
            {
                error = "texconv failed for " + Path.GetFileName(pngPath) +
                        (string.IsNullOrEmpty(output) ? "" : ":\n" + output.Trim());
                return false;
            }
            return true;
        }

        /// <summary>Decode one .dds to a .png in <paramref name="outDir"/>.</summary>
        public static bool DecodeDds(string ddsPath, string outDir, out string error)
        {
            error = null;
            Directory.CreateDirectory(outDir);
            string output;
            bool ok = Run("-nologo -ft png -y -o " + Quote(outDir) + " " + Quote(ddsPath), out output);
            string outFile = Path.Combine(outDir, Path.GetFileNameWithoutExtension(ddsPath) + ".png");
            if (!ok || !File.Exists(outFile))
            {
                error = "texconv failed for " + Path.GetFileName(ddsPath) +
                        (string.IsNullOrEmpty(output) ? "" : ":\n" + output.Trim());
                return false;
            }
            return true;
        }

        /// <summary>Batch-encode every *.png under <paramref name="folder"/> to a BC7
        /// .dds beside it (recursive). texconv is invoked once per containing
        /// directory to keep process spawns down. When <paramref
        /// name="deleteSource"/>, each source .png that converted is removed — the
        /// game only takes the .dds fast path when the .png isn't also present.
        /// Returns (converted, failed); <paramref name="firstError"/> holds the
        /// first failure message.</summary>
        public static void EncodeFolder(string folder, bool deleteSource,
                                        Action<int, int, string> report,
                                        out int converted, out int failed, out string firstError)
        {
            converted = 0;
            failed = 0;
            firstError = null;

            List<string> pngs;
            try { pngs = new List<string>(Directory.EnumerateFiles(folder, "*.png", SearchOption.AllDirectories)); }
            catch (Exception ex) { firstError = ex.Message; return; }

            // Group by directory so one texconv call handles all PNGs in a folder.
            var byDir = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (var p in pngs)
            {
                string d = Path.GetDirectoryName(p);
                if (!byDir.ContainsKey(d)) byDir[d] = new List<string>();
                byDir[d].Add(p);
            }

            int done = 0;
            foreach (var kv in byDir)
            {
                string dir = kv.Key;
                var    files = kv.Value;
                var    sb = new StringBuilder("-nologo -f BC7_UNORM -m 0 -y -o " + Quote(dir));
                foreach (var f in files) sb.Append(' ').Append(Quote(f));

                string output;
                Run(sb.ToString(), out output);

                foreach (var f in files)
                {
                    done++;
                    if (report != null)
                        report(done, pngs.Count, "Encoding " + Path.GetFileName(f) + "  (" + done + "/" + pngs.Count + ")");

                    string dds = Path.Combine(dir, Path.GetFileNameWithoutExtension(f) + ".dds");
                    if (File.Exists(dds))
                    {
                        converted++;
                        if (deleteSource)
                        {
                            try { File.Delete(f); } catch { /* leave the png if locked */ }
                        }
                    }
                    else
                    {
                        failed++;
                        if (firstError == null)
                            firstError = "texconv failed for " + Path.GetFileName(f) +
                                         (string.IsNullOrEmpty(output) ? "" : ":\n" + output.Trim());
                    }
                }
            }
        }
    }
}
