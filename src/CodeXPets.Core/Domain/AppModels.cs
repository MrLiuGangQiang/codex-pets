namespace CodeXPets.Core.Domain;

public enum ReminderState
{
    Idle,
    Busy,
    Completed,
    Error
}

public enum DockEdge
{
    None,
    Left,
    Right
}

public sealed record PetPositionState(
    DockEdge DockEdge,
    string ScreenIdentifier,
    double RelativeX,
    double RelativeY)
{
    public PetPositionState Normalize() => this with
    {
        ScreenIdentifier = ScreenIdentifier ?? string.Empty,
        RelativeX = Math.Clamp(RelativeX, 0d, 1d),
        RelativeY = Math.Clamp(RelativeY, 0d, 1d)
    };
}
