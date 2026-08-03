using CodeXPets.Core.Domain;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CodeXPets.Core.Configuration;

internal static class LegacyPositionSerializer
{
    public static PetPositionState? DeserializeMacOs(ReadOnlySpan<byte> utf8Json)
    {
        try
        {
            var legacy = JsonSerializer.Deserialize(utf8Json, ConfigurationJsonContext.Default.LegacyMacPosition);
            if (legacy?.DockEdge is null ||
                legacy.ScreenIdentifier is null ||
                legacy.RelativeX is null ||
                legacy.RelativeY is null ||
                !Enum.IsDefined(legacy.DockEdge.Value) ||
                !double.IsFinite(legacy.RelativeX.Value) ||
                !double.IsFinite(legacy.RelativeY.Value))
            {
                return null;
            }

            // AppKit stores positions from the bottom of the working area. Avalonia uses a
            // top-origin desktop coordinate system, so legacy macOS Y values must be mirrored.
            return new PetPositionState(
                legacy.DockEdge.Value,
                legacy.ScreenIdentifier,
                legacy.RelativeX.Value,
                1d - legacy.RelativeY.Value).Normalize();
        }
        catch (JsonException)
        {
            return null;
        }
        catch (NotSupportedException)
        {
            return null;
        }
    }

    internal sealed class LegacyMacPosition
    {
        public DockEdge? DockEdge { get; init; }
        public string? ScreenIdentifier { get; init; }
        public double? RelativeX { get; init; }
        public double? RelativeY { get; init; }
    }
}
