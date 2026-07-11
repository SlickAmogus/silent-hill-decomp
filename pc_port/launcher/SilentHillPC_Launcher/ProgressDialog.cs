using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Tiny modal progress window that runs a long file operation on a background
    /// thread so the manager window doesn't freeze. The work delegate is handed a
    /// <c>report(current, total, message)</c> callback: total &gt; 0 shows a
    /// determinate bar, total &lt;= 0 shows a marquee. Marshals every update back
    /// to the UI thread. No cancel — extraction/copy runs to completion.
    /// </summary>
    public class ProgressDialog : Form
    {
        private readonly ProgressBar _bar;
        private readonly Label _label;
        private readonly Action<Action<int, int, string>> _work;
        private Exception _error;

        private ProgressDialog(string title, Action<Action<int, int, string>> work)
        {
            _work = work;

            Text            = title;
            ClientSize      = new Size(410, 92);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition   = FormStartPosition.CenterParent;
            ControlBox      = false;
            MaximizeBox     = false;
            MinimizeBox     = false;
            ShowInTaskbar   = false;

            _label = new Label { Location = new Point(12, 14), Size = new Size(386, 20),
                                 Text = "Working…", AutoEllipsis = true };
            _bar = new ProgressBar { Location = new Point(12, 42), Size = new Size(386, 22),
                                     Minimum = 0, Maximum = 100, Style = ProgressBarStyle.Marquee,
                                     MarqueeAnimationSpeed = 30 };
            Controls.Add(_label);
            Controls.Add(_bar);

            Shown += OnShown;
        }

        private void OnShown(object sender, EventArgs e)
        {
            var t = new Thread(() =>
            {
                try { _work(Report); }
                catch (Exception ex) { _error = ex; }
                try { BeginInvoke((Action)Close); } catch { }
            });
            t.IsBackground = true;
            t.Start();
        }

        private void Report(int current, int total, string message)
        {
            try
            {
                BeginInvoke((Action)(() =>
                {
                    if (total > 0)
                    {
                        if (_bar.Style != ProgressBarStyle.Blocks) _bar.Style = ProgressBarStyle.Blocks;
                        _bar.Maximum = total;
                        _bar.Value   = Math.Max(0, Math.Min(current, total));
                    }
                    else if (_bar.Style != ProgressBarStyle.Marquee)
                    {
                        _bar.Style = ProgressBarStyle.Marquee;
                    }
                    if (message != null) _label.Text = message;
                }));
            }
            catch { }
        }

        /// <summary>Run <paramref name="work"/> under a modal progress window; rethrows any error.</summary>
        public static void Run(IWin32Window owner, string title, Action<Action<int, int, string>> work)
        {
            using (var d = new ProgressDialog(title, work))
            {
                d.ShowDialog(owner);
                if (d._error != null) throw d._error;
            }
        }
    }
}
