#nullable disable
using System.Text.Json;

namespace CodeXPets.Core.Infrastructure;

/// <summary>
/// Minimal compatibility adapter used by the proven legacy session parser while the public core
/// remains based on System.Text.Json. It intentionally returns dictionaries and object arrays in
/// the same shape as the .NET Framework JavaScriptSerializer.
/// </summary>
internal sealed class LegacyJsonSerializer
{
    public object DeserializeObject(string input)
    {
        try
        {
            using var document = JsonDocument.Parse(input);
            return ConvertElement(document.RootElement);
        }
        catch (JsonException exception)
        {
            throw new ArgumentException("Invalid JSON.", exception);
        }
    }

    private static object ConvertElement(JsonElement element) => element.ValueKind switch
    {
        JsonValueKind.Object => element.EnumerateObject().ToDictionary(
            property => property.Name,
            property => ConvertElement(property.Value),
            StringComparer.Ordinal),
        JsonValueKind.Array => element.EnumerateArray().Select(ConvertElement).ToArray(),
        JsonValueKind.String => element.GetString(),
        JsonValueKind.Number when element.TryGetInt64(out var integer) => integer,
        JsonValueKind.Number => element.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.Null or JsonValueKind.Undefined => null,
        _ => element.GetRawText()
    };
}
