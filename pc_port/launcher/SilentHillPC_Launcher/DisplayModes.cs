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

    private const int ENUM_CURRENT_SETTINGS = -1;
    private const int DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x00000001;

    private const int HORZRES = 8;          // virtualized width  this process sees
    private const int VERTRES = 10;         // virtualized height this process sees
    private const int DESKTOPHORZRES = 118; // TRUE physical width
    private const int DESKTOPVERTRES = 117; // TRUE physical height

    /// <summary>
    /// Physical-to-virtual pixel ratio for this process.
    ///
    /// app.manifest sets dpiAware=false on purpose (the forms are fixed-pixel
    /// 96-DPI layouts and turning it on clips every control), which means
    /// Windows virtualizes display metrics for us: on a 3440x1440 panel at 150%
    /// scaling, EnumDisplaySettings reports 2293x960 and the real mode is
    /// nowhere in the list. That is why ultrawide users reported their
    /// resolution "not detected".
    ///
    /// GetDeviceCaps(DESKTOPHORZRES) reports true physical pixels even to a
    /// non-aware process, so the ratio against HORZRES recovers the scale and
    /// lets the enumerated modes be converted back to physical.
    /// </summary>
    private static void GetDpiScale(out double sx, out double sy)
    {
        sx = 1.0;
        sy = 1.0;
        IntPtr hdc = GetDC(IntPtr.Zero);
        if (hdc == IntPtr.Zero)
            return;
        try
        {
            int virtW = GetDeviceCaps(hdc, HORZRES);
            int virtH = GetDeviceCaps(hdc, VERTRES);
            int physW = GetDeviceCaps(hdc, DESKTOPHORZRES);
            int physH = GetDeviceCaps(hdc, DESKTOPVERTRES);
            if (virtW > 0 && physW > 0) sx = (double)physW / virtW;
            if (virtH > 0 && physH > 0) sy = (double)physH / virtH;
        }
        finally
        {
            ReleaseDC(IntPtr.Zero, hdc);
        }
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
        var entry = (pw, ph, hz);
        if (!list.Contains(entry))
            list.Add(entry);
    }

    public static List<(int width, int height, int hz)> GetModes()
    {
        var list = new List<(int, int, int)>();
        double sx, sy;
        GetDpiScale(out sx, out sy);

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
