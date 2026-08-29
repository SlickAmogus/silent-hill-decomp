using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>Launcher UI language.</summary>
    public enum LauncherLang
    {
        English = 0, Spanish, Portuguese, French, German,
        Italian, Japanese, Chinese, Russian, Polish
    }

    /// <summary>
    /// Launcher localization.
    ///
    /// Translation is keyed by the ENGLISH string a control already carries, so
    /// no form needs its own resource file and no designer output has to change:
    /// each form calls <see cref="Apply"/> once after InitializeComponent and the
    /// walker rewrites whatever it recognises. A string that is not in the table
    /// is left exactly as it was, which is what keeps this safe to run over a
    /// whole window — the only text that can change is text listed here.
    ///
    /// Two things are deliberately never touched:
    ///   * TextBox / ComboBox / NumericUpDown, whose Text is user data. The
    ///     launcher reads combo items back with SelectedItem.ToString() (the
    ///     resolution and FPS lists parse them), so translating an item would
    ///     change what gets written to config.cfg.
    ///   * Anything whose English is a proper noun, an acronym or a file
    ///     extension ("PGXP", "RA", ".TIM", "BC7").
    ///
    /// The original English of every control the walker touches is remembered,
    /// so switching language a second time re-translates from English rather
    /// than trying to translate an already-translated string.
    /// </summary>
    public static partial class Loc
    {
        public static LauncherLang Current = LauncherLang.English;

        /// <summary>Raised after Current changes, so open forms can re-apply.</summary>
        public static event Action Changed;

        static readonly ConditionalWeakTable<object, string> s_English =
            new ConditionalWeakTable<object, string>();

        // ---- language metadata -------------------------------------------------

        public static readonly LauncherLang[] All =
        {
            LauncherLang.English, LauncherLang.Spanish, LauncherLang.Portuguese,
            LauncherLang.French,  LauncherLang.German,  LauncherLang.Italian,
            LauncherLang.Japanese, LauncherLang.Chinese, LauncherLang.Russian,
            LauncherLang.Polish
        };

        /// <summary>Regional-indicator flag for the language's most common locale.</summary>
        public static string Flag(LauncherLang l)
        {
            switch (l)
            {
                case LauncherLang.Spanish:    return "\U0001F1EA\U0001F1F8"; // ES
                case LauncherLang.Portuguese: return "\U0001F1E7\U0001F1F7"; // BR
                case LauncherLang.French:     return "\U0001F1EB\U0001F1F7"; // FR
                case LauncherLang.German:     return "\U0001F1E9\U0001F1EA"; // DE
                case LauncherLang.Italian:    return "\U0001F1EE\U0001F1F9"; // IT
                case LauncherLang.Japanese:   return "\U0001F1EF\U0001F1F5"; // JP
                case LauncherLang.Chinese:    return "\U0001F1E8\U0001F1F3"; // CN
                case LauncherLang.Russian:    return "\U0001F1F7\U0001F1FA"; // RU
                case LauncherLang.Polish:     return "\U0001F1F5\U0001F1F1"; // PL
                default:                      return "\U0001F1FA\U0001F1F8"; // US
            }
        }

        /// <summary>The language's name in its own language.</summary>
        public static string NativeName(LauncherLang l)
        {
            switch (l)
            {
                case LauncherLang.Spanish:    return "Español";
                case LauncherLang.Portuguese: return "Português";
                case LauncherLang.French:     return "Français";
                case LauncherLang.German:     return "Deutsch";
                case LauncherLang.Italian:    return "Italiano";
                case LauncherLang.Japanese:   return "日本語";
                case LauncherLang.Chinese:    return "中文";
                case LauncherLang.Russian:    return "Русский";
                case LauncherLang.Polish:     return "Polski";
                default:                      return "English";
            }
        }

        /// <summary>config.cfg value for launcher_language.</summary>
        public static string Code(LauncherLang l)
        {
            switch (l)
            {
                case LauncherLang.Spanish: return "es";
                case LauncherLang.Portuguese: return "pt";
                case LauncherLang.French: return "fr";
                case LauncherLang.German: return "de";
                case LauncherLang.Italian: return "it";
                case LauncherLang.Japanese: return "ja";
                case LauncherLang.Chinese: return "zh";
                case LauncherLang.Russian: return "ru";
                case LauncherLang.Polish: return "pl";
                default: return "en";
            }
        }

        public static LauncherLang FromCode(string code)
        {
            if (string.IsNullOrWhiteSpace(code)) return LauncherLang.English;
            foreach (var l in All)
                if (string.Equals(Code(l), code.Trim(), StringComparison.OrdinalIgnoreCase))
                    return l;
            return LauncherLang.English;
        }

        public static void Set(LauncherLang l)
        {
            if (l == Current) return;
            Current = l;
            var h = Changed;
            if (h != null) h();
        }

        // ---- lookup ------------------------------------------------------------

        /// <summary>Translate one English UI string; unknown strings pass through.</summary>
        public static string T(string english)
        {
            if (string.IsNullOrEmpty(english) || Current == LauncherLang.English)
                return english;
            string[] row;
            if (!Table.TryGetValue(english, out row)) return english;
            int i = (int)Current - 1;
            if (i < 0 || i >= row.Length) return english;
            return string.IsNullOrEmpty(row[i]) ? english : row[i];
        }

        static bool Translatable(Control c)
        {
            // Text is data, not label, on these.
            if (c is TextBoxBase || c is ComboBox || c is NumericUpDown ||
                c is ListBox || c is ListView || c is TrackBar || c is ProgressBar)
                return false;
            return true;
        }

        static string Original(object o, string current)
        {
            string en;
            if (s_English.TryGetValue(o, out en)) return en;
            s_English.Add(o, current);
            return current;
        }

        /// <summary>
        /// Re-label every control under <paramref name="root"/> that this table
        /// knows. Safe to call repeatedly — the first call remembers each
        /// control's English text and later calls translate from that.
        /// </summary>
        public static void Apply(Control root)
        {
            if (root == null) return;

            if (Translatable(root))
                root.Text = T(Original(root, root.Text));

            var form = root as Form;
            if (form != null && form.MainMenuStrip != null)
                ApplyMenu(form.MainMenuStrip.Items);

            foreach (Control c in root.Controls)
            {
                // Owner-drawn combos paint their text through T() at draw time,
                // so a repaint is all a language switch needs.
                var cb = c as ComboBox;
                if (cb != null && cb.DrawMode == DrawMode.OwnerDrawFixed) cb.Invalidate();

                var ts = c as ToolStrip;
                if (ts != null) ApplyMenu(ts.Items);

                var lv = c as ListView;
                if (lv != null)
                    foreach (ColumnHeader h in lv.Columns)
                        h.Text = T(Original(h, h.Text));

                Apply(c);
            }

            if (root.ContextMenuStrip != null)
                ApplyMenu(root.ContextMenuStrip.Items);
        }


        /// <summary>
        /// Show a DropDownList's items translated while leaving the items
        /// themselves English.
        ///
        /// This is the only safe way to translate these: the launcher reads the
        /// selection back with SelectedItem.ToString() and writes it straight to
        /// config.cfg (resolution, fps_cap, map, and the value lists here), so
        /// replacing the item objects would change what gets saved. Owner-draw
        /// changes only what is painted. Every combo on the form is
        /// DropDownList, so this covers the closed box as well as the open list.
        /// </summary>
        public static void LocalizeItems(ComboBox cb)
        {
            if (cb == null) return;
            cb.DrawMode = DrawMode.OwnerDrawFixed;
            cb.DrawItem += (s, e) =>
            {
                e.DrawBackground();
                if (e.Index >= 0 && e.Index < cb.Items.Count)
                {
                    var text = T(Convert.ToString(cb.Items[e.Index]));
                    TextRenderer.DrawText(e.Graphics, text, e.Font ?? cb.Font, e.Bounds,
                        e.ForeColor,
                        TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
                        TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
                }
                e.DrawFocusRectangle();
            };
            cb.Invalidate();
        }

        public static void ApplyMenu(ToolStripItemCollection items)
        {
            if (items == null) return;
            foreach (ToolStripItem it in items)
            {
                it.Text = T(Original(it, it.Text));
                if (!string.IsNullOrEmpty(it.ToolTipText))
                    it.ToolTipText = T(Original(it.ToolTipText, it.ToolTipText));
                var dd = it as ToolStripDropDownItem;
                if (dd != null && dd.HasDropDownItems) ApplyMenu(dd.DropDownItems);
            }
        }

        // The table itself lives in Loc.Strings.cs.
    }
}
