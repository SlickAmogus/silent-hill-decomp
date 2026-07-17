using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Extracts .zip and .7z (and anything else 7-Zip reads) via the standalone
    /// 7za.exe console tool. The exe is EMBEDDED in the launcher and materialized
    /// to a temp path on first use, then invoked as a separate PROCESS — so, unlike
    /// the UnRAR.dll P/Invoke, its bitness is irrelevant (a 32-bit 7za.exe runs fine
    /// from a 64-bit launcher). Using 7-Zip for zips (instead of .NET's
    /// System.IO.Compression) is deliberate: miniz and ZipFile only decode
    /// Store/Deflate, so an LZMA/PPMd-compressed .zip would silently unpack to
    /// nothing; 7za handles every method.
    ///
    /// 7-Zip is (C) Igor Pavlov, distributed under the GNU LGPL (see the bundled
    /// LICENSE-7zip.txt). It is invoked as an unmodified separate program, so no
    /// copyleft obligation propagates to the launcher beyond shipping that notice.
    /// </summary>
    public static class SevenZipExtractor
    {
        private const string EmbeddedName = "SilentHillPC_Launcher.7za.exe";

        private static readonly object _lock = new object();
        private static bool   _tried;
        private static string _exePath; // materialized 7za.exe, or a PATH fallback

        /// <summary>Materialize the embedded 7za.exe (once). Returns whether a usable
        /// 7za/7z executable is available.</summary>
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
                    string path = Path.Combine(dir, "7za.exe");

                    byte[] bytes = ReadEmbedded();
                    if (bytes != null)
                    {
                        bool needWrite = !File.Exists(path) || new FileInfo(path).Length != bytes.Length;
                        if (needWrite)
                        {
                            try { File.WriteAllBytes(path, bytes); }
                            catch { /* a prior run may hold it open; fall through and use what's there */ }
                        }
                        if (File.Exists(path)) { _exePath = path; return _exePath; }
                    }

                    // Fallbacks: a copy beside the launcher, or 7za/7z on PATH.
                    string beside = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "7za.exe");
                    if (File.Exists(beside)) { _exePath = beside; return _exePath; }
                    foreach (var name in new[] { "7za.exe", "7z.exe" })
                    {
                        string onPath = FindOnPath(name);
                        if (onPath != null) { _exePath = onPath; return _exePath; }
                    }
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

        /// <summary>Extract every entry of <paramref name="archivePath"/> into
        /// <paramref name="destDir"/> (full paths preserved). Returns false on any
        /// failure (missing tool, nonzero exit, exception).</summary>
        public static bool Extract(string archivePath, string destDir, Action<int, int, string> report)
        {
            string exe = ResolveExe();
            if (exe == null) return false;

            try
            {
                Directory.CreateDirectory(destDir);

                var psi = new ProcessStartInfo
                {
                    FileName               = exe,
                    // x = extract with full paths; -y = assume yes; -bd = no progress
                    // bar; -o<dir> = output dir (no space after -o, 7-Zip syntax).
                    Arguments              = "x -y -bd \"" + archivePath + "\" -o\"" + destDir + "\"",
                    UseShellExecute        = false,
                    CreateNoWindow         = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError  = true,
                    WorkingDirectory       = destDir
                };

                using (var proc = new Process { StartInfo = psi })
                {
                    int files = 0;
                    proc.OutputDataReceived += (s, e) =>
                    {
                        if (report == null || string.IsNullOrEmpty(e.Data)) return;
                        // 7za prints "- <name>" per file with -bb1; with default output
                        // it prints a header + summary. Surface any file-ish line.
                        string line = e.Data.Trim();
                        if (line.Length == 0) return;
                        if (line.StartsWith("- ") || line.StartsWith("Extracting"))
                        {
                            files++;
                            report(0, 0, string.Format("Extracting {0}  ({1} files)",
                                Path.GetFileName(archivePath), files));
                        }
                    };

                    proc.Start();
                    proc.BeginOutputReadLine();
                    proc.BeginErrorReadLine();
                    proc.WaitForExit();
                    // 7-Zip exit codes: 0 = OK, 1 = warning (still usable), >=2 = error.
                    return proc.ExitCode == 0 || proc.ExitCode == 1;
                }
            }
            catch
            {
                return false;
            }
        }
    }
}
