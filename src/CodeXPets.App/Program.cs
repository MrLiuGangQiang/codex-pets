using Avalonia;
using CodeXPets.App.Infrastructure;
using System.Reflection;

namespace CodeXPets.App;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--version", StringComparer.Ordinal))
        {
            Console.WriteLine(Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "鏈煡");
            return 0;
        }

        var previewIndex = Array.IndexOf(args, "--preview");
        if (previewIndex >= 0 &&
            (previewIndex + 1 >= args.Length || args[previewIndex + 1].StartsWith("--", StringComparison.Ordinal)))
        {
            Console.Error.WriteLine("鐢ㄦ硶锛欳odeXPets --preview <杈撳嚭鐩綍>");
            return 2;
        }

        if (IsUtilityCommand(args))
        {
            return StartApplication(args);
        }

        using var instance = SingleInstanceLease.TryAcquire();
        if (!instance.Acquired)
        {
            try
            {
                PlatformServiceFactory.Create().ShowDuplicateInstanceMessage();
            }
            catch
            {
                Console.Error.WriteLine("CodeXPets already running.");
            }
            return 0;
        }

        return StartApplication(args);
    }

    private static bool IsUtilityCommand(IReadOnlyCollection<string> args) =>
        args.Contains("--preview", StringComparer.Ordinal) ||
        args.Contains("--validate-resources", StringComparer.Ordinal) ||
        args.Contains("--smoke-test", StringComparer.Ordinal) ||
        args.Contains("--test-sound", StringComparer.Ordinal);

    private static int StartApplication(string[] args)
    {
        try
        {
            return BuildAvaloniaApp().StartWithClassicDesktopLifetime(
                args,
                Avalonia.Controls.ShutdownMode.OnExplicitShutdown);
        }
        catch (Exception exception)
        {
            TryWriteStartupError(exception);
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static void TryWriteStartupError(Exception exception)
    {
        try
        {
            var path = Path.Combine(
                CodeXPets.Core.Configuration.JsonSettingsStore.GetApplicationDataDirectory(),
                "startup-error.txt");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, exception.ToString());
        }
        catch
        {
            // Startup diagnostics must never hide the original failure.
        }
    }

    public static AppBuilder BuildAvaloniaApp()
    {
        var builder = AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .LogToTrace();

        if (OperatingSystem.IsMacOS())
        {
            builder = builder.With(new MacOSPlatformOptions
            {
                ShowInDock = false,
                DisableDefaultApplicationMenuItems = true
            });
        }

#if DEBUG
#endif
        return builder;
    }
}


