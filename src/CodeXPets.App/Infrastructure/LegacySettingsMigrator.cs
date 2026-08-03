using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;
using Microsoft.Win32;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.Versioning;
using System.Text;
using System.Xml.Linq;

namespace CodeXPets.App.Infrastructure;

public static class LegacySettingsMigrator
{
    public static AppSettings LoadOrMigrate(ISettingsStore store)
    {
        if (File.Exists(store.SettingsFilePath)) return store.Load();
        var settings = OperatingSystem.IsWindows() ? LoadWindows() :
            OperatingSystem.IsMacOS() ? LoadMac() : AppSettings.CreateDefault();
        settings.Normalize();
        try { store.Save(settings); } catch { }
        return settings;
    }

    [SupportedOSPlatform("windows")]
    private static AppSettings LoadWindows()
    {
        var settings = AppSettings.CreateDefault();
        try
        {
            ApplyWindowsKey(settings, @"Software\CodeXPets");
            ApplyWindowsKey(settings, @"Software\CodeXPets\Windows");
            using var current = Registry.CurrentUser.OpenSubKey(@"Software\CodeXPets\Windows", false)
                ?? Registry.CurrentUser.OpenSubKey(@"Software\CodeXPets", false);
            if (current?.GetValue("PetPositionV1") is string serialized)
                settings.PetPosition = ParseWindowsPosition(serialized);
        }
        catch { }
        return settings;
    }

    [SupportedOSPlatform("windows")]
    private static void ApplyWindowsKey(AppSettings settings, string path)
    {
        using var key = Registry.CurrentUser.OpenSubKey(path, false);
        if (key is null) return;
        settings.DockHoverHeight = ReadInt(key, "DockHoverHeight", settings.DockHoverHeight);
        settings.DockIdleHideSeconds = ReadInt(key, "DockIdleHideSeconds", settings.DockIdleHideSeconds);
        settings.DockRevealSeconds = ReadInt(key, "DockRevealSeconds", settings.DockRevealSeconds);
        settings.DockNotificationSeconds = ReadInt(key, "DockNotificationSeconds", settings.DockNotificationSeconds);
        settings.SoundEnabled = ReadInt(key, "SoundEnabled", settings.SoundEnabled ? 1 : 0) != 0;
        if (key.GetValue("SessionsRoot") is string root && !string.IsNullOrWhiteSpace(root))
            settings.SessionsRoot = root;
    }

    [SupportedOSPlatform("windows")]
    private static int ReadInt(RegistryKey key, string name, int fallback)
    {
        try { return key.GetValue(name) is { } value ? Convert.ToInt32(value, CultureInfo.InvariantCulture) : fallback; }
        catch { return fallback; }
    }

    private static PetPositionState? ParseWindowsPosition(string value)
    {
        try
        {
            var parts = value.Split(';');
            if (parts.Length != 5 || parts[0] != "1") return null;
            var edge = parts[1] switch { "L" => DockEdge.Left, "R" => DockEdge.Right, _ => DockEdge.None };
            var screen = Encoding.UTF8.GetString(Convert.FromBase64String(parts[2]));
            if (!double.TryParse(parts[3], NumberStyles.Float, CultureInfo.InvariantCulture, out var x) ||
                !double.TryParse(parts[4], NumberStyles.Float, CultureInfo.InvariantCulture, out var y)) return null;
            return new PetPositionState(edge, screen, x, y).Normalize();
        }
        catch { return null; }
    }

    [SupportedOSPlatform("macos")]
    private static AppSettings LoadMac()
    {
        var settings = AppSettings.CreateDefault();
        try
        {
            var info = new ProcessStartInfo("/usr/bin/defaults")
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            info.ArgumentList.Add("export");
            info.ArgumentList.Add("com.mrliugangqiang.codexpets");
            info.ArgumentList.Add("-");
            using var process = Process.Start(info);
            if (process is null) return settings;
            var xml = process.StandardOutput.ReadToEnd();
            if (!process.WaitForExit(2000))
            {
                try { process.Kill(entireProcessTree: true); } catch { }
                return settings;
            }
            if (process.ExitCode != 0 || string.IsNullOrWhiteSpace(xml)) return settings;
            var values = ReadPlist(xml);
            settings.DockHoverHeight = ReadPlistInt(values, "DockHoverHeight", settings.DockHoverHeight);
            settings.DockIdleHideSeconds = ReadPlistInt(values, "DockIdleHideSeconds", settings.DockIdleHideSeconds);
            settings.DockRevealSeconds = ReadPlistInt(values, "DockRevealSeconds", settings.DockRevealSeconds);
            settings.DockNotificationSeconds = ReadPlistInt(values, "DockNotificationSeconds", settings.DockNotificationSeconds);
            if (values.TryGetValue("SoundEnabled", out var sound))
                settings.SoundEnabled = sound is bool flag ? flag : settings.SoundEnabled;
            if ((values.TryGetValue("SessionsRoot.macOS", out var root) ||
                 values.TryGetValue("SessionsRoot", out root)) && root is string path)
                settings.SessionsRoot = path;
            if ((values.TryGetValue("PetPositionV1.macOS", out var position) ||
                 values.TryGetValue("PetPositionV1", out position)) && position is byte[] data)
                settings.PetPosition = LegacyPositionSerializer.DeserializeMacOs(data);
        }
        catch { }
        return settings;
    }

    private static Dictionary<string, object?> ReadPlist(string xml)
    {
        var document = XDocument.Parse(xml);
        var dictionary = document.Root?.Element("dict");
        if (dictionary is null) return new Dictionary<string, object?>();
        var nodes = dictionary.Elements().ToArray();
        var result = new Dictionary<string, object?>(StringComparer.Ordinal);
        for (var index = 0; index + 1 < nodes.Length; index++)
        {
            if (nodes[index].Name.LocalName != "key") continue;
            var key = nodes[index].Value;
            var value = nodes[++index];
            result[key] = value.Name.LocalName switch
            {
                "string" => value.Value,
                "integer" when int.TryParse(value.Value, out var integer) => integer,
                "real" when double.TryParse(value.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var real) => real,
                "true" => true,
                "false" => false,
                "data" => Convert.FromBase64String(value.Value),
                _ => null
            };
        }
        return result;
    }

    private static int ReadPlistInt(IReadOnlyDictionary<string, object?> values, string key, int fallback) =>
        values.TryGetValue(key, out var value) && value is int integer ? integer : fallback;
}
