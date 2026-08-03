using CodeXPets.Core.Domain;
using System.Text.Json;

namespace CodeXPets.Core.Configuration;

public sealed class AppSettings
{
    public int DockHoverHeight { get; set; } = 240;
    public int DockIdleHideSeconds { get; set; } = 10;
    public int DockRevealSeconds { get; set; } = 3;
    public int DockNotificationSeconds { get; set; } = 5;
    public bool SoundEnabled { get; set; } = true;
    public bool PetVisible { get; set; } = true;
    public string SessionsRoot { get; set; } = CodexPaths.GetDefaultSessionsRoot();
    public PetPositionState? PetPosition { get; set; }

    public static AppSettings CreateDefault() => new();

    public AppSettings Clone() => (AppSettings)MemberwiseClone();

    public void CopyFrom(AppSettings other)
    {
        ArgumentNullException.ThrowIfNull(other);
        DockHoverHeight = other.DockHoverHeight;
        DockIdleHideSeconds = other.DockIdleHideSeconds;
        DockRevealSeconds = other.DockRevealSeconds;
        DockNotificationSeconds = other.DockNotificationSeconds;
        SoundEnabled = other.SoundEnabled;
        PetVisible = other.PetVisible;
        SessionsRoot = other.SessionsRoot;
        PetPosition = other.PetPosition;
        Normalize();
    }

    public void Normalize()
    {
        DockHoverHeight = Math.Clamp(DockHoverHeight, 40, 1000);
        DockIdleHideSeconds = Math.Clamp(DockIdleHideSeconds, 0, 3600);
        DockRevealSeconds = Math.Clamp(DockRevealSeconds, 1, 60);
        DockNotificationSeconds = Math.Clamp(DockNotificationSeconds, 1, 120);
        SessionsRoot = CodexPaths.NormalizeSessionsRoot(SessionsRoot);
        PetPosition = PetPosition?.Normalize();
    }
}

public interface ISettingsStore
{
    string SettingsFilePath { get; }
    AppSettings Load();
    void Save(AppSettings settings);
}

public sealed class JsonSettingsStore : ISettingsStore
{
    public JsonSettingsStore(string? settingsFilePath = null)
    {
        SettingsFilePath = string.IsNullOrWhiteSpace(settingsFilePath)
            ? Path.Combine(GetApplicationDataDirectory(), "settings.json")
            : Path.GetFullPath(settingsFilePath);
    }

    public string SettingsFilePath { get; }

    public AppSettings Load()
    {
        try
        {
            if (!File.Exists(SettingsFilePath))
            {
                return AppSettings.CreateDefault();
            }

            var result = JsonSerializer.Deserialize(File.ReadAllText(SettingsFilePath),
                ConfigurationJsonContext.Default.AppSettings) ?? AppSettings.CreateDefault();
            result.Normalize();
            return result;
        }
        catch
        {
            return AppSettings.CreateDefault();
        }
    }

    public void Save(AppSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        settings.Normalize();
        var directory = Path.GetDirectoryName(SettingsFilePath);
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var temporaryPath = SettingsFilePath + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, ConfigurationJsonContext.Default.AppSettings));
        File.Move(temporaryPath, SettingsFilePath, overwrite: true);
    }

    public static string GetApplicationDataDirectory()
    {
        if (OperatingSystem.IsMacOS())
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                "Library", "Application Support", "CodeXPets");
        }

        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(local))
        {
            local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".codexpets");
        }

        return Path.Combine(local, "CodeXPets");
    }
}

