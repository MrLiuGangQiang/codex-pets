using Avalonia.Platform;
using CodeXPets.Core.Configuration;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace CodeXPets.App.Services;

public enum NotificationSound
{
    Started,
    Completed,
    Error
}

public sealed class SoundService : IDisposable
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private Process? _macPlayer;
    private volatile bool _disposed;

    public void Play(NotificationSound sound)
    {
        if (_disposed) return;
        _ = Task.Run(() => PlayCoreAsync(sound, waitForMacCompletion: false));
    }

    internal bool PlayAndWait(NotificationSound sound)
    {
        if (_disposed) return false;
        return PlayCoreAsync(sound, waitForMacCompletion: true).GetAwaiter().GetResult();
    }

    private async Task<bool> PlayCoreAsync(NotificationSound sound, bool waitForMacCompletion)
    {
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_disposed) return false;
            var path = EnsureExtracted(sound);
            if (OperatingSystem.IsWindows()) return PlayWindows(path);
            if (OperatingSystem.IsMacOS()) return PlayMac(path, waitForMacCompletion);
            return false;
        }
        catch
        {
            // A notification sound must never destabilize the monitor or UI.
            return false;
        }
        finally
        {
            _gate.Release();
        }
    }

    private static string EnsureExtracted(NotificationSound sound)
    {
        var fileName = sound switch
        {
            NotificationSound.Started => "voice-start.mp3",
            NotificationSound.Completed => "voice-complete.mp3",
            _ => "voice-error.mp3"
        };
        var directory = Path.Combine(JsonSettingsStore.GetApplicationDataDirectory(), "audio");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, fileName);
        if (File.Exists(path) && new FileInfo(path).Length > 0) return path;
        using var source = AssetLoader.Open(ResourceCatalog.GetAssetUri("Assets/Audio/" + fileName));
        using var target = File.Create(path);
        source.CopyTo(target);
        return path;
    }

    [SupportedOSPlatform("windows")]
    private static bool PlayWindows(string path)
    {
        var success = false;
        Exception? threadError = null;
        var thread = new Thread(() =>
        {
            try
            {
                const string alias = "codexpets_voice";
                _ = mciSendString($"close {alias}", null, 0, nint.Zero);
                var escaped = path.Replace("\"", "\"\"");
                var opened = mciSendString(
                    $"open \"{escaped}\" type mpegvideo alias {alias}", null, 0, nint.Zero) == 0;
                if (!opened) return;
                try
                {
                    // MCI's MPEG driver is bound to this STA thread. Waiting here keeps the
                    // device alive until the voice clip has actually finished playing.
                    success = mciSendString($"play {alias} wait", null, 0, nint.Zero) == 0;
                }
                finally
                {
                    _ = mciSendString($"close {alias}", null, 0, nint.Zero);
                }
            }
            catch (Exception exception)
            {
                threadError = exception;
            }
        })
        {
            IsBackground = true,
            Name = "CodeXPets Windows audio"
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (threadError is not null)
            throw new InvalidOperationException("Windows notification audio failed.", threadError);
        return success;
    }

    private bool PlayMac(string path, bool waitForCompletion)
    {
        if (_macPlayer is { HasExited: false })
        {
            _macPlayer.Kill(entireProcessTree: true);
            _macPlayer.Dispose();
        }
        var info = new ProcessStartInfo("/usr/bin/afplay") { UseShellExecute = false };
        info.ArgumentList.Add(path);
        var process = Process.Start(info);
        if (process is null) return false;
        _macPlayer = process;
        if (!waitForCompletion) return true;

        process.WaitForExit();
        var success = process.ExitCode == 0;
        process.Dispose();
        if (ReferenceEquals(_macPlayer, process)) _macPlayer = null;
        return success;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _gate.Wait();
        try
        {
            if (_macPlayer is { HasExited: false }) _macPlayer.Kill(entireProcessTree: true);
            _macPlayer?.Dispose();
            _macPlayer = null;
        }
        finally
        {
            _gate.Release();
        }
    }

    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    private static extern int mciSendString(string command, char[]? returnValue,
        int returnLength, nint callback);
}
