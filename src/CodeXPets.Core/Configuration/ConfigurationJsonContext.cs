using System.Text.Json.Serialization;

namespace CodeXPets.Core.Configuration;

[JsonSourceGenerationOptions(
    WriteIndented = true,
    PropertyNameCaseInsensitive = true,
    UseStringEnumConverter = true)]
[JsonSerializable(typeof(AppSettings))]
[JsonSerializable(typeof(LegacyPositionSerializer.LegacyMacPosition))]
internal partial class ConfigurationJsonContext : JsonSerializerContext;
