using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public class ConfigManager
{
    private readonly string _path;
    private Dictionary<string, string> _values = new Dictionary<string, string>();
    private List<string> _lines = new List<string>();

    /// <summary>
    /// Keys the launcher itself assigned through <see cref="Set"/>. ONLY these
    /// are written back.
    ///
    /// This set is the whole point of the class's contract: <see cref="_values"/>
    /// holds EVERY key parsed from the file, not just the ones the launcher
    /// owns, so writing all of it meant the launcher rewrote settings it has no
    /// UI for — from a snapshot taken when the launcher STARTED. Editing
    /// config.cfg in a text editor while the launcher was open and then pressing
    /// Play reverted every such edit, and any line added after startup was
    /// deleted outright (it was not in the stale _lines list). resident_textures
    /// and texpack_budget_mb are exactly that case: the launcher does not know
    /// either key, yet it was resetting one and deleting the other.
    /// </summary>
    private readonly HashSet<string> _dirty = new HashSet<string>(StringComparer.Ordinal);

    private bool _wantLauncherSection;

    public ConfigManager(string path)
    {
        _path = path;
        if (File.Exists(path))
            Load();
    }

    public string Get(string key, string defaultValue = "")
    {
        return _values.ContainsKey(key) ? _values[key] : defaultValue;
    }

    public void Set(string key, string value)
    {
        _values[key] = value;
        _dirty.Add(key);
    }

    public void Load()
    {
        _lines = File.ReadAllLines(_path).ToList();
        _values.Clear();
        ParseInto(_lines, _values, null);
    }

    /// <summary>Split a config line into key/value. Returns false for comments
    /// and anything without a '='. The value keeps any further '=' characters,
    /// which a path or a bind name may legitimately contain.</summary>
    private static bool TrySplit(string line, out string key, out string value)
    {
        key = null;
        value = null;
        if (line == null) return false;
        if (line.TrimStart().StartsWith("#")) return false;
        int eq = line.IndexOf('=');
        if (eq < 0) return false;
        key = line.Substring(0, eq).Trim();
        value = line.Substring(eq + 1).Trim();
        return key.Length != 0;
    }

    /// <summary>Parse lines into a dictionary. Keys in <paramref name="skip"/>
    /// are left alone so a re-read cannot clobber a pending launcher edit.</summary>
    private static void ParseInto(List<string> lines, Dictionary<string, string> into, HashSet<string> skip)
    {
        foreach (var line in lines)
        {
            string k, v;
            if (!TrySplit(line, out k, out v)) continue;
            if (skip != null && skip.Contains(k)) continue;
            into[k] = v;
        }
    }

    /// <summary>
    /// Request the "## Launcher" header comment that groups the launcher_* keys
    /// <see cref="Save"/> appends. Recorded rather than applied immediately
    /// because Save re-reads the file from disk and would drop an edit made to
    /// the stale line list. Idempotent.
    /// </summary>
    public void EnsureLauncherSection()
    {
        _wantLauncherSection = true;
    }

    public void Save()
    {
        // Re-read immediately before writing. The in-memory copy dates from
        // launcher startup, and the file may have been edited since — by a text
        // editor, by the game (pc_config.c's PcConfig_SaveKeyValue writes
        // control_style, language, use_pgxp, the flashlight keys and more at
        // runtime), or by another launcher window. Writing the stale copy
        // silently destroyed all of it.
        if (File.Exists(_path))
        {
            try
            {
                _lines = File.ReadAllLines(_path).ToList();
                // Refresh everything the launcher is NOT about to write, so a
                // later Get() reports what is really on disk.
                ParseInto(_lines, _values, _dirty);
            }
            catch (IOException)
            {
                // Unreadable right now (locked): fall back to the snapshot rather
                // than losing the user's launcher settings entirely.
            }
        }

        // StartsWith, not Equals: the header we write carries a trailing comment
        // ("## Launcher (managed by the launcher ...)"), so an exact match never
        // recognised our own output and appended a fresh empty block on every
        // save. Existing configs accumulated one per launcher run.
        if (_wantLauncherSection &&
            !_lines.Any(l => l.Trim().StartsWith("## Launcher", StringComparison.OrdinalIgnoreCase)))
        {
            _lines.Add("");
            _lines.Add("# ===========================================================================");
            _lines.Add("## Launcher (managed by the launcher — the game ignores these keys)");
            _lines.Add("# ===========================================================================");
        }

        // Rewrite only the keys the launcher actually set; every other line is
        // passed through byte-for-byte, comments and ordering included.
        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (int i = 0; i < _lines.Count; i++)
        {
            string key, value;
            if (!TrySplit(_lines[i], out key, out value)) continue;
            seen.Add(key);
            if (_dirty.Contains(key))
                _lines[i] = $"{key} = {_values[key]}";
        }

        // Keys the launcher set that have no line yet (a new option whose
        // config.cfg predates it). Without this they would be dropped.
        foreach (var key in _dirty)
        {
            if (!seen.Contains(key))
                _lines.Add($"{key} = {_values[key]}");
        }

        File.WriteAllLines(_path, _lines);
    }
}
