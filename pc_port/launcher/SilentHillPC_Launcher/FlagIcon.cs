using System.Drawing;
using System.Drawing.Drawing2D;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Draws a small flag for each launcher language.
    ///
    /// Emoji flags are not an option here: they are regional-indicator PAIRS,
    /// and Windows only ligates those into a flag through DirectWrite. GDI —
    /// which is what WinForms text rendering uses — draws the two letters
    /// instead, so a button with "\U0001F1FA\U0001F1F8" on it reads "US" no
    /// matter which font is set. These are drawn as shapes for that reason.
    ///
    /// They are deliberately simple: at ~22x14 a flag is read by its colour
    /// layout, so the US canton carries a suggestion of stars rather than 50 of
    /// them, and China gets one star rather than five.
    /// </summary>
    public static class FlagIcon
    {
        public static void Draw(Graphics g, Rectangle r, LauncherLang lang)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            switch (lang)
            {
                case LauncherLang.Spanish:   Bands(g, r, false, Rgb(0xAA151B), Rgb(0xF1BF00), Rgb(0xAA151B), 0.25f); break;
                case LauncherLang.French:    Bands(g, r, true,  Rgb(0x0055A4), Color.White,   Rgb(0xEF4135)); break;
                case LauncherLang.German:    Bands(g, r, false, Color.Black,   Rgb(0xDD0000), Rgb(0xFFCE00)); break;
                case LauncherLang.Italian:   Bands(g, r, true,  Rgb(0x008C45), Rgb(0xF4F5F0), Rgb(0xCD212A)); break;
                case LauncherLang.Russian:   Bands(g, r, false, Color.White,   Rgb(0x0039A6), Rgb(0xD52B1E)); break;

                case LauncherLang.Polish:
                    Fill(g, r, Color.White);
                    using (var b = new SolidBrush(Rgb(0xDC143C)))
                        g.FillRectangle(b, r.X, r.Y + r.Height / 2, r.Width, r.Height - r.Height / 2);
                    break;

                case LauncherLang.Japanese:
                    Fill(g, r, Color.White);
                    Disc(g, r, Rgb(0xBC002D), 0.36f);
                    break;

                case LauncherLang.Chinese:
                    Fill(g, r, Rgb(0xDE2910));
                    Star(g, new PointF(r.X + r.Width * 0.26f, r.Y + r.Height * 0.36f),
                         r.Height * 0.26f, Rgb(0xFFDE00));
                    break;

                case LauncherLang.Portuguese: // Brazil — the Portuguese-speaking build audience
                    Fill(g, r, Rgb(0x009C3B));
                    using (var b = new SolidBrush(Rgb(0xFFDF00)))
                        g.FillPolygon(b, new[]
                        {
                            new PointF(r.X + r.Width * 0.5f,  r.Y + r.Height * 0.12f),
                            new PointF(r.X + r.Width * 0.88f, r.Y + r.Height * 0.5f),
                            new PointF(r.X + r.Width * 0.5f,  r.Y + r.Height * 0.88f),
                            new PointF(r.X + r.Width * 0.12f, r.Y + r.Height * 0.5f)
                        });
                    Disc(g, r, Rgb(0x002776), 0.2f);
                    break;

                default: // English — the Stars and Stripes, suggested rather than drawn
                    // Five bands, not thirteen: at this size real stripes are a
                    // sub-pixel each and average out to flat pink.
                    g.SmoothingMode = SmoothingMode.None;
                    Fill(g, r, Color.White);
                    using (var b = new SolidBrush(Rgb(0xB22234)))
                        for (int i = 0; i < 3; i++)
                        {
                            int y0 = r.Y + i * 2 * r.Height / 5;
                            int y1 = r.Y + (i * 2 + 1) * r.Height / 5;
                            if (y1 > y0) g.FillRectangle(b, r.X, y0, r.Width, y1 - y0);
                        }
                    using (var b = new SolidBrush(Rgb(0x3C3B6E)))
                        g.FillRectangle(b, r.X, r.Y, (int)(r.Width * 0.44f), r.Height * 3 / 5);
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    break;
            }

            using (var p = new Pen(Color.FromArgb(110, 0, 0, 0)))
                g.DrawRectangle(p, r.X, r.Y, r.Width - 1, r.Height - 1);

            g.SmoothingMode = old;
        }

        static Color Rgb(int v) => Color.FromArgb((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);

        static void Fill(Graphics g, Rectangle r, Color c)
        {
            using (var b = new SolidBrush(c)) g.FillRectangle(b, r);
        }

        /// <summary>Three bands, vertical when <paramref name="vertical"/>.
        /// <paramref name="midFrac"/> lets Spain's wide yellow band differ.</summary>
        static void Bands(Graphics g, Rectangle r, bool vertical, Color a, Color b, Color c,
                          float midFrac = 1f / 3f)
        {
            var prev = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.None;
            float side = (1f - midFrac) / 2f;
            if (vertical)
            {
                int w0 = (int)(r.Width * side), w1 = (int)(r.Width * midFrac);
                using (var br = new SolidBrush(a)) g.FillRectangle(br, r.X, r.Y, w0, r.Height);
                using (var br = new SolidBrush(b)) g.FillRectangle(br, r.X + w0, r.Y, w1, r.Height);
                using (var br = new SolidBrush(c)) g.FillRectangle(br, r.X + w0 + w1, r.Y, r.Width - w0 - w1, r.Height);
            }
            else
            {
                int h0 = (int)(r.Height * side), h1 = (int)(r.Height * midFrac);
                using (var br = new SolidBrush(a)) g.FillRectangle(br, r.X, r.Y, r.Width, h0);
                using (var br = new SolidBrush(b)) g.FillRectangle(br, r.X, r.Y + h0, r.Width, h1);
                using (var br = new SolidBrush(c)) g.FillRectangle(br, r.X, r.Y + h0 + h1, r.Width, r.Height - h0 - h1);
            }
            g.SmoothingMode = prev;
        }

        static void Disc(Graphics g, Rectangle r, Color c, float frac)
        {
            float d = r.Height * frac * 2f;
            using (var b = new SolidBrush(c))
                g.FillEllipse(b, r.X + (r.Width - d) / 2f, r.Y + (r.Height - d) / 2f, d, d);
        }

        static void Star(Graphics g, PointF centre, float radius, Color c)
        {
            var pts = new PointF[10];
            for (int i = 0; i < 10; i++)
            {
                double ang = -System.Math.PI / 2 + i * System.Math.PI / 5;
                float rad = (i % 2 == 0) ? radius : radius * 0.42f;
                pts[i] = new PointF(centre.X + (float)(System.Math.Cos(ang) * rad),
                                    centre.Y + (float)(System.Math.Sin(ang) * rad));
            }
            using (var b = new SolidBrush(c)) g.FillPolygon(b, pts);
        }
    }
}
