using CodeXPets.Core.Configuration;

namespace CodeXPets.Core.Tests;

public sealed class CodexPathsTests
{
    [Fact]
    public void DefaultsAlwaysUseCodexHomeAndSessions()
    {
        var before = Environment.GetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable);
        try
        {
            Environment.SetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable, null);
            Assert.EndsWith(".codex", CodexPaths.GetDefaultHome(), StringComparison.OrdinalIgnoreCase);
            Assert.Equal(Path.Combine(CodexPaths.GetDefaultHome(), "sessions"),
                CodexPaths.GetDefaultSessionsRoot());
            Assert.Equal(Path.Combine(CodexPaths.GetDefaultHome(), "config.toml"),
                CodexPaths.GetDefaultConfigFile());
        }
        finally
        {
            Environment.SetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable, before);
        }
    }

    [Fact]
    public void ForeignPlatformPathsAreRejected()
    {
        if (OperatingSystem.IsWindows())
        {
            Assert.Equal(CodexPaths.GetDefaultSessionsRoot(),
                CodexPaths.NormalizeSessionsRoot("~/Library/Application Support/Codex"));
        }
        else
        {
            Assert.Equal(CodexPaths.GetDefaultSessionsRoot(),
                CodexPaths.NormalizeSessionsRoot(@"C:\\Users\\Someone\\.codex\\sessions"));
        }
    }

    [Fact]
    public void CodeXHomeOverridesTheDefault()
    {
        var before = Environment.GetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable);
        var root = Path.Combine(Path.GetTempPath(), "codexpets-home-" + Guid.NewGuid().ToString("N"));
        try
        {
            Environment.SetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable, root);
            Assert.Equal(Path.GetFullPath(root), CodexPaths.GetDefaultHome());
            Assert.Equal(Path.Combine(Path.GetFullPath(root), "sessions"), CodexPaths.GetDefaultSessionsRoot());
        }
        finally
        {
            Environment.SetEnvironmentVariable(CodexPaths.HomeEnvironmentVariable, before);
        }
    }
}
