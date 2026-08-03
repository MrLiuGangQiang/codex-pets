
namespace CodeXPets.Core.Configuration;

/// <summary>Resolves Codex data locations without leaking one operating system's path syntax into another.</summary>
public static class CodexPaths
{
    public const string HomeEnvironmentVariable = "CODEX_HOME";

    public static string GetDefaultHome()
    {
        var configured = Environment.GetEnvironmentVariable(HomeEnvironmentVariable);
        if (!string.IsNullOrWhiteSpace(configured))
        {
            return NormalizeHome(configured);
        }

        return Path.Combine(GetUserHome(), ".codex");
    }

    public static string GetDefaultSessionsRoot() => Path.Combine(GetDefaultHome(), "sessions");

    public static string GetDefaultConfigFile() => Path.Combine(GetDefaultHome(), "config.toml");

    public static string NormalizeSessionsRoot(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return GetDefaultSessionsRoot();
        }

        if (LooksLikeForeignPlatformPath(path))
        {
            return GetDefaultSessionsRoot();
        }

        try
        {
            var expanded = ExpandHomeAndEnvironment(path.Trim());
            return Path.GetFullPath(expanded);
        }
        catch
        {
            return GetDefaultSessionsRoot();
        }
    }

    private static bool LooksLikeForeignPlatformPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return false;
        }

        var value = path.Trim();
        if (OperatingSystem.IsWindows())
        {
            return value.StartsWith("~/", StringComparison.Ordinal) ||
                   value.StartsWith("/", StringComparison.Ordinal) ||
                   value.Contains("$HOME", StringComparison.OrdinalIgnoreCase) ||
                   value.StartsWith("file:///", StringComparison.OrdinalIgnoreCase);
        }

        if (OperatingSystem.IsMacOS() || OperatingSystem.IsLinux())
        {
            return value.Length >= 3 && char.IsLetter(value[0]) && value[1] == ':' &&
                   (value[2] == '\\' || value[2] == '/');
        }

        return false;
    }

    private static string NormalizeHome(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || LooksLikeForeignPlatformPath(path))
        {
            return Path.Combine(GetUserHome(), ".codex");
        }

        try
        {
            return Path.GetFullPath(ExpandHomeAndEnvironment(path.Trim()));
        }
        catch
        {
            return Path.Combine(GetUserHome(), ".codex");
        }
    }

    private static string ExpandHomeAndEnvironment(string value)
    {
        var expanded = Environment.ExpandEnvironmentVariables(value);
        var home = GetUserHome();
        if (expanded.Equals("~", StringComparison.Ordinal))
        {
            return home;
        }

        if (expanded.StartsWith("~/", StringComparison.Ordinal) ||
            expanded.StartsWith("~\\", StringComparison.Ordinal))
        {
            return Path.Combine(home, expanded[2..]);
        }

        if (!OperatingSystem.IsWindows())
        {
            expanded = expanded.Replace("$HOME", home, StringComparison.OrdinalIgnoreCase)
                               .Replace("${HOME}", home, StringComparison.OrdinalIgnoreCase);
        }

        return expanded;
    }

    private static string GetUserHome()
    {
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (!string.IsNullOrWhiteSpace(profile))
        {
            return profile;
        }

        return Environment.GetEnvironmentVariable(OperatingSystem.IsWindows() ? "USERPROFILE" : "HOME")
               ?? AppContext.BaseDirectory;
    }
}
