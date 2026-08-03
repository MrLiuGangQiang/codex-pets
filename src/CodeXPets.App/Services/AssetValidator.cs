using SkiaSharp;

namespace CodeXPets.App.Services;

internal static class AssetValidator
{
    private static readonly ImageAssetSpec[] ImageAssets =
    [
        new("Assets/Images/white-british-shorthair-spritesheet.png", 1536, 832),
        new("Assets/Images/british-shorthair-dock-spritesheet.png", 2048, 256),
        new("Assets/Images/cloud-bubble.png", 2122, 734),
        new("Assets/Icons/AppIcon.png", 1024, 1024),
        new("Assets/Icons/status-idle.png", 64, 64),
        new("Assets/Icons/status-completed.png", 64, 64),
        new("Assets/Icons/status-error.png", 64, 64),
        new("Assets/Icons/status-busy-0.png", 64, 64),
        new("Assets/Icons/status-busy-1.png", 64, 64),
        new("Assets/Icons/status-busy-2.png", 64, 64),
        new("Assets/Icons/status-busy-3.png", 64, 64),
        new("Assets/Icons/status-busy-4.png", 64, 64),
        new("Assets/Icons/status-busy-5.png", 64, 64),
        new("Assets/Icons/status-busy-6.png", 64, 64),
        new("Assets/Icons/status-busy-7.png", 64, 64)
    ];

    private static readonly FileAssetSpec[] BinaryAssets =
    [
        new("Assets/Icons/CodeXPets.ico", 1_024),
        new("Assets/Audio/voice-start.mp3", 1_024),
        new("Assets/Audio/voice-complete.mp3", 1_024),
        new("Assets/Audio/voice-error.mp3", 1_024)
    ];

    public static bool Validate(Func<string, Stream> openAsset, out string error)
    {
        ArgumentNullException.ThrowIfNull(openAsset);
        try
        {
            foreach (var asset in ImageAssets)
            {
                using var stream = openAsset(asset.Path);
                using var bitmap = SKBitmap.Decode(stream);
                if (bitmap is null)
                {
                    error = "图片无法解码：" + asset.Path;
                    return false;
                }

                if (bitmap.Width != asset.Width || bitmap.Height != asset.Height)
                {
                    error = $"图片尺寸错误：{asset.Path}，实际 {bitmap.Width}x{bitmap.Height}，" +
                            $"预期 {asset.Width}x{asset.Height}";
                    return false;
                }
            }

            foreach (var asset in BinaryAssets)
            {
                using var stream = openAsset(asset.Path);
                if (stream.Length < asset.MinimumLength)
                {
                    error = "二进制资源为空或不完整：" + asset.Path;
                    return false;
                }
            }

            if (!ValidateFloatingCat(openAsset, out error) ||
                !ValidateDockCat(openAsset, out error) ||
                !ValidateAppIcon(openAsset, out error) ||
                !ValidateStatusIcons(openAsset, out error))
            {
                return false;
            }

            error = string.Empty;
            return true;
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    private static bool ValidateFloatingCat(Func<string, Stream> openAsset, out string error)
    {
        const string path = "Assets/Images/white-british-shorthair-spritesheet.png";
        using var stream = openAsset(path);
        using var bitmap = SKBitmap.Decode(stream);
        if (bitmap is null)
        {
            error = "白色英短精灵表无法解码。";
            return false;
        }

        var palette = CountPalette(bitmap, new SKRectI(0, 0, bitmap.Width, bitmap.Height));
        if (palette.Opaque < 100_000 || palette.Transparent < bitmap.Width * bitmap.Height * 0.60 ||
            palette.PureWhite < palette.Opaque * 0.60 || palette.Dark < palette.Opaque * 0.15 ||
            palette.EyeYellow < 800)
        {
            error = "浮动精灵配色校验失败：需要白色身体、深色耳朵/尾巴、金黄色眼睛，并保持透明背景。";
            return false;
        }

        var ears = CountPalette(bitmap, new SKRectI(35, 25, 150, 80));
        var tail = CountPalette(bitmap, new SKRectI(125, 95, 192, 200));
        var face = CountPalette(bitmap, new SKRectI(55, 65, 135, 110));
        var body = CountPalette(bitmap, new SKRectI(45, 65, 145, 205));
        if (ears.Dark < 600 || ears.WarmAccent < 100 || tail.Dark < 500 ||
            face.EyeYellow < 50 || body.PureWhite < 4_000)
        {
            error = "浮动精灵关键区域校验失败：请确认深色耳朵、深色尾巴、金黄色眼睛和白色身体均保留。";
            return false;
        }

        for (var row = 0; row < 4; row++)
        {
            var frameCount = row == 1 ? 4 : 8;
            for (var frame = 0; frame < frameCount; frame++)
            {
                var cell = new SKRectI(frame * ResourceCatalog.SpriteCellWidth,
                    row * ResourceCatalog.SpriteCellHeight,
                    (frame + 1) * ResourceCatalog.SpriteCellWidth,
                    (row + 1) * ResourceCatalog.SpriteCellHeight);
                if (CountPalette(bitmap, cell).Opaque >= 100) continue;
                error = $"白色英短精灵表缺少动作帧：row={row}, frame={frame}";
                return false;
            }
        }

        var seated = FindOpaqueBounds(bitmap, new SKRectI(0, 0, 192, 208));
        var walking = FindOpaqueBounds(bitmap, new SKRectI(0, 416, 192, 624));
        var thinking = FindOpaqueBounds(bitmap, new SKRectI(5 * 192, 416, 6 * 192, 624));
        if (seated.Height <= seated.Width * 1.05 ||
            walking.Width <= walking.Height * 1.15 ||
            thinking.Height <= thinking.Width * 1.05)
        {
            error = "白色英短空闲、行走或思考姿势轮廓与原型不一致。";
            return false;
        }

        error = string.Empty;
        return true;
    }

    private static bool ValidateDockCat(Func<string, Stream> openAsset, out string error)
    {
        const string path = "Assets/Images/british-shorthair-dock-spritesheet.png";
        using var stream = openAsset(path);
        using var bitmap = SKBitmap.Decode(stream);
        if (bitmap is null)
        {
            error = "白色英短扒边精灵表无法解码。";
            return false;
        }

        var palette = CountPalette(bitmap, new SKRectI(0, 0, bitmap.Width, bitmap.Height));
        if (palette.Opaque < 100_000 || palette.Transparent < bitmap.Width * bitmap.Height * 0.50 ||
            palette.PureWhite < palette.Opaque * 0.65 || palette.Dark < palette.Opaque * 0.15 ||
            palette.EyeYellow < 1_000)
        {
            error = "扒边精灵配色校验失败：需要白色身体、深色耳朵/尾巴和金黄色眼睛。";
            return false;
        }

        var leftEars = CountPalette(bitmap, new SKRectI(0, 0, 180, 112));
        var rightEars = CountPalette(bitmap, new SKRectI(4 * 256 + 76, 0, 5 * 256, 112));
        if (leftEars.Dark < 1_500 || rightEars.Dark < 1_500 ||
            leftEars.WarmAccent < 500 || rightEars.WarmAccent < 500)
        {
            error = "扒边精灵耳朵配色校验失败：需要保留深色外耳和可见的暖色内耳。";
            return false;
        }

        SKRectI? leftReference = null;
        SKRectI? rightReference = null;
        for (var expression = 0; expression < 4; expression++)
        {
            var leftCell = new SKRectI(expression * 256, 0, (expression + 1) * 256, 256);
            var rightCell = new SKRectI((4 + expression) * 256, 0, (5 + expression) * 256, 256);
            var left = FindOpaqueBounds(bitmap, leftCell);
            var right = FindOpaqueBounds(bitmap, rightCell);
            if (left.Left != leftCell.Left || right.Right != rightCell.Right ||
                left.Height <= left.Width || right.Height <= right.Width)
            {
                error = $"扒边姿势未贴紧屏幕边缘：expression={expression}";
                return false;
            }

            var requiresGoldEyes = expression is 0 or 3;
            if (requiresGoldEyes &&
                (!DockFrameHasTwoGoldEyes(bitmap, expression) ||
                 !DockFrameHasTwoGoldEyes(bitmap, 4 + expression)))
            {
                error = $"扒边睁眼帧必须保留两只金黄色眼睛：expression={expression}";
                return false;
            }

            if (expression == 0)
            {
                leftReference = left;
                rightReference = right;
                continue;
            }

            if (leftReference is not { } leftBase || rightReference is not { } rightBase ||
                left.Top != leftBase.Top || left.Width != leftBase.Width || left.Height != leftBase.Height ||
                right.Top != rightBase.Top || right.Width != rightBase.Width || right.Height != rightBase.Height ||
                !RegionsEqual(bitmap, new SKRectI(0, 147, 40, 256),
                    new SKRectI(expression * 256, 147, expression * 256 + 40, 256)) ||
                !RegionsEqual(bitmap, new SKRectI(4 * 256 + 216, 147, 5 * 256, 256),
                    new SKRectI((4 + expression) * 256 + 216, 147, (5 + expression) * 256, 256)))
            {
                error = $"扒边表情帧改变了固定姿势或抓边前爪：expression={expression}";
                return false;
            }
        }

        error = string.Empty;
        return true;
    }

    private static bool ValidateAppIcon(Func<string, Stream> openAsset, out string error)
    {
        const string path = "Assets/Icons/AppIcon.png";
        using var stream = openAsset(path);
        using var bitmap = SKBitmap.Decode(stream);
        if (bitmap is null)
        {
            error = "应用图标无法解码。";
            return false;
        }

        var body = CountPalette(bitmap, new SKRectI(300, 600, 700, 930));
        var leftEar = CountPalette(bitmap, new SKRectI(240, 250, 430, 460));
        var rightEar = CountPalette(bitmap, new SKRectI(480, 250, 670, 460));
        var tail = CountPalette(bitmap, new SKRectI(650, 650, 830, 930));
        var leftEye = CountPalette(bitmap, new SKRectI(300, 480, 370, 570));
        var rightEye = CountPalette(bitmap, new SKRectI(465, 480, 540, 570));
        if (body.PureWhite < 80_000 || leftEar.PureBlack < 9_000 ||
            rightEar.PureBlack < 9_000 || leftEar.WarmAccent > 500 ||
            rightEar.WarmAccent > 500 || tail.PureBlack < 18_000 ||
            leftEye.EyeYellow < 1_000 || rightEye.EyeYellow < 1_000)
        {
            error = "应用图标中的英短配色错误：应为纯白身体、黑色双耳和尾巴、黄色双眼。";
            return false;
        }

        var backgroundSamples = new[]
        {
            new SKPointI(220, 230), new SKPointI(830, 230),
            new SKPointI(210, 500), new SKPointI(835, 500),
            new SKPointI(220, 900), new SKPointI(825, 900)
        };
        if (backgroundSamples.Any(point => IsNear(bitmap.GetPixel(point.X, point.Y), 255, 255, 255, 3)))
        {
            error = "应用图标猫咪周围出现了白色矩形背景。";
            return false;
        }

        error = string.Empty;
        return true;
    }

    private static bool ValidateStatusIcons(Func<string, Stream> openAsset, out string error)
    {
        foreach (var asset in ImageAssets.Where(item => item.Path.StartsWith(
                     "Assets/Icons/status-", StringComparison.Ordinal)))
        {
            using var stream = openAsset(asset.Path);
            using var bitmap = SKBitmap.Decode(stream);
            if (bitmap is null)
            {
                error = "状态图标无法解码：" + asset.Path;
                return false;
            }

            var bounds = FindOpaqueBounds(bitmap, new SKRectI(0, 0, bitmap.Width, bitmap.Height));
            if (bounds.Width < 54 || bounds.Height < 54)
            {
                error = $"任务栏状态点过小：{asset.Path}，不透明范围仅 {bounds.Width}x{bounds.Height}";
                return false;
            }
        }

        error = string.Empty;
        return true;
    }

    private static bool DockFrameHasTwoGoldEyes(SKBitmap bitmap, int frame)
    {
        var offset = frame * ResourceCatalog.DockCellSize;
        var mirrored = frame >= 4;
        var highEye = mirrored
            ? new SKRectI(offset + 204, 105, offset + 241, 143)
            : new SKRectI(offset + 15, 105, offset + 52, 143);
        var lowEye = mirrored
            ? new SKRectI(offset + 145, 139, offset + 179, 176)
            : new SKRectI(offset + 77, 139, offset + 111, 176);
        return CountPalette(bitmap, highEye).EyeYellow >= 80 &&
               CountPalette(bitmap, lowEye).EyeYellow >= 80;
    }

    private static SKRectI FindOpaqueBounds(SKBitmap bitmap, SKRectI region)
    {
        var minX = region.Right;
        var minY = region.Bottom;
        var maxX = region.Left - 1;
        var maxY = region.Top - 1;
        for (var y = region.Top; y < region.Bottom; y++)
        {
            for (var x = region.Left; x < region.Right; x++)
            {
                if (bitmap.GetPixel(x, y).Alpha <= 16) continue;
                minX = Math.Min(minX, x);
                minY = Math.Min(minY, y);
                maxX = Math.Max(maxX, x);
                maxY = Math.Max(maxY, y);
            }
        }

        return maxX < minX ? SKRectI.Empty : new SKRectI(minX, minY, maxX + 1, maxY + 1);
    }

    private static bool RegionsEqual(SKBitmap bitmap, SKRectI first, SKRectI second)
    {
        if (first.Width != second.Width || first.Height != second.Height) return false;
        for (var y = 0; y < first.Height; y++)
        {
            for (var x = 0; x < first.Width; x++)
            {
                if (bitmap.GetPixel(first.Left + x, first.Top + y) !=
                    bitmap.GetPixel(second.Left + x, second.Top + y)) return false;
            }
        }

        return true;
    }

    private static PaletteCounts CountPalette(SKBitmap bitmap, SKRectI region)
    {
        var left = Math.Clamp(region.Left, 0, bitmap.Width);
        var top = Math.Clamp(region.Top, 0, bitmap.Height);
        var right = Math.Clamp(region.Right, left, bitmap.Width);
        var bottom = Math.Clamp(region.Bottom, top, bitmap.Height);
        var counts = new PaletteCounts();
        for (var y = top; y < bottom; y++)
        {
            for (var x = left; x < right; x++)
            {
                var color = bitmap.GetPixel(x, y);
                if (color.Alpha < 16)
                {
                    counts.Transparent++;
                    continue;
                }

                if (color.Alpha < 96)
                {
                    continue;
                }

                counts.Opaque++;
                if (IsNear(color, 255, 255, 255, 5)) counts.PureWhite++;
                if (IsNear(color, 0, 0, 0, 5)) counts.PureBlack++;
                if (IsNear(color, 255, 204, 0, 5)) counts.EyeYellow++;
                if (color.Red > 100 && color.Red > color.Green * 1.12 &&
                    color.Red > color.Blue * 1.08 && !IsNear(color, 255, 204, 0, 35))
                {
                    counts.WarmAccent++;
                }

                if (Math.Max(color.Red, Math.Max(color.Green, color.Blue)) <= 80)
                {
                    counts.Dark++;
                }
            }
        }

        return counts;
    }

    private static bool IsNear(SKColor color, byte red, byte green, byte blue, int tolerance) =>
        Math.Abs(color.Red - red) <= tolerance &&
        Math.Abs(color.Green - green) <= tolerance &&
        Math.Abs(color.Blue - blue) <= tolerance;

    private sealed record ImageAssetSpec(string Path, int Width, int Height);

    private sealed record FileAssetSpec(string Path, long MinimumLength);

    private sealed class PaletteCounts
    {
        public long Transparent { get; set; }
        public long Opaque { get; set; }
        public long PureWhite { get; set; }
        public long PureBlack { get; set; }
        public long EyeYellow { get; set; }
        public long WarmAccent { get; set; }
        public long Dark { get; set; }
    }
}
