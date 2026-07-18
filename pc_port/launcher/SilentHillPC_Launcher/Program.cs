using System;
using System.Threading;
using System.Windows.Forms;

internal static class Program
{
    // Held for the process lifetime so the named mutex stays alive while we run;
    // a static field is a GC root, so it won't be collected mid-session.
    static Mutex s_singleInstance;

    [STAThread]
    static void Main()
    {
        bool createdNew;
        s_singleInstance = new Mutex(false, @"Local\SilentHillPC_Launcher_SingleInstance", out createdNew);
        if (!createdNew)
        {
            MessageBox.Show(
                "Silent Hill PC Launcher is already running.",
                "Silent Hill PC Launcher",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Form1());
    }
}
