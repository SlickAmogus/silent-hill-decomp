using System;
using System.IO;
using System.Runtime.InteropServices;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Extracts .rar archives via UnRAR.dll (the same vendored unrar the game
    /// links, built as a DLL and shipped next to the launcher). P/Invoke of the
    /// standard UnRAR C API; struct layouts mirror third_party/unrar/dll.hpp
    /// (#pragma pack(1)). Degrades gracefully: if the DLL is missing or anything
    /// fails, Extract returns false and the caller falls back to letting the game
    /// unpack the archive on launch.
    /// </summary>
    public static class RarExtractor
    {
        private const int RAR_OM_EXTRACT   = 1;
        private const int RAR_EXTRACT      = 2;
        private const int ERAR_END_ARCHIVE = 10;

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct RAROpenArchiveDataEx
        {
            public IntPtr ArcName;      // char*
            public IntPtr ArcNameW;     // wchar_t*
            public uint   OpenMode;
            public uint   OpenResult;
            public IntPtr CmtBuf;
            public uint   CmtBufSize;
            public uint   CmtSize;
            public uint   CmtState;
            public uint   Flags;
            public IntPtr Callback;
            public IntPtr UserData;     // LPARAM (pointer-sized)
            public uint   OpFlags;
            public IntPtr CmtBufW;
            public IntPtr MarkOfTheWeb;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 23)] public uint[] Reserved;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct RARHeaderDataEx
        {
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1024)] public byte[] ArcName;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2048)] public byte[] ArcNameW;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1024)] public byte[] FileName;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2048)] public byte[] FileNameW;
            public uint   Flags;
            public uint   PackSize, PackSizeHigh, UnpSize, UnpSizeHigh, HostOS, FileCRC, FileTime, UnpVer, Method, FileAttr;
            public IntPtr CmtBuf;
            public uint   CmtBufSize, CmtSize, CmtState, DictSize, HashType;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] Hash;
            public uint   RedirType;
            public IntPtr RedirName;
            public uint   RedirNameSize, DirTarget, MtimeLow, MtimeHigh, CtimeLow, CtimeHigh, AtimeLow, AtimeHigh;
            public IntPtr ArcNameEx;
            public uint   ArcNameExSize;
            public IntPtr FileNameEx;
            public uint   FileNameExSize;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 982)] public uint[] Reserved;
        }

        [DllImport("UnRAR.dll")] private static extern IntPtr RAROpenArchiveEx(ref RAROpenArchiveDataEx data);
        [DllImport("UnRAR.dll")] private static extern int    RARCloseArchive(IntPtr hArcData);
        [DllImport("UnRAR.dll")] private static extern int    RARReadHeaderEx(IntPtr hArcData, ref RARHeaderDataEx headerData);
        [DllImport("UnRAR.dll", CharSet = CharSet.Unicode)]
        private static extern int RARProcessFileW(IntPtr hArcData, int operation,
                                                  [MarshalAs(UnmanagedType.LPWStr)] string destPath,
                                                  [MarshalAs(UnmanagedType.LPWStr)] string destName);

        public static bool IsAvailable()
        {
            try { return File.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "UnRAR.dll")); }
            catch { return false; }
        }

        /// <summary>Extract every entry of <paramref name="rarPath"/> into <paramref name="destDir"/>.</summary>
        public static bool Extract(string rarPath, string destDir, Action<int, int, string> report)
        {
            IntPtr arcNameW = IntPtr.Zero;
            IntPtr handle   = IntPtr.Zero;
            try
            {
                Directory.CreateDirectory(destDir);
                arcNameW = Marshal.StringToHGlobalUni(rarPath);

                var open = new RAROpenArchiveDataEx
                {
                    ArcNameW = arcNameW,
                    OpenMode = RAR_OM_EXTRACT,
                    Reserved = new uint[23]
                };
                handle = RAROpenArchiveEx(ref open);
                if (handle == IntPtr.Zero || open.OpenResult != 0) return false;

                var hdr = new RARHeaderDataEx
                {
                    ArcName  = new byte[1024], ArcNameW  = new byte[2048],
                    FileName = new byte[1024], FileNameW = new byte[2048],
                    Hash     = new byte[32],   Reserved  = new uint[982]
                };

                int count = 0;
                while (true)
                {
                    int rh = RARReadHeaderEx(handle, ref hdr);
                    if (rh == ERAR_END_ARCHIVE) break;
                    if (rh != 0) return false;

                    int rp = RARProcessFileW(handle, RAR_EXTRACT, destDir, null);
                    if (rp != 0) return false;

                    count++;
                    if (report != null) report(0, 0, string.Format("Extracting {0}  ({1} files)",
                        Path.GetFileName(rarPath), count));
                }
                return true;
            }
            catch
            {
                return false;
            }
            finally
            {
                if (handle != IntPtr.Zero) { try { RARCloseArchive(handle); } catch { } }
                if (arcNameW != IntPtr.Zero) Marshal.FreeHGlobal(arcNameW);
            }
        }
    }
}
