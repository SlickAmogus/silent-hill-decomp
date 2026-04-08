using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public class ConfigManager
{
    private readonly string _path;
    private Dictionary<string, string> _values = new Dictionary<string, string>();
    private List<string> _lines = new List<string>();

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
    }

    public void Load()
    {
        _lines = File.ReadAllLines(_path).ToList();
        _values.Clear();

        foreach (var line in _lines)
        {
            if (line.TrimStart().StartsWith("#")) continue;
            if (!line.Contains("=")) continue;

            var parts = line.Split('=');
            if (parts.Length == 2)
            {
                var key = parts[0].Trim();
                var val = parts[1].Trim();
                _values[key] = val;
            }
        }
    }

    public void Save()
    {
        for (int i = 0; i < _lines.Count; i++)
        {
            var line = _lines[i];
            if (line.TrimStart().StartsWith("#") || !line.Contains("="))
                continue;

            var key = line.Split('=')[0].Trim();
            if (_values.ContainsKey(key))
                _lines[i] = $"{key} = {_values[key]}";
        }

        File.WriteAllLines(_path, _lines);
    }
}
