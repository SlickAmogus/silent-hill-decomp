using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DisplayModes
{
    [StructLayout(LayoutKind.Sequential)]
    public struct DEVMODE
    {
        private const int CCHDEVICENAME = 32;
        private const int CCHFORMNAME = 32;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHDEVICENAME)]
        public string dmDeviceName;
        public short dmSpecVersion;
        public short dmDriverVersion;
        public short dmSize;
        public short dmDriverExtra;
        public int dmFields;

        public int dmPositionX;
        public int dmPositionY;
        public int dmDisplayOrientation;
        public int dmDisplayFixedOutput;

        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHFORMNAME)]
        public string dmFormName;

        public short dmLogPixels;
        public int dmBitsPerPel;
        public int dmPelsWidth;
        public int dmPelsHeight;

        public int dmDisplayFlags;
        public int dmDisplayFrequency;

        public int dmICMMethod;
        public int dmICMIntent;
        public int dmMediaType;
        public int dmDitherType;
        public int dmReserved1;
        public int dmReserved2;

        public int dmPanningWidth;
        public int dmPanningHeight;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceKey;
    }

    [DllImport("user32.dll")]
    public static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

    [DllImport("user32.dll", CharSet = CharSet.Ansi)]
    private static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum,
                                                  ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

    [DllImport("gdi32.dll")]
    private static extern int GetDeviceCaps(IntPtr hdc, int nIndex);

    /* Windows 10 1607+ (V2: 1703+). Present on every system the launcher
     * supports in practice, absent on 7/8.1 -- the call is wrapped. */
    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    private static readonly IntPtr DPI_PER_MONITOR_AWARE_V2 = new IntPtr(-4);
    private static readonly IntPtr DPI_PER_MONITOR_AWARE    = new IntPtr(-3);

    private const int ENUM_CURRENT_SETTINGS = -1;
    private const int DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x00000001;

    private const int HORZRES = 8;          // virtualized width  this process sees
    private const int VERTRES = 10;         // virtualized height this process sees
    private const int DESKTOPHORZRES = 118; // TRUE physical width
    private const int DESKTOPVERTRES = 117; // TRUE physical height

    /// <summary>
    /// Stop Windows virtualizing display metrics for THIS THREAD, and return the
    /// previous context so the caller can put it back.
    ///
    /// app.manifest sets dpiAware=false on purpose (the forms are fixed-pixel
    /// 96-DPI layouts and turning it on clips every control), and the price is
    /// that EnumDisplaySettings reports SCALED modes: on a 3840x2160 panel at
    /// 150%, 2560x1440 enumerates as 1706x960 and the real mode is nowhere in
    /// the list. That is why 1440p and ultrawide users report their resolution
    /// "not detected".
    ///
    /// Awareness is per thread, not per process, so this is borrowed for the
    /// length of the enumeration and handed straight back. No window is created
    /// while it is held, so no layout can inherit it.
    /// </summary>
    private static IntPtr EnterPhysicalPixelContext()
    {
        try
        {
            IntPtr prev = SetThreadDpiAwarenessContext(DPI_PER_MONITOR_AWARE_V2);
            if (prev == IntPtr.Zero)
                prev = SetThreadDpiAwarenessContext(DPI_PER_MONITOR_AWARE);
            return prev;
        }
        catch (EntryPointNotFoundException) { return IntPtr.Zero; }
        catch (DllNotFoundException)        { return IntPtr.Zero; }
    }

    private static void LeavePhysicalPixelContext(IntPtr prev)
    {
        if (prev == IntPtr.Zero)
            return;
        try { SetThreadDpiAwarenessContext(prev); }
        catch (EntryPointNotFoundException) { }
        catch (DllNotFoundException)        { }
    }

    /// <summary>
    /// How far the mode enumeration is off from physical pixels, measured
    /// rather than assumed: DESKTOPHORZRES is true physical even to an unaware
    /// process, so comparing it against the width EnumDisplaySettings reports
    /// for the same (primary) display gives the exact factor to undo.
    ///
    /// With the thread context above in force both numbers agree and this
    /// returns 1.0, which is the whole point -- an exact list beats a
    /// reconstructed one. It only has real work to do on Windows 7/8.1, where
    /// the context does not exist.
    /// </summary>
    private static void GetEnumScale(out double sx, out double sy)
    {
        sx = 1.0;
        sy = 1.0;

        int physW = 0, physH = 0;
        IntPtr hdc = GetDC(IntPtr.Zero);
        if (hdc != IntPtr.Zero)
        {
            try
            {
                physW = GetDeviceCaps(hdc, DESKTOPHORZRES);
                physH = GetDeviceCaps(hdc, DESKTOPVERTRES);
            }
            finally
            {
                ReleaseDC(IntPtr.Zero, hdc);
            }
        }
        if (physW <= 0 || physH <= 0)
            return;

        var dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        if (!EnumDisplaySettings(null, ENUM_CURRENT_SETTINGS, ref dm))
            return;

        if (dm.dmPelsWidth  > 0) sx = (double)physW / dm.dmPelsWidth;
        if (dm.dmPelsHeight > 0) sy = (double)physH / dm.dmPelsHeight;
    }

    /* Undoing the scale is lossy in both directions: 2560 virtualized at 150%
     * comes back as 1706 or 1707 depending on which way the driver rounded, and
     * multiplying up again lands on 2559 or 2561. A dropdown entry that is one
     * pixel off is the same thing to the user as a missing one, so a
     * reconstructed size within two pixels of a real display width is taken to
     * BE that width. Only reached when the thread context above was
     * unavailable and the scale is doing work. */
    private static readonly int[] s_commonWidths =
        { 640, 720, 800, 1024, 1152, 1280, 1360, 1366, 1440, 1600, 1680, 1920,
          2048, 2560, 2880, 3200, 3440, 3840, 5120, 7680 };

    private static readonly int[] s_commonHeights =
        { 480, 540, 576, 600, 720, 768, 800, 864, 900, 960, 1024, 1050, 1080,
          1152, 1200, 1440, 1600, 1620, 1800, 2160, 2400, 2880, 4320 };

    private static int SnapToCommon(int v, int[] table)
    {
        foreach (int t in table)
        {
            if (Math.Abs(t - v) <= 2)
                return t;
        }
        return v;
    }

    private static List<string> GetAttachedDeviceNames()
    {
        var names = new List<string>();
        var dd = new DISPLAY_DEVICE();
        dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));

        for (uint i = 0; EnumDisplayDevices(null, i, ref dd, 0); i++)
        {
            if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0)
                names.Add(dd.DeviceName);
            dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        }
        return names;
    }

    private static void AddMode(List<(int, int, int)> list, int w, int h, int hz,
                                double sx, double sy)
    {
        if (w <= 0 || h <= 0)
            return;
        // Back to physical pixels. Rounded, not truncated: 2293 * 1.5 is
        // 3439.5 and truncation would offer "3439x1440".
        int pw = (int)Math.Round(w * sx);
        int ph = (int)Math.Round(h * sy);
        if (sx != 1.0) pw = SnapToCommon(pw, s_commonWidths);
        if (sy != 1.0) ph = SnapToCommon(ph, s_commonHeights);
        var entry = (pw, ph, hz);
        if (!list.Contains(entry))
            list.Add(entry);
    }

    public static List<(int width, int height, int hz)> GetModes()
    {
        IntPtr prevDpiContext = EnterPhysicalPixelContext();
        try
        {
            return EnumerateModes();
        }
        finally
        {
            LeavePhysicalPixelContext(prevDpiContext);
        }
    }

    private static List<(int width, int height, int hz)> EnumerateModes()
    {
        var list = new List<(int, int, int)>();
        double sx, sy;
        GetEnumScale(out sx, out sy);

        // Every attached adapter, not just the primary: an ultrawide is often
        // the second monitor, and EnumDisplaySettings(null) only ever describes
        // the one the calling thread is on.
        var devices = GetAttachedDeviceNames();
        devices.Add(null); // primary, for the case where enumeration found none

        foreach (var dev in devices)
        {
            var dm = new DEVMODE();
            for (int mode = 0; ; mode++)
            {
                // dmSize has to be reset per call; the API overwrites the struct.
                dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
                if (!EnumDisplaySettings(dev, mode, ref dm))
                    break;
                AddMode(list, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, sx, sy);
            }

            // Whatever the display is running right now, always. If the mode
            // list is incomplete for any reason, the resolution the user is
            // actually sitting at still has to appear.
            dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            if (EnumDisplaySettings(dev, ENUM_CURRENT_SETTINGS, ref dm))
                AddMode(list, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, sx, sy);
        }

        return list;
    }
}
