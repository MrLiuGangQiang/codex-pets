using Avalonia;
using Avalonia.Controls;
using System.Collections.Generic;

namespace CodeXPets.App.Infrastructure;

public interface IPlatformService
{
    string PlatformName { get; }
    string RuntimeArchitecture { get; }
    bool RequiresMousePassThroughPolling { get; }
    PixelPoint GetCursorPosition();
    void ConfigurePetWindow(Window window, Func<Point, bool> isInteractivePoint);
    void UpdatePetWindowClickRegion(Window window, IReadOnlyList<Rect> interactiveRectsDip);
    void UpdatePetWindowMousePassThrough(Window window, bool shouldReceiveMouse);
    bool IsAutoStartEnabled();
    void SetAutoStartEnabled(bool enabled);
    void OpenFolder(string path);
    void OpenUri(Uri uri);
    void ShowDuplicateInstanceMessage();
}

public static class PlatformServiceFactory
{
    public static IPlatformService Create() => OperatingSystem.IsWindows()
        ? new WindowsPlatformService()
        : OperatingSystem.IsMacOS()
            ? new MacPlatformService()
            : throw new PlatformNotSupportedException("CodeXPets currently supports Windows and macOS.");
}
