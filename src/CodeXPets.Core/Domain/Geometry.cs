namespace CodeXPets.Core.Domain;

public readonly record struct PointD(double X, double Y);

public readonly record struct RectD(double X, double Y, double Width, double Height)
{
    public double Left => X;
    public double Top => Y;
    public double Right => X + Width;
    public double Bottom => Y + Height;
    public double CenterX => X + Width / 2d;

    public bool Contains(PointD point) => point.X >= Left && point.X <= Right &&
                                          point.Y >= Top && point.Y <= Bottom;
}
