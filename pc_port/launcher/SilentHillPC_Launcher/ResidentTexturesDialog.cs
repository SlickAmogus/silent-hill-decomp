using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Shown at Play when config.cfg carries `resident_textures = 0`.
    ///
    /// That setting is a compatibility fallback, not a performance option, and
    /// leaving it off silently costs a lot: the game reverts to the vanilla
    /// 8-full/2-half VRAM page pool, so distant map chunks lose their pages to
    /// nearer ones and render flat or garbled; texture-pack art is composed
    /// eagerly at load instead of on demand within a frame budget, which is the
    /// worse stutter; and the pack VRAM budget has no eviction on that path, so
    /// once it fills, textures simply stop upgrading for the rest of the run.
    ///
    /// It is worth ASKING rather than silently forcing, because the fallback is
    /// genuinely the right answer on some hardware — which is exactly why it
    /// exists — and because a user who deliberately set it should not have the
    /// launcher overrule them.
    /// </summary>
    internal sealed class ResidentTexturesDialog : Form
    {
        /// <summary>What the user chose.</summary>
        public enum Choice { KeepOff, TurnOn, NeverAsk }

        public Choice Result { get; private set; }

        public ResidentTexturesDialog()
        {
            Result = Choice.KeepOff;

            Text = "Resident textures are turned off";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ShowInTaskbar = false;
            ClientSize = new Size(520, 250);

            var head = new Label
            {
                Location = new Point(14, 14),
                Size = new Size(492, 22),
                Font = new Font(SystemFonts.MessageBoxFont, FontStyle.Bold),
                Text = "Your config has resident_textures = 0.",
            };

            var body = new Label
            {
                Location = new Point(14, 42),
                Size = new Size(492, 150),
                Text =
                    "This turns off the expanded texture pool and falls back to the original " +
                    "PlayStation VRAM behaviour. With it off you can expect:\r\n\r\n" +
                    "    •  distant walls and floors losing their textures, or showing\r\n" +
                    "        garbled / rainbow-coloured surfaces\r\n" +
                    "    •  more stuttering while texture packs load\r\n" +
                    "    •  HD texture packs quietly stopping partway through a session\r\n\r\n" +
                    "It is normally only worth using on older hardware, or if you get a lot of " +
                    "graphical glitches with it turned on.\r\n\r\n" +
                    "Would you like to turn it on?",
            };

            var yes = new Button
            {
                Text = "&Yes, turn it on",
                Location = new Point(14, 206),
                Size = new Size(140, 30),
                DialogResult = DialogResult.OK,
            };
            yes.Click += (s, e) => { Result = Choice.TurnOn; };

            var no = new Button
            {
                Text = "&No, leave it off",
                Location = new Point(162, 206),
                Size = new Size(140, 30),
                DialogResult = DialogResult.OK,
            };
            no.Click += (s, e) => { Result = Choice.KeepOff; };

            var never = new Button
            {
                Text = "&Don't ask again",
                Location = new Point(346, 206),
                Size = new Size(160, 30),
                DialogResult = DialogResult.OK,
            };
            never.Click += (s, e) => { Result = Choice.NeverAsk; };

            Controls.Add(head);
            Controls.Add(body);
            Controls.Add(yes);
            Controls.Add(no);
            Controls.Add(never);
            AcceptButton = yes;
            // Closing with Esc or the X is "leave it off, ask me again" — the least
            // surprising reading of dismissing a question.
            CancelButton = no;
        }
    }
}
