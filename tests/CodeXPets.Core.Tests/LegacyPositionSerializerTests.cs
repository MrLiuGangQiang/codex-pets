using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;
using System.Text;

namespace CodeXPets.Core.Tests;

public sealed class LegacyPositionSerializerTests
{
    [Fact]
    public void MacOsPositionUsesSwiftNamesAndConvertsBottomOriginY()
    {
        const string json = """
            {"dockEdge":"right","screenIdentifier":"Studio Display","relativeX":1,"relativeY":0.25}
            """;

        var position = LegacyPositionSerializer.DeserializeMacOs(Encoding.UTF8.GetBytes(json));

        Assert.NotNull(position);
        Assert.Equal(DockEdge.Right, position.DockEdge);
        Assert.Equal("Studio Display", position.ScreenIdentifier);
        Assert.Equal(1d, position.RelativeX);
        Assert.Equal(0.75d, position.RelativeY);
    }

    [Fact]
    public void MacOsPositionIsNormalizedAfterCoordinateConversion()
    {
        const string json = """
            {"DockEdge":"left","ScreenIdentifier":"Built-in","RelativeX":-2,"RelativeY":3}
            """;

        var position = LegacyPositionSerializer.DeserializeMacOs(Encoding.UTF8.GetBytes(json));

        Assert.NotNull(position);
        Assert.Equal(DockEdge.Left, position.DockEdge);
        Assert.Equal(0d, position.RelativeX);
        Assert.Equal(0d, position.RelativeY);
    }

    [Theory]
    [InlineData("{ invalid json")]
    [InlineData("{}")]
    [InlineData("{\"dockEdge\":\"unknown\",\"screenIdentifier\":\"Display\",\"relativeX\":0.5,\"relativeY\":0.5}")]
    public void InvalidMacOsPositionReturnsNull(string json)
    {
        Assert.Null(LegacyPositionSerializer.DeserializeMacOs(Encoding.UTF8.GetBytes(json)));
    }
}
