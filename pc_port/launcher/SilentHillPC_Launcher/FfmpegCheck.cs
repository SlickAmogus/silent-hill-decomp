using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The game compiles its ffmpeg FMV path in but ships NO ffmpeg DLLs
    /// (redistributing them drags in LGPL/GPL duties and H.264/HEVC patent-pool
    /// exposure). A user who installs an FMV mod in a modern container has to
    /// supply the runtime himself, and without it the game silently falls back to
    /// the original disc movie — so tell him why, once, at launcher start.
    ///
    /// Keep the container list and the library majors in agreement with
    /// pc_port/src/fmv/fmv_player.cpp: it probes gamedata/fmv/&lt;name&gt;.{mp4,mkv,
    /// webm,mov} and loads avcodec 62 / avformat 62 / avutil 60 / swscale 9 /
    /// swresample 6 by bare name, refusing any other major (we touch struct
    /// fields directly, so a different major is an ABI break).
    /// </summary>
    public static class FfmpegCheck
    {
        private static readonly string[] s_containerPatterns = { "*.mp4", "*.mkv", "*.webm", "*.mov" };

        private static readonly string[] s_requiredLibs =
        {
            "avcodec-62", "avformat-62", "avutil-60", "swscale-9", "swresample-6"
        };

        private const string GyanUrl = "https://www.gyan.dev/ffmpeg/builds/";
        private const string BtbNUrl = "https://github.com/BtbN/FFmpeg-Builds/releases";

        /// <summary>
        /// True when gamedata/fmv/ holds at least one modern-container movie, i.e.
        /// an override only the ffmpeg path can play. Missing folder = false.
        /// </summary>
        public static bool HasModernFmvOverride(string gameRoot)
        {
            try
            {
                string fmvDir = Path.Combine(Path.Combine(gameRoot, "gamedata"), "fmv");
                if (!Directory.Exists(fmvDir)) return false;

                foreach (var pattern in s_containerPatterns)
                    if (Directory.GetFiles(fmvDir, pattern, SearchOption.TopDirectoryOnly).Length > 0)
                        return true;
            }
            catch { /* unreadable gamedata — treat as "no mod", never block */ }
            return false;
        }

        /// <summary>
        /// The required libraries that resolve nowhere the loader would look:
        /// the exe's own folder first, then PATH. Both the bare and the
        /// 'lib'-prefixed spelling count, since builds differ.
        /// </summary>
        public static List<string> MissingLibs(string gameRoot)
        {
            var dirs    = SearchDirs(gameRoot);
            var missing = new List<string>();

            foreach (var lib in s_requiredLibs)
                if (!ResolvesAnywhere(dirs, lib))
                    missing.Add(lib + ".dll");

            return missing;
        }

        private static bool ResolvesAnywhere(List<string> dirs, string lib)
        {
            foreach (var dir in dirs)
                if (FileExistsSafe(dir, lib + ".dll") || FileExistsSafe(dir, "lib" + lib + ".dll"))
                    return true;
            return false;
        }

        private static bool FileExistsSafe(string dir, string file)
        {
            try { return File.Exists(Path.Combine(dir, file)); }
            catch { return false; } // bad chars in a PATH entry, unreachable share, ...
        }

        private static List<string> SearchDirs(string gameRoot)
        {
            var dirs = new List<string>();
            if (!string.IsNullOrEmpty(gameRoot)) dirs.Add(gameRoot);

            string path = "";
            try { path = Environment.GetEnvironmentVariable("PATH") ?? ""; }
            catch { }

            foreach (var entry in path.Split(Path.PathSeparator))
            {
                string d = entry.Trim().Trim('"');
                if (d.Length > 0) dirs.Add(d);
            }
            return dirs;
        }

        /// <summary>
        /// Show the "you need the ffmpeg runtime" notice when — and only when — a
        /// modern-container FMV mod is installed and the DLLs can't be found.
        /// Advisory only: it never blocks Play and never throws.
        /// </summary>
        public static void WarnIfNeeded(IWin32Window owner, string gameRoot)
        {
            try
            {
                if (!HasModernFmvOverride(gameRoot)) return;
                if (MissingLibs(gameRoot).Count == 0) return;

                ShowNotice(owner);
            }
            catch { /* advisory only — never keep the user from launching */ }
        }

        private static void ShowNotice(IWin32Window owner)
        {
            string text =
                "An FMV mod using a modern video container (.mp4 / .mkv / .webm / .mov) was found in " +
                "gamedata\\fmv, but the ffmpeg runtime it needs isn't installed.\r\n\r\n" +
                "This is OPTIONAL. The game and every original movie work fine without it — only those " +
                "modded FMVs are affected, and they are skipped so the original disc movie plays " +
                "instead.\r\n\r\n" +
                "To play them, download an ffmpeg 8.x SHARED build for Windows and copy these DLLs next " +
                "to SilentHillPC.exe:\r\n\r\n" +
                "    avcodec-62.dll    avformat-62.dll    avutil-60.dll\r\n" +
                "    swscale-9.dll    swresample-6.dll\r\n\r\n" +
                "Recommended builds:\r\n" +
                "    " + GyanUrl + "\r\n" +
                "    " + BtbNUrl + "\r\n\r\n" +
                "Pick a \"shared\" 8.x package and take the DLLs from its bin folder (the 'lib'-prefixed " +
                "spelling works too). An ffmpeg 8.x already on your PATH is used as well. It must be " +
                "8.x — the game refuses any other version rather than risk a crash.";

            using (var dlg = new Form())
            {
                dlg.Text = "FFmpeg runtime not found";
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition = FormStartPosition.CenterParent;
                dlg.MaximizeBox = false;
                dlg.MinimizeBox = false;
                dlg.ShowInTaskbar = false;
                dlg.ClientSize = new Size(520, 355);

                var link = new LinkLabel
                {
                    Text = text,
                    AutoSize = false,
                    Location = new Point(0, 0),
                    Size = new Size(520, 318),
                    Padding = new Padding(12),
                    LinkBehavior = LinkBehavior.HoverUnderline,
                };

                void AddLink(string url)
                {
                    int idx = text.IndexOf(url, StringComparison.Ordinal);
                    if (idx >= 0)
                        link.Links.Add(idx, url.Length, url);
                }
                AddLink(GyanUrl);
                AddLink(BtbNUrl);

                link.LinkClicked += (s, e) =>
                {
                    var url = e.Link.LinkData as string;
                    if (string.IsNullOrEmpty(url)) return;
                    try { Process.Start(url); } catch { /* no browser / blocked */ }
                };

                var ok = new Button
                {
                    Text = "OK",
                    DialogResult = DialogResult.OK,
                    Size = new Size(75, 23),
                    Location = new Point(433, 323),
                };

                dlg.Controls.Add(link);
                dlg.Controls.Add(ok);
                dlg.AcceptButton = ok;
                dlg.ShowDialog(owner);
            }
        }
    }
}
