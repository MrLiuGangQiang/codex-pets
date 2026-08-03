using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;

namespace CodeXPets.Core.Tests;

public sealed class SettingsStoreTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(),
        "CodeXPetsSettingsTests_" + Guid.NewGuid().ToString("N"));

    [Fact]
    public void JsonSettingsRoundTripPreservesCrossPlatformState()
    {
        var path = Path.Combine(_root, "settings.json");
        var store = new JsonSettingsStore(path);
        var settings = new AppSettings
        {
            DockHoverHeight = 320,
            DockIdleHideSeconds = 14,
            DockRevealSeconds = 4,
            DockNotificationSeconds = 8,
            SoundEnabled = false,
            PetVisible = false,
            SessionsRoot = Path.Combine(_root, "sessions"),
            PetPosition = new PetPositionState(DockEdge.Right, "Display A|0,0,1920,1080", 1, 0.42)
        };

        store.Save(settings);
        var loaded = store.Load();

        Assert.Equal(320, loaded.DockHoverHeight);
        Assert.Equal(14, loaded.DockIdleHideSeconds);
        Assert.Equal(4, loaded.DockRevealSeconds);
        Assert.Equal(8, loaded.DockNotificationSeconds);
        Assert.False(loaded.SoundEnabled);
        Assert.False(loaded.PetVisible);
        Assert.Equal(Path.GetFullPath(settings.SessionsRoot), loaded.SessionsRoot);
        Assert.Equal(settings.PetPosition, loaded.PetPosition);
        Assert.False(File.Exists(path + ".tmp"));
    }

    [Fact]
    public void InvalidJsonFallsBackToSafeDefaults()
    {
        Directory.CreateDirectory(_root);
        var path = Path.Combine(_root, "settings.json");
        File.WriteAllText(path, "{ invalid json");
        var loaded = new JsonSettingsStore(path).Load();
        Assert.True(loaded.PetVisible);
        Assert.True(loaded.SoundEnabled);
        Assert.Equal(CodexPaths.GetDefaultSessionsRoot(), loaded.SessionsRoot);
    }

    public void Dispose()
    {
        if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true);
    }
}

