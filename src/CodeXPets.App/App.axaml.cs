using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using CodeXPets.App.Services;
using CodeXPets.Core.Configuration;

namespace CodeXPets.App;

public partial class App : Application
{
    private AppController? _controller;

    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.ShutdownMode = Avalonia.Controls.ShutdownMode.OnExplicitShutdown;
            if (HandleUtilityCommand(desktop))
            {
                base.OnFrameworkInitializationCompleted();
                return;
            }
            _controller = new AppController(this, desktop);
            desktop.Exit += (_, _) => _controller?.Dispose();
        }
        base.OnFrameworkInitializationCompleted();
    }

    private static bool HandleUtilityCommand(IClassicDesktopStyleApplicationLifetime desktop)
    {
        var arguments = desktop.Args ?? [];
        var previewIndex = Array.IndexOf(arguments, "--preview");
        if (previewIndex >= 0 && previewIndex + 1 < arguments.Length)
        {
            PreviewRenderer.SaveAll(arguments[previewIndex + 1]);
            Console.WriteLine("CodeXPets previews: " + arguments[previewIndex + 1]);
            Dispatcher.UIThread.Post(() => desktop.Shutdown(0), DispatcherPriority.Background);
            return true;
        }
        var soundIndex = Array.IndexOf(arguments, "--test-sound");
        if (soundIndex >= 0)
        {
            var requested = soundIndex + 1 < arguments.Length
                ? arguments[soundIndex + 1].Trim().ToLowerInvariant()
                : "start";
            var sound = requested switch
            {
                "start" or "started" => NotificationSound.Started,
                "complete" or "completed" => NotificationSound.Completed,
                "error" => NotificationSound.Error,
                _ => (NotificationSound?)null
            };
            var played = false;
            if (sound is { } notification)
            {
                using var service = new SoundService();
                played = service.PlayAndWait(notification);
            }
            Console.WriteLine(sound is null
                ? "CodeXPets sound test: usage --test-sound [start|complete|error]"
                : played ? "CodeXPets sound test: OK" : "CodeXPets sound test: FAILED");
            Dispatcher.UIThread.Post(() => desktop.Shutdown(sound is null ? 2 : played ? 0 : 1),
                DispatcherPriority.Background);
            return true;
        }
        if (arguments.Contains("--validate-resources", StringComparer.Ordinal))
        {
            var valid = ResourceCatalog.ValidateEssentialAssets(out var error);
            Console.WriteLine(valid ? "CodeXPets resources: OK" : "CodeXPets resources: " + error);
            Dispatcher.UIThread.Post(() => desktop.Shutdown(valid ? 0 : 1), DispatcherPriority.Background);
            return true;
        }
        if (arguments.Contains("--smoke-test", StringComparer.Ordinal))
        {
            var valid = ResourceCatalog.ValidateEssentialAssets(out var error);
            if (valid) valid = PreviewRenderer.ValidateAll(out error);
            var settings = new JsonSettingsStore().Load();
            Console.WriteLine(valid ? "CodeXPets smoke test: OK" : "CodeXPets smoke test: " + error);
            Console.WriteLine("sessions=" + settings.SessionsRoot);
            Dispatcher.UIThread.Post(() => desktop.Shutdown(valid ? 0 : 1), DispatcherPriority.Background);
            return true;
        }
        return false;
    }
}

