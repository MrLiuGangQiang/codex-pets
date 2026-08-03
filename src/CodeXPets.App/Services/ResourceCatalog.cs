using Avalonia;

using Avalonia.Controls;

using Avalonia.Media.Imaging;

using Avalonia.Platform;

using SkiaSharp;
using System.Runtime.InteropServices;

namespace CodeXPets.App.Services;

internal readonly record struct SpriteFrameMetric(

    double AnchorX,

    double Bottom,

    Rect OpaqueBounds,

    bool HasPixels)

{

    public static SpriteFrameMetric Default { get; } = new(

        ResourceCatalog.SpriteCellWidth / 2d,

        ResourceCatalog.SpriteCellHeight,

        new Rect(0, 0, ResourceCatalog.SpriteCellWidth, ResourceCatalog.SpriteCellHeight),

        false);

}

public sealed class ResourceCatalog : IDisposable

{

    public const int SpriteCellWidth = 192;

    public const int SpriteCellHeight = 208;

    public const int DockCellSize = 256;

    private const int SpriteColumns = 8;

    private const int SpriteRows = 4;

    private const int DockFrameCount = 8;

    public ResourceCatalog()

    {

        CatSpriteSheet = LoadBitmap("Assets/Images/white-british-shorthair-spritesheet.png");

        DockFrames = LoadBitmapFrames("Assets/Images/british-shorthair-dock-spritesheet.png",
            DockCellSize, DockCellSize, DockFrameCount);

        _catAlphaMask = LoadAlphaMask("Assets/Images/white-british-shorthair-spritesheet.png", out _catMaskWidth, out _);
        _dockAlphaMask = LoadAlphaMask("Assets/Images/british-shorthair-dock-spritesheet.png", out _dockMaskWidth, out _);
        CloudBubble = LoadCloudResources(out _cloudAlphaMask, out _cloudMaskWidth, out _cloudMaskHeight, out var cloudBounds);
        CloudOpaqueBounds = cloudBounds;
        _catRegionRects = new List<Rect>[SpriteRows, SpriteColumns];
        for (var row = 0; row < SpriteRows; row++)
        {
            for (var column = 0; column < SpriteColumns; column++)
            {
                _catRegionRects[row, column] = ComputeOpaqueRuns(_catAlphaMask, _catMaskWidth, SpriteCellHeight * 4,
                    column * SpriteCellWidth, row * SpriteCellHeight, SpriteCellWidth, SpriteCellHeight);
            }
        }
        _dockRegionRects = new List<Rect>[DockFrameCount];
        for (var i = 0; i < DockFrameCount; i++)
        {
            _dockRegionRects[i] = ComputeOpaqueRuns(_dockAlphaMask, _dockMaskWidth, DockCellSize,
                i * DockCellSize, 0, DockCellSize, DockCellSize);
        }
        _cloudRegionRects = ComputeOpaqueRuns(_cloudAlphaMask, _cloudMaskWidth, _cloudMaskHeight,
            0, 0, _cloudMaskWidth, _cloudMaskHeight);
        SpriteMetrics = AnalyzeSpriteFrames("Assets/Images/white-british-shorthair-spritesheet.png");

        DockOpaqueBounds = AnalyzeDockFrames("Assets/Images/british-shorthair-dock-spritesheet.png");

        IdleIcon = LoadIcon("Assets/Icons/status-idle.png");

        CompletedIcon = LoadIcon("Assets/Icons/status-completed.png");

        ErrorIcon = LoadIcon("Assets/Icons/status-error.png");

        BusyIcons = Enumerable.Range(0, 8)

            .Select(frame => LoadIcon($"Assets/Icons/status-busy-{frame}.png"))

            .ToArray();

        // Free one-time startup buffers before the app begins animating.
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Optimized, false);
    }
    public Bitmap CatSpriteSheet { get; }


    internal IReadOnlyList<Bitmap> DockFrames { get; }

    public Bitmap CloudBubble { get; }

    public WindowIcon IdleIcon { get; }

    public WindowIcon CompletedIcon { get; }

    public WindowIcon ErrorIcon { get; }

    public IReadOnlyList<WindowIcon> BusyIcons { get; }

    internal SpriteFrameMetric[,] SpriteMetrics { get; }

    internal Rect[] DockOpaqueBounds { get; }

    internal Rect CloudOpaqueBounds { get; }
    private readonly byte[] _catAlphaMask;
    private readonly byte[] _dockAlphaMask;
    private readonly byte[] _cloudAlphaMask;
    private readonly int _catMaskWidth;
    private readonly int _dockMaskWidth;
    private readonly int _cloudMaskWidth;
    private readonly int _cloudMaskHeight;
    private readonly List<Rect>[,] _catRegionRects;
    private readonly List<Rect>[] _dockRegionRects;
    private readonly List<Rect> _cloudRegionRects;

    internal bool IsCloudOpaque(int x, int y) => IsOpaque(_cloudAlphaMask, _cloudMaskWidth, _cloudMaskHeight, x, y);

    internal bool IsCatOpaque(int row, int frame, int x, int y, bool mirrored)
    {
        if (mirrored) x = SpriteCellWidth - 1 - x;
        return IsOpaque(_catAlphaMask, _catMaskWidth, SpriteCellHeight * 4,
            frame * SpriteCellWidth + x, row * SpriteCellHeight + y);
    }

    internal bool IsDockPetOpaque(int frame, int x, int y) =>
        IsOpaque(_dockAlphaMask, _dockMaskWidth, DockCellSize, frame * DockCellSize + x, y);

    internal IReadOnlyList<Rect> GetCatRegionRects(int row, int frame) =>
        _catRegionRects[row, frame];

    internal IReadOnlyList<Rect> GetDockRegionRects(int frame) =>
        _dockRegionRects[frame];

    internal IReadOnlyList<Rect> GetCloudRegionRects() => _cloudRegionRects;

    private static List<Rect> ComputeOpaqueRuns(byte[] alpha, int width, int height,
        int x0, int y0, int cellWidth, int cellHeight)
    {
        var rects = new List<Rect>();
        var x1 = x0 + cellWidth;
        var y1 = y0 + cellHeight;
        for (var y = y0; y < y1; y++)
        {
            var runStart = -1;
            for (var x = x0; x <= x1; x++)
            {
                var opaque = x < x1 && alpha[y * width + x] > 16;
                if (opaque)
                {
                    if (runStart < 0) runStart = x;
                }
                else if (runStart >= 0)
                {
                    rects.Add(new Rect(runStart - x0, y - y0, x - runStart, 1));
                    runStart = -1;
                }
            }
        }
        return rects;
    }

    private static byte[] LoadAlphaMask(string relativePath, out int width, out int height)
    {
        using var stream = OpenAsset(relativePath);
        using var bitmap = SKBitmap.Decode(stream) ?? throw new InvalidDataException($"无法读取资源：{relativePath}");
        width = bitmap.Width;
        height = bitmap.Height;
        var alpha = new byte[width * height];
        for (var y = 0; y < height; y++)
            for (var x = 0; x < width; x++)
                alpha[y * width + x] = bitmap.GetPixel(x, y).Alpha;
        return alpha;
    }

    private static bool IsOpaque(byte[] mask, int width, int height, int x, int y)
    {
        return x >= 0 && y >= 0 && x < width && y < height && mask[y * width + x] > 16;
    }

    public static Uri GetAssetUri(string relativePath) =>

        new($"avares://CodeXPets/{relativePath.Replace('\\', '/')}");

    public static Stream OpenAsset(string relativePath) => AssetLoader.Open(GetAssetUri(relativePath));

    public static bool ValidateEssentialAssets(out string error) =>

        AssetValidator.Validate(OpenAsset, out error);

    private static Rect AnalyzeOpaqueBounds(string relativePath, Rect fallback)
    {
        try
        {
            using var stream = OpenAsset(relativePath);
            using var bitmap = SKBitmap.Decode(stream);
            if (bitmap is null) return fallback;

            var minX = bitmap.Width;
            var minY = bitmap.Height;
            var maxX = -1;
            var maxY = -1;
            for (var y = 0; y < bitmap.Height; y++)
            {
                for (var x = 0; x < bitmap.Width; x++)
                {
                    if (bitmap.GetPixel(x, y).Alpha <= 16) continue;
                    minX = Math.Min(minX, x);
                    minY = Math.Min(minY, y);
                    maxX = Math.Max(maxX, x);
                    maxY = Math.Max(maxY, y);
                }
            }

            if (maxX >= minX)
                return new Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
        }
        catch
        {
            // Full-image bounds are a safe fallback if asset analysis fails.
        }

        return fallback;
    }

    private static SpriteFrameMetric[,] AnalyzeSpriteFrames(string relativePath)

    {

        var result = new SpriteFrameMetric[SpriteRows, SpriteColumns];

        for (var row = 0; row < SpriteRows; row++)

        {

            for (var column = 0; column < SpriteColumns; column++)

            {

                result[row, column] = SpriteFrameMetric.Default;

            }

        }

        try

        {

            using var stream = OpenAsset(relativePath);

            using var bitmap = SKBitmap.Decode(stream);

            if (bitmap is null) return result;

            var headZoneHeight = Math.Max(1, (int)Math.Round(SpriteCellHeight * 0.32));

            for (var row = 0; row < SpriteRows; row++)

            {

                for (var column = 0; column < SpriteColumns; column++)

                {

                    var minX = SpriteCellWidth;

                    var minY = SpriteCellHeight;

                    var maxX = -1;

                    var maxY = -1;

                    long allX = 0;

                    long headX = 0;

                    var allCount = 0;

                    var headCount = 0;

                    for (var y = 0; y < SpriteCellHeight; y++)

                    {

                        var sheetY = row * SpriteCellHeight + y;

                        if (sheetY >= bitmap.Height) break;

                        for (var x = 0; x < SpriteCellWidth; x++)

                        {

                            var sheetX = column * SpriteCellWidth + x;

                            if (sheetX >= bitmap.Width || bitmap.GetPixel(sheetX, sheetY).Alpha < 20)

                                continue;

                            minX = Math.Min(minX, x);

                            minY = Math.Min(minY, y);

                            maxX = Math.Max(maxX, x);

                            maxY = Math.Max(maxY, y);

                            allX += x;

                            allCount++;

                            if (y >= headZoneHeight) continue;

                            headX += x;

                            headCount++;

                        }

                    }

                    if (allCount == 0) continue;

                    var anchorX = headCount > 0 ? headX / (double)headCount : allX / (double)allCount;

                    result[row, column] = new SpriteFrameMetric(anchorX, maxY + 1,

                        new Rect(minX, minY, maxX - minX + 1, maxY - minY + 1), true);

                }

            }

        }

        catch

        {

            // Default cell-centred metrics still render safely if analysis fails.

        }

        return result;

    }

    private static Rect[] AnalyzeDockFrames(string relativePath)

    {

        var result = Enumerable.Repeat(new Rect(0, 0, DockCellSize, DockCellSize), DockFrameCount)

            .ToArray();

        try

        {

            using var stream = OpenAsset(relativePath);

            using var bitmap = SKBitmap.Decode(stream);

            if (bitmap is null) return result;

            for (var index = 0; index < DockFrameCount; index++)

            {

                var cellLeft = index * DockCellSize;

                var minX = DockCellSize;

                var minY = DockCellSize;

                var maxX = -1;

                var maxY = -1;

                for (var y = 0; y < DockCellSize && y < bitmap.Height; y++)

                {

                    for (var x = 0; x < DockCellSize && cellLeft + x < bitmap.Width; x++)

                    {

                        if (bitmap.GetPixel(cellLeft + x, y).Alpha <= 16) continue;

                        minX = Math.Min(minX, x);

                        minY = Math.Min(minY, y);

                        maxX = Math.Max(maxX, x);

                        maxY = Math.Max(maxY, y);

                    }

                }

                if (maxX >= minX)

                    result[index] = new Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);

            }

        }

        catch

        {

            // Full-cell bounds are a safe fallback.

        }

        return result;

    }

    private static Bitmap LoadCloudResources(out byte[] alphaMask, out int width, out int height, out Rect opaqueBounds)
    {
        const int MaxWidth = 640;
        using var stream = OpenAsset("Assets/Images/cloud-bubble.png");
        using var original = SKBitmap.Decode(stream) ??
            throw new InvalidDataException("Unable to decode cloud bubble.");
        var scale = Math.Min(1.0, MaxWidth / (double)original.Width);
        var newWidth = Math.Max(1, (int)Math.Round(original.Width * scale));
        var newHeight = Math.Max(1, (int)Math.Round(original.Height * scale));
        using var downscaled = new SKBitmap(newWidth, newHeight, SKColorType.Bgra8888, SKAlphaType.Premul);
        using (var canvas = new SKCanvas(downscaled))
        {
            canvas.Clear(SKColors.Transparent);
            canvas.DrawBitmap(original, new SKRect(0, 0, original.Width, original.Height),
                new SKRect(0, 0, newWidth, newHeight));
        }

        width = newWidth;
        height = newHeight;
        alphaMask = new byte[newWidth * newHeight];
        var minX = newWidth;
        var minY = newHeight;
        var maxX = -1;
        var maxY = -1;
        for (var y = 0; y < newHeight; y++)
        {
            for (var x = 0; x < newWidth; x++)
            {
                var alpha = downscaled.GetPixel(x, y).Alpha;
                alphaMask[y * newWidth + x] = alpha;
                if (alpha > 16)
                {
                    minX = Math.Min(minX, x);
                    minY = Math.Min(minY, y);
                    maxX = Math.Max(maxX, x);
                    maxY = Math.Max(maxY, y);
                }
            }
        }

        opaqueBounds = maxX >= minX
            ? new Rect(minX, minY, maxX - minX + 1, maxY - minY + 1)
            : new Rect(0, 0, newWidth, newHeight);

        using var image = SKImage.FromBitmap(downscaled);
        using var data = image.Encode(SKEncodedImageFormat.Png, 90);
        using var encoded = new MemoryStream(data.ToArray(), writable: false);
        return new Bitmap(encoded);
    }

    private static Bitmap LoadBitmap(string relativePath)

    {

        using var stream = OpenAsset(relativePath);

        return new Bitmap(stream);

    }

    private static Bitmap[] LoadBitmapFrames(string relativePath, int frameWidth,
        int frameHeight, int frameCount)
    {
        using var stream = OpenAsset(relativePath);
        using var sheet = SKBitmap.Decode(stream) ??
            throw new InvalidDataException("Unable to decode dock sprite sheet.");
        if (sheet.Width < frameWidth * frameCount || sheet.Height < frameHeight)
            throw new InvalidDataException("Dock sprite sheet dimensions are invalid.");

        var result = new List<Bitmap>(frameCount);
        try
        {
            for (var index = 0; index < frameCount; index++)
            {
                using var frame = new SKBitmap(frameWidth, frameHeight,
                    SKColorType.Bgra8888, SKAlphaType.Premul);
                using (var canvas = new SKCanvas(frame))
                {
                    canvas.Clear(SKColors.Transparent);
                    canvas.DrawBitmap(sheet,
                        new SKRect(index * frameWidth, 0, (index + 1) * frameWidth, frameHeight),
                        new SKRect(0, 0, frameWidth, frameHeight));
                    canvas.Flush();
                }

                using var image = SKImage.FromBitmap(frame);
                using var data = image.Encode(SKEncodedImageFormat.Png, 100);
                using var encoded = new MemoryStream(data.ToArray(), writable: false);
                result.Add(new Bitmap(encoded));
            }

            return result.ToArray();
        }
        catch
        {
            foreach (var bitmap in result) bitmap.Dispose();
            throw;
        }
    }

    private static WindowIcon LoadIcon(string relativePath)

    {

        using var stream = OpenAsset(relativePath);

        return new WindowIcon(stream);

    }

    public void Dispose()

    {

        CatSpriteSheet.Dispose();


        foreach (var frame in DockFrames) frame.Dispose();

        CloudBubble.Dispose();

    }

}

