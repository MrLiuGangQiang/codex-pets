using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.TextFormatting;
using Avalonia.Media.Imaging;
using Avalonia.Rendering;
using CodeXPets.App.Services;
using CodeXPets.Core.Application;
using CodeXPets.Core.Domain;
using System.Text.RegularExpressions;

namespace CodeXPets.App.Views;

public sealed class PetSurface : Control
{
    private const double BubbleWidth = 270;
    // Keep the cloud compact while giving it a slightly rounder silhouette.
    private const double BubbleHeight = 110;
    private const double BubbleVerticalInset = 45;
    private const double DockBubbleHorizontalOffset = 48;
    private const double DockBubbleAboveVerticalOffset = 6;
    private const double DockBubbleBelowVerticalOffset = 2;
    private const double DockPetAboveVerticalOffset = 7;
    private const double DockPetBelowVerticalOffset = 4;
    private const double PetWidth = 130;
    private const double PetHeight = 140;
    private const double DockSize = 104;
    private const double BodyLineHeight = 15;
    private const int VisibleBodyLineCount = 3;
    internal const double SessionSwitchIntervalSeconds = 6;
    private const double InitialScrollHoldSeconds = 1.9;
    private const double EndScrollHoldSeconds = 1.7;
    private const double ContentScrollSpeed = 15;
    private static readonly IBrush IdleHeaderBrush = new SolidColorBrush(Color.FromRgb(67, 105, 139));
    private static readonly IBrush BusyHeaderBrush = new SolidColorBrush(Color.FromRgb(43, 105, 168));
    private static readonly IBrush CompletedHeaderBrush = new SolidColorBrush(Color.FromRgb(43, 139, 87));
    private static readonly IBrush ErrorHeaderBrush = new SolidColorBrush(Color.FromRgb(194, 57, 52));
    private static readonly IBrush BodyBrush = new SolidColorBrush(Color.FromRgb(45, 60, 78));
    private static readonly IBrush DotOutlineBrush = new SolidColorBrush(Color.FromRgb(42, 50, 60));
    private static readonly IBrush DotFillBrush = new SolidColorBrush(Color.FromRgb(241, 248, 255));
    private static readonly IBrush BulbOutlineBrush = new SolidColorBrush(Color.FromRgb(68, 43, 25));
    private static readonly IBrush BulbGlowBrush = new SolidColorBrush(Color.FromRgb(83, 169, 236));
    private static readonly IBrush BulbHighlightBrush = new SolidColorBrush(Color.FromRgb(202, 232, 255));
    private static readonly IBrush BulbErrorGlowBrush = new SolidColorBrush(Color.FromRgb(226, 62, 55));
    private static readonly IBrush BulbErrorHighlightBrush = new SolidColorBrush(Color.FromRgb(255, 174, 154));
    private static readonly IBrush BulbBaseBrush = new SolidColorBrush(Color.FromRgb(91, 78, 70));
    private static readonly (int Row, int X, int Width)[] BulbSilhouette =
    [
        (3, 4, 5), (4, 3, 7), (5, 2, 9), (6, 2, 9),
        (7, 2, 9), (8, 3, 7), (9, 4, 5), (10, 5, 3),
        (11, 4, 5), (12, 4, 5), (13, 5, 3)
    ];
    private static readonly Typeface HeaderTypeface = new(FontFamily.Default, FontStyle.Normal, FontWeight.ExtraBold);
    private static readonly Typeface BodyTypeface = new(FontFamily.Default, FontStyle.Normal, FontWeight.Normal);

    private readonly ResourceCatalog _resources;
    private string _statusText = "空闲";
    private string _thoughtText = "主人，现在没有在进行中的任务!别让我歇着!";
    private ReminderState _state = ReminderState.Idle;
    private IReadOnlyList<string> _taskTitles = [];
    private IReadOnlyList<string?> _progressLabels = [];
    private int _taskIndex;
    private int _animationTick;
    private double _scrollOffset;
    private TextLayout? _headerLayout;
    private string _headerLayoutText = string.Empty;
    private double _headerLayoutWidth;
    private double _headerLayoutHeight;
    private ReminderState _headerLayoutState;
    private TextLayout? _bodyLayout;
    private string _layoutText = string.Empty;
    private double _layoutWidth;
    private string _normalizedSourceText = string.Empty;
    private string _normalizedDisplayText = string.Empty;
    private bool _hasScrollableBody;
    private double _scrollHoldSeconds = InitialScrollHoldSeconds;
    private double _sessionRotationSeconds;
    private bool _scrollAtEnd;

    public PetSurface(ResourceCatalog resources)
    {
        _resources = resources;
        ClipToBounds = false;
        RenderOptions.SetBitmapInterpolationMode(this, BitmapInterpolationMode.HighQuality);
    }

    public bool IsDocked { get; set; }
    public DockEdge DockEdge { get; set; }
    public bool DockBubbleBelow { get; set; }
    public double DockVisibility { get; set; } = 1;
    public bool MirrorFloatingSprite { get; set; }
    public DateTimeOffset DockThoughtUntil { get; set; } = DateTimeOffset.MinValue;
    public ReminderState State => _state;
    public int SelectedTaskIndex => _taskIndex;
    internal double ScrollOffset => _scrollOffset;
    internal bool NeedsSmoothAnimation => ShouldShowThoughtBubble && _hasScrollableBody &&
        _scrollHoldSeconds <= 0 && !_scrollAtEnd;

    public bool ShouldShowThoughtBubble => AppLogic.ShouldShowThoughtBubble(
        IsDocked, _state, DateTimeOffset.Now, DockThoughtUntil);

    public Rect BubbleBounds
    {
        get
        {
            var y = DockBubbleBelow
                ? Bounds.Height - BubbleHeight - BubbleVerticalInset -
                  (IsDocked ? DockBubbleBelowVerticalOffset : 0)
                : BubbleVerticalInset + (IsDocked ? DockBubbleAboveVerticalOffset : 0);
            var x = (Bounds.Width - BubbleWidth) / 2;
            if (IsDocked)
            {
                x += DockEdge switch
                {
                    DockEdge.Left => -DockBubbleHorizontalOffset,
                    DockEdge.Right => DockBubbleHorizontalOffset,
                    _ => 0
                };
            }

            return new Rect(x, y, BubbleWidth, BubbleHeight);
        }
    }

    public Rect ContentBounds
    {
        get
        {
            var cloud = BubbleBounds;
            var x = cloud.X + cloud.Width * 0.30;
            var y = cloud.Y + cloud.Height * 0.34;
            var maxRight = cloud.X + cloud.Width * 0.80;
            var maxBottom = cloud.Y + cloud.Height * 0.82;
            return new Rect(x, y, Math.Max(1, Math.Min(156, maxRight - x)),
                Math.Max(1, Math.Min(BodyLineHeight * VisibleBodyLineCount, maxBottom - y)));
        }
    }

    public Rect HeaderBounds
    {
        get
        {
            var cloud = BubbleBounds;
            return new Rect(cloud.X + cloud.Width * 0.26, cloud.Y + cloud.Height * 0.10,
                cloud.Width * 0.52, cloud.Height * 0.20);
        }
    }

    public Rect DockPetBounds
    {
        get
        {
            var smooth = SmoothStep(DockVisibility);
            var hiddenOffset = DockSize * (1 - smooth);
            var x = DockEdge == DockEdge.Left ? -hiddenOffset : Bounds.Width - DockSize + hiddenOffset;
            var y = DockBubbleBelow
                ? DockPetBelowVerticalOffset
                : Bounds.Height - DockSize - DockPetAboveVerticalOffset;
            return new Rect(x, y, DockSize, DockSize);
        }
    }

    public Rect VisiblePetBounds => IsDocked ? GetDockPose().Visible : GetFloatingPose().Visible;

    public Rect VisibleCloudBounds => GetVisibleCloudBounds();

    public IReadOnlyList<Rect> ThoughtDotRects
    {
        get
        {
            if (!ShouldShowThoughtBubble) return [];
            var (large, small) = ThoughtDotBounds();
            return [large, small];
        }
    }

    public void UpdateStatus(string status, string thought, ReminderState state,
        IReadOnlyList<string> titles, IReadOnlyList<string?>? progressLabels,
        bool selectNewestTask, int preferredTaskIndex)
    {
        var selectedTitle = GetSelectedTaskTitle();
        var previousDisplayedText = GetDisplayedText();
        var previousIndex = _taskIndex;
        var previousTaskCount = _taskTitles.Count;
        var normalizedProgress = Enumerable.Range(0, titles.Count)
            .Select(index => progressLabels is not null && index < progressLabels.Count
                ? progressLabels[index]
                : null)
            .ToArray();
        _statusText = status;
        _thoughtText = thought;
        _state = state;
        _taskTitles = titles.ToArray();
        _progressLabels = normalizedProgress;

        _taskIndex = AppLogic.ReconcileTaskSelection(state, _taskTitles, previousIndex,
            selectedTitle, selectNewestTask, preferredTaskIndex);

        var selectionChanged = previousIndex != _taskIndex;
        if (selectionChanged || !string.Equals(previousDisplayedText, GetDisplayedText(), StringComparison.Ordinal))
            ResetContentScroll();
        if (selectionChanged || (previousTaskCount <= 1) != (_taskTitles.Count <= 1))
            ResetSessionRotation();
        InvalidateVisual();
    }

    public bool SwitchToNextTask()
    {
        if (_taskTitles.Count <= 1) return false;
        _taskIndex = (_taskIndex + 1) % _taskTitles.Count;
        ResetContentScroll();
        ResetSessionRotation();
        InvalidateVisual();
        return true;
    }

    public bool IsTaskSwitchPoint(Point point)
    {
        var bubble = BubbleBounds;
        var content = ContentBounds;
        return AppLogic.IsTaskSwitchPoint(IsDocked, ShouldShowThoughtBubble, _state,
            _taskTitles.Count, ToRectD(bubble), ToRectD(content), new PointD(point.X, point.Y));
    }

    public bool IsInteractivePoint(Point point)
    {
        if (IsPetPixel(point)) return true;
        if (!ShouldShowThoughtBubble) return false;
        if (IsCloudPixel(point)) return true;
        foreach (var dot in ThoughtDotRects)
            if (dot.Contains(point)) return true;
        return false;
    }

    /// <summary>
    /// Builds the exact window region used by SetWindowRgn. It is the same
    /// pixel-level data as IsInteractivePoint: opaque cat/cloud pixels plus the
    /// thought-dot rectangles, so there are no swallowed-click dead zones.
    /// </summary>
    public int GetClickRegionSignature()
    {
        if (IsDocked)
        {
            var pose = GetDockPose();
            return HashCode.Combine(IsDocked, DockEdge, (int)Math.Round(DockVisibility * 10),
                MirrorFloatingSprite, ShouldShowThoughtBubble, 1000 + pose.Index);
        }

        var floating = GetFloatingPose();
        return HashCode.Combine(IsDocked, DockEdge, (int)Math.Round(DockVisibility * 10),
            MirrorFloatingSprite, ShouldShowThoughtBubble, floating.Row * 100 + floating.Frame);
    }

    public IReadOnlyList<Rect> BuildClickRegionRects()
    {
        var rects = new List<Rect>();
        if (IsDocked)
        {
            var pose = GetDockPose();
            TransformRuns(rects, _resources.GetDockRegionRects(pose.Index),
                pose.Destination, ResourceCatalog.DockCellSize, ResourceCatalog.DockCellSize, mirrorX: false);
        }
        else
        {
            var pose = GetFloatingPose();
            TransformRuns(rects, _resources.GetCatRegionRects(pose.Row, pose.Frame),
                pose.Destination, ResourceCatalog.SpriteCellWidth, ResourceCatalog.SpriteCellHeight,
                mirrorX: MirrorFloatingSprite);
        }

        if (ShouldShowThoughtBubble)
        {
            TransformRuns(rects, _resources.GetCloudRegionRects(),
                BubbleBounds, _resources.CloudBubble.PixelSize.Width, _resources.CloudBubble.PixelSize.Height,
                mirrorX: false);
            rects.AddRange(ThoughtDotRects);
        }
        return rects;
    }

    private static void TransformRuns(List<Rect> rects, IReadOnlyList<Rect> runs, Rect destination,
        double sourceWidth, double sourceHeight, bool mirrorX)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0 || destination.Width <= 0 || destination.Height <= 0) return;
        foreach (var run in runs)
        {
            var srcX0 = run.X;
            var srcX1 = run.X + run.Width;
            if (mirrorX)
            {
                var a = sourceWidth - srcX1;
                var b = sourceWidth - srcX0;
                srcX0 = a;
                srcX1 = b;
            }

            var left = destination.X + srcX0 / sourceWidth * destination.Width;
            var right = destination.X + srcX1 / sourceWidth * destination.Width;
            var top = destination.Y + run.Y / sourceHeight * destination.Height;
            var height = destination.Height / sourceHeight;
            rects.Add(new Rect(left, top, Math.Max(0.5, right - left), Math.Max(0.5, height)));
        }
    }

    private bool IsCloudPixel(Point point)
    {
        var bounds = BubbleBounds;
        if (!bounds.Contains(point) || bounds.Width <= 0 || bounds.Height <= 0) return false;
        var width = Math.Max(1, _resources.CloudBubble.PixelSize.Width);
        var height = Math.Max(1, _resources.CloudBubble.PixelSize.Height);
        var x = (int)Math.Floor((point.X - bounds.X) / bounds.Width * width);
        var y = (int)Math.Floor((point.Y - bounds.Y) / bounds.Height * height);
        return _resources.IsCloudOpaque(x, y);
    }

    private bool IsPetPixel(Point point)
    {
        if (IsDocked)
        {
            var pose = GetDockPose();
            if (!pose.Destination.Contains(point)) return false;
            var x = (int)Math.Floor((point.X - pose.Destination.X) / pose.Destination.Width * ResourceCatalog.DockCellSize);
            var y = (int)Math.Floor((point.Y - pose.Destination.Y) / pose.Destination.Height * ResourceCatalog.DockCellSize);
            return _resources.IsDockPetOpaque(pose.Index, x, y);
        }

        var floating = GetFloatingPose();
        if (!floating.Destination.Contains(point)) return false;
        var xFloating = (int)Math.Floor((point.X - floating.Destination.X) / floating.Destination.Width * ResourceCatalog.SpriteCellWidth);
        var yFloating = (int)Math.Floor((point.Y - floating.Destination.Y) / floating.Destination.Height * ResourceCatalog.SpriteCellHeight);
        return _resources.IsCatOpaque(floating.Row, floating.Frame, xFloating, yFloating, MirrorFloatingSprite);
    }

    public bool Animate(int tick, double elapsedSeconds)
    {
        var previousTick = _animationTick;
        _animationTick = tick;
        var frameChanged = IsPetFrameChanged(previousTick, tick);
        var bubbleVisible = ShouldShowThoughtBubble;
        var scrollChanged = bubbleVisible && AdvanceScroll(elapsedSeconds);
        var sessionChanged = bubbleVisible && AdvanceSessionRotation(elapsedSeconds);
        if (frameChanged || scrollChanged || sessionChanged) InvalidateVisual();
        return frameChanged || scrollChanged || sessionChanged;
    }

    private bool IsPetFrameChanged(int previousTick, int currentTick)
    {
        if (previousTick == currentTick) return false;
        return IsDocked
            ? AppLogic.SelectDockSpriteIndex(DockEdge, _state, previousTick) !=
              AppLogic.SelectDockSpriteIndex(DockEdge, _state, currentTick)
            : AppLogic.SelectFloatingFrame(_state, previousTick) !=
              AppLogic.SelectFloatingFrame(_state, currentTick);
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        if (ShouldShowThoughtBubble) DrawThoughtBubble(context);
        DrawPet(context);
    }

    private void DrawThoughtBubble(DrawingContext context)
    {
        var cloud = BubbleBounds;
        using (context.PushRenderOptions(new RenderOptions
        {
            BitmapInterpolationMode = BitmapInterpolationMode.None
        }))
        {
            context.DrawImage(_resources.CloudBubble,
                new Rect(0, 0, _resources.CloudBubble.PixelSize.Width, _resources.CloudBubble.PixelSize.Height),
                cloud);
        }
        var (large, small) = ThoughtDotBounds();
        DrawThoughtDot(context, large);
        DrawThoughtDot(context, small);
        DrawLightBulb(context, _state == ReminderState.Error);

        var selectedProgress = _taskIndex >= 0 && _taskIndex < _progressLabels.Count
            ? _progressLabels[_taskIndex]
            : null;
        var header = _state == ReminderState.Busy
            ? AppLogic.FormatBusyHeader(selectedProgress, _taskIndex, _taskTitles.Count)
            : _statusText;
        var headerBounds = HeaderBounds;
        EnsureHeaderLayout(header, headerBounds);
        if (_headerLayout is not null)
        {
            var y = headerBounds.Y + Math.Max(0, (headerBounds.Height - _headerLayout.Height) / 2);
            _headerLayout.Draw(context, new Point(headerBounds.X, y));
        }

        var body = GetNormalizedDisplayText();
        var viewport = ContentBounds;
        EnsureBodyLayout(body, viewport.Width);
        if (_bodyLayout is null) return;
        using (context.PushClip(viewport))
            _bodyLayout.Draw(context, new Point(viewport.X, viewport.Y - _scrollOffset));
    }

    private void DrawPet(DrawingContext context)
    {
        if (IsDocked)
        {
            var pose = GetDockPose();
            var frame = _resources.DockFrames[pose.Index];
            var source = new Rect(0, 0, frame.PixelSize.Width, frame.PixelSize.Height);
            // Each dock pose is a standalone bitmap. This prevents texture filtering
            // from ever sampling an adjacent expression frame at fractional DPI/offsets.
            using (context.PushRenderOptions(new RenderOptions
            {
                BitmapInterpolationMode = BitmapInterpolationMode.None
            }))
            {
                context.DrawImage(frame, source, pose.Destination);
            }
            return;
        }

        var floatingPose = GetFloatingPose();
        var sourceRect = new Rect(floatingPose.Frame * ResourceCatalog.SpriteCellWidth,
            floatingPose.Row * ResourceCatalog.SpriteCellHeight,
            ResourceCatalog.SpriteCellWidth, ResourceCatalog.SpriteCellHeight);
        var destination = floatingPose.Destination;
        if (MirrorFloatingSprite)
        {
            var centerX = destination.X + destination.Width / 2;
            using (context.PushTransform(Matrix.CreateTranslation(-centerX, 0) *
                   Matrix.CreateScale(-1, 1) *
                   Matrix.CreateTranslation(centerX, 0)))
            {
                context.DrawImage(_resources.CatSpriteSheet, sourceRect, destination);
            }
        }
        else
        {
            context.DrawImage(_resources.CatSpriteSheet, sourceRect, destination);
        }
    }

    private (int Row, int Frame, Rect Destination, Rect Visible) GetFloatingPose()
    {
        var row = AppLogic.SelectFloatingSpriteRow(_state);
        var frame = AppLogic.SelectFloatingFrame(_state, _animationTick);
        var metrics = _resources.SpriteMetrics[row, frame];
        if (!metrics.HasPixels) metrics = SpriteFrameMetric.Default;
        var scaleX = PetWidth / ResourceCatalog.SpriteCellWidth;
        var scaleY = PetHeight / ResourceCatalog.SpriteCellHeight;
        var targetX = Bounds.Width / 2d;
        var groundY = Bounds.Height - 3;
        var anchorX = MirrorFloatingSprite
            ? ResourceCatalog.SpriteCellWidth - metrics.AnchorX
            : metrics.AnchorX;
        var destination = new Rect(targetX - anchorX * scaleX,
            groundY - metrics.Bottom * scaleY, PetWidth, PetHeight);
        var opaque = metrics.OpaqueBounds;
        var opaqueX = MirrorFloatingSprite
            ? ResourceCatalog.SpriteCellWidth - opaque.Right
            : opaque.X;
        var visible = new Rect(destination.X + opaqueX * scaleX,
            destination.Y + opaque.Y * scaleY,
            Math.Max(1, opaque.Width * scaleX), Math.Max(1, opaque.Height * scaleY));
        return (row, frame, destination, visible);
    }

    private (int Index, Rect Destination, Rect Visible) GetDockPose()
    {
        var index = AppLogic.SelectDockSpriteIndex(DockEdge, _state, _animationTick);
        var destination = DockPetBounds;
        var opaque = index >= 0 && index < _resources.DockOpaqueBounds.Length
            ? _resources.DockOpaqueBounds[index]
            : new Rect(0, 0, ResourceCatalog.DockCellSize, ResourceCatalog.DockCellSize);
        var scale = DockSize / ResourceCatalog.DockCellSize;
        var visible = new Rect(destination.X + opaque.X * scale,
            destination.Y + opaque.Y * scale,
            Math.Max(1, opaque.Width * scale), Math.Max(1, opaque.Height * scale));
        return (index, destination, visible);
    }

    private IBrush GetHeaderBrush() => _state switch
    {
        ReminderState.Busy => BusyHeaderBrush,
        ReminderState.Completed => CompletedHeaderBrush,
        ReminderState.Error => ErrorHeaderBrush,
        _ => IdleHeaderBrush
    };

    private void DrawLightBulb(DrawingContext context, bool error)
    {
        const double cellSize = 2.5;
        var cloud = GetVisibleCloudBounds();
        // Keep the lamp in the cloud's left safe area; the text starts farther
        // right when a lamp is present, so the two never overlap.
        var originX = Math.Round(cloud.X + cloud.Width * 0.10);
        var originY = Math.Round(cloud.Y + cloud.Height * 0.32);
        var glow = error ? BulbErrorGlowBrush : BulbGlowBrush;
        var highlight = error ? BulbErrorHighlightBrush : BulbHighlightBrush;

        void Cells(IBrush brush, int x, int y, int width, int height) =>
            context.DrawRectangle(brush, null,
                new Rect(originX + x * cellSize, originY + y * cellSize,
                    width * cellSize, height * cellSize));

        // Exact pixel-art lamp used by the original Windows implementation.
        Cells(BulbOutlineBrush, 6, 0, 1, 2);
        Cells(BulbOutlineBrush, 2, 2, 1, 1);
        Cells(BulbOutlineBrush, 10, 2, 1, 1);
        Cells(BulbOutlineBrush, 0, 6, 2, 1);
        Cells(BulbOutlineBrush, 11, 6, 2, 1);

        foreach (var (row, x, width) in BulbSilhouette)
            Cells(BulbOutlineBrush, x, row, width, 1);

        Cells(glow, 4, 4, 5, 1);
        Cells(glow, 3, 5, 7, 3);
        Cells(glow, 4, 8, 5, 1);
        Cells(glow, 5, 9, 3, 1);
        Cells(highlight, 4, 4, 2, 1);
        Cells(highlight, 3, 5, 2, 2);
        Cells(BulbOutlineBrush, 5, 7, 1, 2);
        Cells(BulbOutlineBrush, 7, 7, 1, 2);
        Cells(BulbOutlineBrush, 6, 9, 1, 1);
        Cells(BulbBaseBrush, 5, 11, 3, 2);
        Cells(BulbOutlineBrush, 5, 13, 3, 1);
    }

    private void DrawThoughtDot(DrawingContext context, Rect rect)
    {
        context.DrawGeometry(DotOutlineBrush, null, CreateOctagon(rect, 3));
        var inner = rect.Deflate(3);
        if (inner.Width > 0 && inner.Height > 0)
            context.DrawGeometry(DotFillBrush, null, CreateOctagon(inner, 1));
    }

    private static StreamGeometry CreateOctagon(Rect rect, double notch)
    {
        var n = Math.Min(notch, Math.Min(rect.Width, rect.Height) / 3);
        var geometry = new StreamGeometry();
        using var figure = geometry.Open();
        figure.BeginFigure(new Point(rect.X + n, rect.Y), true);
        figure.LineTo(new Point(rect.Right - n, rect.Y));
        figure.LineTo(new Point(rect.Right, rect.Y + n));
        figure.LineTo(new Point(rect.Right, rect.Bottom - n));
        figure.LineTo(new Point(rect.Right - n, rect.Bottom));
        figure.LineTo(new Point(rect.X + n, rect.Bottom));
        figure.LineTo(new Point(rect.X, rect.Bottom - n));
        figure.LineTo(new Point(rect.X, rect.Y + n));
        figure.EndFigure(true);
        return geometry;
    }

    private Rect GetVisibleCloudBounds()
    {
        var cloud = BubbleBounds;
        var source = _resources.CloudOpaqueBounds;
        var sourceWidth = Math.Max(1, _resources.CloudBubble.PixelSize.Width);
        var sourceHeight = Math.Max(1, _resources.CloudBubble.PixelSize.Height);
        return new Rect(cloud.X + source.X / sourceWidth * cloud.Width,
            cloud.Y + source.Y / sourceHeight * cloud.Height,
            source.Width / sourceWidth * cloud.Width,
            source.Height / sourceHeight * cloud.Height);
    }

    private (Rect Large, Rect Small) ThoughtDotBounds()
    {
        const double largeWidth = 17;
        const double largeHeight = 15;
        const double smallWidth = 11;
        const double smallHeight = 10;

        var cloud = BubbleBounds;
        var visibleCloud = GetVisibleCloudBounds();
        var pet = VisiblePetBounds;
        if (IsDocked)
        {
            // Build one connector from the cloud-facing edge to the center of the cat's
            // head. The two dots are laid out with equal visible gaps (cloud → large →
            // small → cat), rather than interpolating unrelated X/Y coordinates.
            var cloudInset = Math.Min(38, visibleCloud.Width * 0.18);
            var cloudAnchorX = DockEdge == DockEdge.Left
                ? visibleCloud.X + cloudInset
                : visibleCloud.Right - cloudInset;
            var inward = DockEdge == DockEdge.Left ? 1d : -1d;
            var petAnchorX = pet.Center.X + inward * 2;
            var cloudAnchorY = DockBubbleBelow ? visibleCloud.Y - 1 : visibleCloud.Bottom + 1;
            var petAnchorY = DockBubbleBelow ? pet.Bottom + 1 : pet.Y - 1;
            var cloudAnchor = new Point(cloudAnchorX, cloudAnchorY);
            var petAnchor = new Point(petAnchorX, petAnchorY);
            var perpendicularOffset = DockEdge == DockEdge.Left
                ? (DockBubbleBelow ? 8 : -8)
                : (DockBubbleBelow ? -8 : 8);
            return PlaceThoughtDotsWithEqualGaps(cloudAnchor, petAnchor,
                largeWidth, largeHeight, smallWidth, smallHeight,
                perpendicularOffset: perpendicularOffset);
        }

        var cloudCenterX = cloud.Center.X;
        var floatingDirection = MirrorFloatingSprite ? 1 : -1;
        var smallCenterX = cloudCenterX + floatingDirection * 11;
        var largeCenterX = cloudCenterX + floatingDirection * 7;
        double smallY;
        double largeY;
        if (DockBubbleBelow)
        {
            smallY = pet.Bottom + 2;
            largeY = Math.Min(visibleCloud.Y - largeHeight - 3,
                smallY + smallHeight + 3);
        }
        else
        {
            var desiredSmallY = pet.Y - smallHeight - 0.5;
            var minimumLargeY = visibleCloud.Bottom + 3;
            var maximumLargeY = visibleCloud.Bottom + 12;
            largeY = Math.Clamp(desiredSmallY - largeHeight - 3,
                minimumLargeY, maximumLargeY);
            smallY = Math.Max(desiredSmallY, largeY + largeHeight + 1);
        }
        return (new Rect(largeCenterX - largeWidth / 2, largeY, largeWidth, largeHeight),
            new Rect(smallCenterX - smallWidth / 2, smallY, smallWidth, smallHeight));
    }

    private static (Rect Large, Rect Small) PlaceThoughtDotsWithEqualGaps(
        Point cloudAnchor, Point petAnchor, double largeWidth, double largeHeight,
        double smallWidth, double smallHeight, double perpendicularOffset = 0)
    {
        var deltaX = petAnchor.X - cloudAnchor.X;
        var deltaY = petAnchor.Y - cloudAnchor.Y;
        var distance = Math.Sqrt(deltaX * deltaX + deltaY * deltaY);
        if (distance < 0.001)
        {
            return (new Rect(cloudAnchor.X - largeWidth / 2, cloudAnchor.Y - largeHeight / 2,
                    largeWidth, largeHeight),
                new Rect(petAnchor.X - smallWidth / 2, petAnchor.Y - smallHeight / 2,
                    smallWidth, smallHeight));
        }

        var unitX = deltaX / distance;
        var unitY = deltaY / distance;
        // Project each axis-aligned dot's half-size onto the connector direction.
        var largeRadius = (Math.Abs(unitX) * largeWidth + Math.Abs(unitY) * largeHeight) / 2;
        var smallRadius = (Math.Abs(unitX) * smallWidth + Math.Abs(unitY) * smallHeight) / 2;
        var gap = Math.Max(1, (distance - largeRadius * 2 - smallRadius * 2) / 3);
        var largeDistance = gap + largeRadius;
        var smallDistance = largeDistance + largeRadius + gap + smallRadius;
        if (smallDistance + smallRadius > distance)
        {
            // Very small layouts still preserve ordering and even center spacing.
            largeDistance = distance / 3;
            smallDistance = distance * 2 / 3;
        }

        var largeCenter = new Point(cloudAnchor.X + unitX * largeDistance,
            cloudAnchor.Y + unitY * largeDistance);
        // Offset the small (cat-side) dot perpendicular to the connector so the
        // two dots are visually staggered both vertically and horizontally.
        var normalX = -unitY;
        var normalY = unitX;
        var smallCenter = new Point(
            cloudAnchor.X + unitX * smallDistance + normalX * perpendicularOffset,
            cloudAnchor.Y + unitY * smallDistance + normalY * perpendicularOffset);
        return (new Rect(largeCenter.X - largeWidth / 2, largeCenter.Y - largeHeight / 2,
                largeWidth, largeHeight),
            new Rect(smallCenter.X - smallWidth / 2, smallCenter.Y - smallHeight / 2,
                smallWidth, smallHeight));
    }

    private void EnsureHeaderLayout(string text, Rect bounds)
    {
        if (_headerLayout is not null && _headerLayoutText == text &&
            _headerLayoutState == _state &&
            Math.Abs(_headerLayoutWidth - bounds.Width) < 0.1 &&
            Math.Abs(_headerLayoutHeight - bounds.Height) < 0.1)
        {
            return;
        }

        _headerLayout?.Dispose();
        _headerLayoutText = text;
        _headerLayoutWidth = bounds.Width;
        _headerLayoutHeight = bounds.Height;
        _headerLayoutState = _state;
        _headerLayout = new TextLayout(text, HeaderTypeface, 12.5, GetHeaderBrush(),
            TextAlignment.Center, TextWrapping.NoWrap, TextTrimming.CharacterEllipsis,
            maxWidth: Math.Max(1, bounds.Width), maxHeight: Math.Max(1, bounds.Height));
    }

    private void EnsureBodyLayout(string text, double width)
    {
        if (_bodyLayout is not null && _layoutText == text && Math.Abs(_layoutWidth - width) < 0.1)
            return;
        _bodyLayout?.Dispose();
        _layoutText = text;
        _layoutWidth = width;
        _bodyLayout = new TextLayout(text, BodyTypeface, 11.5, BodyBrush,
            TextAlignment.Left, TextWrapping.Wrap, TextTrimming.None,
            maxWidth: Math.Max(1, width), maxHeight: double.PositiveInfinity,
            lineHeight: BodyLineHeight);
    }

    private string GetNormalizedDisplayText()
    {
        var source = GetDisplayedText();
        if (ReferenceEquals(source, _normalizedSourceText) ||
            string.Equals(source, _normalizedSourceText, StringComparison.Ordinal))
        {
            return _normalizedDisplayText;
        }

        _normalizedSourceText = source;
        _normalizedDisplayText = NormalizeDisplayText(source);
        return _normalizedDisplayText;
    }

    private bool AdvanceScroll(double elapsedSeconds)
    {
        var text = GetNormalizedDisplayText();
        EnsureBodyLayout(text, ContentBounds.Width);
        var maxOffset = Math.Max(0, (_bodyLayout?.Height ?? 0) - ContentBounds.Height);
        _hasScrollableBody = maxOffset > 0;
        if (maxOffset <= 0)
        {
            var changed = Math.Abs(_scrollOffset) > 0.01;
            _scrollOffset = 0;
            _scrollHoldSeconds = InitialScrollHoldSeconds;
            _scrollAtEnd = false;
            return changed;
        }
        if (_scrollHoldSeconds > 0)
        {
            _scrollHoldSeconds = Math.Max(0, _scrollHoldSeconds - elapsedSeconds);
            return false;
        }
        if (!_scrollAtEnd)
        {
            var before = _scrollOffset;
            _scrollOffset = Math.Min(maxOffset, _scrollOffset + ContentScrollSpeed * elapsedSeconds);
            if (_scrollOffset >= maxOffset)
            {
                _scrollAtEnd = true;
                _scrollHoldSeconds = EndScrollHoldSeconds;
            }
            return Math.Abs(before - _scrollOffset) > 0.01;
        }

        // Scrolling is independent of session rotation: after reaching the bottom,
        // restart the same content from its first line and keep looping.
        ResetContentScroll();
        return true;
    }

    private bool AdvanceSessionRotation(double elapsedSeconds)
    {
        if (_taskTitles.Count <= 1)
        {
            ResetSessionRotation();
            return false;
        }

        _sessionRotationSeconds += Math.Max(0, elapsedSeconds);
        if (_sessionRotationSeconds + 1e-9 < SessionSwitchIntervalSeconds) return false;

        var steps = Math.Max(1,
            (int)Math.Floor((_sessionRotationSeconds + 1e-9) / SessionSwitchIntervalSeconds));
        _sessionRotationSeconds = Math.Max(0,
            _sessionRotationSeconds - steps * SessionSwitchIntervalSeconds);
        _taskIndex = (_taskIndex + steps) % _taskTitles.Count;
        ResetContentScroll();
        return true;
    }

    private void ResetContentScroll()
    {
        _scrollOffset = 0;
        _scrollHoldSeconds = InitialScrollHoldSeconds;
        _scrollAtEnd = false;
        _bodyLayout?.Dispose();
        _bodyLayout = null;
        _layoutText = string.Empty;
        _normalizedSourceText = string.Empty;
        _normalizedDisplayText = string.Empty;
        _hasScrollableBody = false;
    }

    private void ResetSessionRotation()
    {
        _sessionRotationSeconds = 0;
    }

    private string? GetSelectedTaskTitle() => _taskTitles.Count == 0
        ? null
        : _taskTitles[Math.Clamp(_taskIndex, 0, _taskTitles.Count - 1)];

    private string GetDisplayedText() => GetSelectedTaskTitle() ?? _thoughtText;

    private static string NormalizeDisplayText(string? text)
    {
        if (string.IsNullOrEmpty(text)) return string.Empty;
        var lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n')
            .Split('\n')
            .Select(line => Regex.Replace(line, "\\s+", " ").Trim())
            .Where(line => line.Length > 0);
        return string.Join('\n', lines);
    }

    private static RectD ToRectD(Rect rect) => new(rect.X, rect.Y, rect.Width, rect.Height);
    private static double SmoothStep(double value)
    {
        value = Math.Clamp(value, 0, 1);
        return value * value * (3 - 2 * value);
    }
}
