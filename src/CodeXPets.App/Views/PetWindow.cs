using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Platform;
using CodeXPets.App.Infrastructure;
using CodeXPets.App.Services;
using CodeXPets.Core.Application;
using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;

namespace CodeXPets.App.Views;

public sealed class PetWindow : Window
{
    public const double LogicalWidth = 420;
    public const double LogicalHeight = 260;
    private const double DockSlideInSeconds = 0.30;
    private const double DockSlideOutSeconds = 0.55;

    private readonly IPlatformService _platform;
    private readonly PetSurface _surface;
    private AppSettings _settings;
    private DockEdge _dockEdge;
    private int _dockCoordinate;
    private string _dockScreenIdentifier = string.Empty;
    private DateTimeOffset _dockLastContentChange = DateTimeOffset.Now;
    private DateTimeOffset _dockHoverRevealUntil = DateTimeOffset.MinValue;
    private bool _dragPending;
    private bool _dragging;
    private bool _dragStartedDocked;
    private PixelPoint _dragStartCursor;
    private PixelPoint _dragStartPosition;
    private PixelPoint _lastCursor;
    private string _lastStatusSignature = string.Empty;
    private bool _positionRestored;
    private bool? _lastMouseReception;
    private IPointer? _capturedPointer;
    private int _clickRegionSignature;
    private DateTime _lastRegionUpdateUtc = DateTime.MinValue;

    public PetWindow(ResourceCatalog resources, IPlatformService platform, AppSettings settings)
    {
        _platform = platform;
        _settings = settings;
        Width = LogicalWidth;
        Height = LogicalHeight;
        MinWidth = LogicalWidth;
        MaxWidth = LogicalWidth;
        MinHeight = LogicalHeight;
        MaxHeight = LogicalHeight;
        CanResize = false;
        WindowDecorations = WindowDecorations.None;
        TransparencyLevelHint = [WindowTransparencyLevel.Transparent];
        Background = Brushes.Transparent;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;
        Title = "CodeXPets";
        ExtendClientAreaToDecorationsHint = true;
        _surface = new PetSurface(resources);
        Content = _surface;

        _surface.PointerPressed += OnPointerPressed;
        _surface.PointerMoved += OnPointerMoved;
        _surface.PointerReleased += OnPointerReleased;
        _surface.PointerCaptureLost += OnPointerCaptureLost;
        Opened += OnOpened;
        PositionChanged += (_, _) => UpdateMirrorDirection();
        Screens.Changed += (_, _) => OnScreensChanged();
    }

    public bool IsDocked => _dockEdge != DockEdge.None;
    public bool IsDragActive => _dragPending || _dragging;
    public event EventHandler<PetPositionState>? PositionStateChanged;

    public void SetPetContextMenu(ContextMenu menu) => _surface.ContextMenu = menu;

    public void ShowInactive()
    {
        if (IsVisible == false)
        {
            _lastMouseReception = null;
            Show();
        }
        Topmost = true;
        if (_positionRestored)
        {
            RefreshClickRegion();
            UpdateMousePassThrough();
        }
    }

    public void ApplySettings(AppSettings settings)
    {
        _settings = settings;
        if (IsDocked) PositionDockedWindow();
        _surface.InvalidateVisual();
    }

    public void UpdateStatus(string status, string thought, ReminderState state,
        IReadOnlyList<string> titles, IReadOnlyList<string?>? progress,
        bool selectNewestTask, int preferredTaskIndex)
    {
        var signature = string.Join('\u001f', new[]
        {
            status, thought, state.ToString(), string.Join('\u001e', titles),
            progress is null ? string.Empty : string.Join('\u001e', progress.Select(item => item ?? string.Empty))
        });
        var changed = !string.Equals(signature, _lastStatusSignature, StringComparison.Ordinal);
        if (!changed && !selectNewestTask) return;
        _lastStatusSignature = signature;
        if (IsDocked && state != ReminderState.Idle)
        {
            var now = DateTimeOffset.Now;
            _dockLastContentChange = now;
            _surface.DockThoughtUntil = now.AddSeconds(
                AppLogic.CloudNotificationSeconds(state, _settings.DockNotificationSeconds));
            _surface.DockVisibility = 1;
        }
        _surface.UpdateStatus(status, thought, state, titles, progress,
            selectNewestTask, preferredTaskIndex);
    }

    public bool Animate(int tick, double elapsedSeconds)
    {
        if (!IsVisible) return false;

        var needsCursor = IsDocked || _platform.RequiresMousePassThroughPolling;
        var cursor = needsCursor ? _platform.GetCursorPosition() : default;
        var hovering = IsDocked && IsDockHovering(cursor);
        var dockChanged = false;
        if (IsDocked)
        {
            var now = DateTimeOffset.Now;
            if (hovering)
                _dockHoverRevealUntil = now.AddSeconds(_settings.DockRevealSeconds);
            var show = AppLogic.ShouldShowDock(_dockLastContentChange, now,
                IsDragActive, hovering, _dockHoverRevealUntil, _settings.DockIdleHideSeconds);
            var before = _surface.DockVisibility;
            if (show)
                _surface.DockVisibility = Math.Min(1, before + elapsedSeconds / DockSlideInSeconds);
            else
                _surface.DockVisibility = Math.Max(0, before - elapsedSeconds / DockSlideOutSeconds);
            dockChanged = Math.Abs(before - _surface.DockVisibility) > 0.0001;
        }
        else if (_surface.DockVisibility < 1)
        {
            _surface.DockVisibility = 1;
            dockChanged = true;
        }

        var animated = _surface.Animate(tick, elapsedSeconds);
        RefreshClickRegion();
        if (_platform.RequiresMousePassThroughPolling)
            UpdateMousePassThrough(cursor, hovering);
        if (dockChanged && !animated) _surface.InvalidateVisual();
        var dockTransitioning = IsDocked && _surface.DockVisibility > 0.0001 &&
            _surface.DockVisibility < 0.9999;
        return dockChanged || dockTransitioning || _surface.NeedsSmoothAnimation || IsDragActive;
    }

    public void SaveCurrentPosition()
    {
        if (!_positionRestored || Screens.Primary is null) return;
        var referencePoint = IsDocked ? GetWindowCenter() : GetFloatingAnchor();
        var screen = FindDockScreen() ?? Screens.ScreenFromPoint(referencePoint) ?? Screens.Primary;
        if (screen is null) return;
        var work = screen.WorkingArea;
        PetPositionState state;
        if (IsDocked)
        {
            var relativeY = work.Height <= 0 ? 0.5 : (_dockCoordinate - work.Y) / (double)work.Height;
            state = new PetPositionState(_dockEdge, ScreenIdentifier(screen),
                _dockEdge == DockEdge.Left ? 0 : 1, relativeY).Normalize();
        }
        else
        {
            var anchor = GetFloatingAnchor();
            var relativeX = work.Width <= 0 ? 0.5 : (anchor.X - work.X) / (double)work.Width;
            var relativeY = work.Height <= 0 ? 0.5 : (anchor.Y - work.Y) / (double)work.Height;
            state = new PetPositionState(DockEdge.None, ScreenIdentifier(screen),
                relativeX, relativeY).Normalize();
        }
        PositionStateChanged?.Invoke(this, state);
    }

    private void OnOpened(object? sender, EventArgs eventArgs)
    {
        if (_positionRestored) return;
        _positionRestored = true;
        PlaceAtDefaultLocation();
        RestoreSavedPosition();
        _platform.ConfigurePetWindow(this, _surface.IsInteractivePoint);
        RefreshClickRegion();
        UpdateMousePassThrough();
    }

    private void OnPointerPressed(object? sender, PointerPressedEventArgs eventArgs)
    {
        var point = eventArgs.GetPosition(_surface);
        var properties = eventArgs.GetCurrentPoint(_surface).Properties;
        if (properties.PointerUpdateKind == PointerUpdateKind.RightButtonPressed)
        {
            return;
        }
        if (properties.PointerUpdateKind != PointerUpdateKind.LeftButtonPressed) return;
        // Reject transparent pixels before any task-switch or drag handling so
        // everything except the cat and cloud passes through to the application below.
        if (!_surface.IsInteractivePoint(point)) return;
        if (_surface.IsTaskSwitchPoint(point))
        {
            if (_surface.SwitchToNextTask()) RevealDockForInteraction();
            eventArgs.Handled = true;
            return;
        }

        _dragStartedDocked = IsDocked;
        if (_dragStartedDocked)
        {
            _surface.DockVisibility = 1;
            _surface.InvalidateVisual();
        }
        _dragPending = true;
        _dragging = false;
        _dragStartCursor = _platform.GetCursorPosition();
        _lastCursor = _dragStartCursor;
        _dragStartPosition = Position;
        _capturedPointer = eventArgs.Pointer;
        _capturedPointer.Capture(_surface);
        eventArgs.Handled = true;
    }

    private void OnPointerMoved(object? sender, PointerEventArgs eventArgs)
    {
        if (!IsDragActive) return;
        var cursor = _platform.GetCursorPosition();
        _lastCursor = cursor;
        var dx = cursor.X - _dragStartCursor.X;
        var dy = cursor.Y - _dragStartCursor.Y;
        var threshold = Math.Max(3, (int)Math.Round(4 * RenderScaling));
        if (!_dragging && Math.Abs(dx) + Math.Abs(dy) < threshold) return;
        if (!_dragging && IsDocked)
        {
            UndockForDrag(cursor);
            dx = 0;
            dy = 0;
        }
        _dragging = true;
        Position = new PixelPoint(_dragStartPosition.X + dx, _dragStartPosition.Y + dy);
        UpdateMirrorDirection();
        eventArgs.Handled = true;
    }

    private void OnPointerReleased(object? sender, PointerReleasedEventArgs eventArgs)
    {
        if (eventArgs.InitialPressMouseButton == MouseButton.Left) FinishDrag();
    }

    private void OnPointerCaptureLost(object? sender, PointerCaptureLostEventArgs eventArgs)
    {
        if (IsDragActive) FinishDrag();
    }

    private void FinishDrag()
    {
        if (!IsDragActive) return;
        var moved = _dragging;
        var startedDocked = _dragStartedDocked;
        _dragging = false;
        _dragPending = false;
        _dragStartedDocked = false;
        try { _capturedPointer?.Capture(null); } catch { }
        _capturedPointer = null;
        if (!moved)
        {
            if (startedDocked && IsDocked) RevealDockForInteraction();
            return;
        }
        if (!TrySnapToEdge(_lastCursor)) ClampToWorkingArea();
        SaveCurrentPosition();
    }

    private void RevealDockForInteraction()
    {
        if (!IsDocked) return;
        var now = DateTimeOffset.Now;
        _surface.DockVisibility = 1;
        _dockLastContentChange = now;
        _surface.DockThoughtUntil = now.AddSeconds(
            AppLogic.CloudNotificationSeconds(_surface.State, _settings.DockNotificationSeconds));
        _dockHoverRevealUntil = now.AddSeconds(_settings.DockRevealSeconds);
        _surface.InvalidateVisual();
    }

    private bool TrySnapToEdge(PixelPoint cursor)
    {
        var screen = Screens.ScreenFromPoint(cursor) ?? Screens.Primary;
        if (screen is null) return false;
        var work = screen.WorkingArea;
        var distance = Math.Max(24, Math.Round(36 * screen.Scaling));
        var edge = AppLogic.SelectSnapEdge(new PointD(cursor.X, cursor.Y),
            new RectD(work.X, work.Y, work.Width, work.Height), distance);
        if (edge == DockEdge.None) return false;
        _dockEdge = edge;
        _dockScreenIdentifier = ScreenIdentifier(screen);
        _dockCoordinate = cursor.Y;
        _surface.IsDocked = true;
        _surface.DockEdge = edge;
        _surface.DockVisibility = 1;
        var now = DateTimeOffset.Now;
        _dockLastContentChange = now;
        _surface.DockThoughtUntil = _surface.State == ReminderState.Idle
            ? DateTimeOffset.MinValue
            : now.AddSeconds(AppLogic.CloudNotificationSeconds(
                _surface.State, _settings.DockNotificationSeconds));
        _dockHoverRevealUntil = DateTimeOffset.MinValue;
        PositionDockedWindow();
        _surface.InvalidateVisual();
        return true;
    }

    private void UndockForDrag(PixelPoint cursor)
    {
        _dockEdge = DockEdge.None;
        _dockScreenIdentifier = string.Empty;
        _surface.IsDocked = false;
        _surface.DockEdge = DockEdge.None;
        _surface.DockBubbleBelow = false;
        _surface.DockVisibility = 1;
        _surface.DockThoughtUntil = DateTimeOffset.MinValue;
        _dockHoverRevealUntil = DateTimeOffset.MinValue;
        var width = GetWindowPixelWidth();
        var height = GetWindowPixelHeight();
        Position = new PixelPoint(cursor.X - width / 2,
            cursor.Y - height + Math.Max(24, (int)Math.Round(70 * RenderScaling)));
        _dragStartCursor = cursor;
        _dragStartPosition = Position;
        _surface.InvalidateVisual();
    }

    private void PositionDockedWindow()
    {
        if (!IsDocked) return;
        var screen = FindDockScreen() ?? Screens.ScreenFromPoint(GetWindowCenter()) ?? Screens.Primary;
        if (screen is null) return;
        _dockScreenIdentifier = ScreenIdentifier(screen);
        var work = screen.WorkingArea;
        var width = GetWindowPixelWidth(screen.Scaling);
        var height = GetWindowPixelHeight(screen.Scaling);
        _surface.IsDocked = true;
        _surface.DockEdge = _dockEdge;
        _surface.DockBubbleBelow = false;
        var visible = _surface.VisiblePetBounds;
        var visibleCenterOffset = (visible.Y + visible.Height / 2d) * screen.Scaling;
        var y = _dockCoordinate - (int)Math.Round(visibleCenterOffset);
        if (y < work.Y)
        {
            _surface.DockBubbleBelow = true;
            visible = _surface.VisiblePetBounds;
            visibleCenterOffset = (visible.Y + visible.Height / 2d) * screen.Scaling;
            y = _dockCoordinate - (int)Math.Round(visibleCenterOffset);
        }
        var x = _dockEdge == DockEdge.Left ? work.X : work.Right - width;
        y = Math.Clamp(y, work.Y, Math.Max(work.Y, work.Bottom - height));
        Position = new PixelPoint(x, y);
        _surface.InvalidateVisual();
    }

    private void ClampToWorkingArea()
    {
        if (IsDocked)
        {
            PositionDockedWindow();
            return;
        }
        var screen = Screens.ScreenFromPoint(GetWindowCenter()) ?? Screens.Primary;
        if (screen is null) return;
        var work = screen.WorkingArea;
        var width = GetWindowPixelWidth(screen.Scaling);
        var height = GetWindowPixelHeight(screen.Scaling);
        Position = new PixelPoint(
            Math.Clamp(Position.X, work.X, Math.Max(work.X, work.Right - width)),
            Math.Clamp(Position.Y, work.Y, Math.Max(work.Y, work.Bottom - height)));
    }

    private void PlaceAtDefaultLocation()
    {
        var screen = Screens.Primary;
        if (screen is null) return;
        var work = screen.WorkingArea;
        var width = GetWindowPixelWidth(screen.Scaling);
        var height = GetWindowPixelHeight(screen.Scaling);
        Position = new PixelPoint(Math.Max(work.X, work.Right - width - 24),
            Math.Max(work.Y, work.Bottom - height - 24));
    }

    private void RestoreSavedPosition()
    {
        var saved = _settings.PetPosition?.Normalize();
        if (saved is null) return;
        var screen = FindScreen(saved.ScreenIdentifier) ?? Screens.Primary;
        if (screen is null) return;
        var work = screen.WorkingArea;
        if (saved.DockEdge != DockEdge.None)
        {
            _dockEdge = saved.DockEdge;
            _dockScreenIdentifier = ScreenIdentifier(screen);
            _dockCoordinate = work.Y + (int)Math.Round(saved.RelativeY * work.Height);
            _surface.IsDocked = true;
            _surface.DockEdge = _dockEdge;
            PositionDockedWindow();
        }
        else
        {
            _dockEdge = DockEdge.None;
            _surface.IsDocked = false;
            var width = GetWindowPixelWidth(screen.Scaling);
            var height = GetWindowPixelHeight(screen.Scaling);
            var x = work.X + (int)Math.Round(saved.RelativeX * work.Width) - width / 2;
            var y = work.Y + (int)Math.Round(saved.RelativeY * work.Height) - height;
            Position = new PixelPoint(Math.Clamp(x, work.X, Math.Max(work.X, work.Right - width)),
                Math.Clamp(y, work.Y, Math.Max(work.Y, work.Bottom - height)));
        }
        UpdateMirrorDirection();
    }

    private void OnScreensChanged()
    {
        if (_positionRestored) ClampToWorkingArea();
        UpdateMirrorDirection();
    }

    private void UpdateMirrorDirection()
    {
        if (IsDocked)
        {
            _surface.MirrorFloatingSprite = false;
            return;
        }
        var center = GetWindowCenter();
        var screen = Screens.ScreenFromPoint(center) ?? Screens.Primary;
        if (screen is null) return;
        var work = screen.WorkingArea;
        var next = AppLogic.ShouldMirrorFloatingSprite(new PointD(center.X, center.Y),
            new RectD(work.X, work.Y, work.Width, work.Height));
        if (_surface.MirrorFloatingSprite != next)
        {
            _surface.MirrorFloatingSprite = next;
            _surface.InvalidateVisual();
        }
    }

    private bool IsDockHovering(PixelPoint cursor)
    {
        if (!IsDocked) return false;
        var screen = FindDockScreen() ?? Screens.ScreenFromPoint(GetWindowCenter()) ?? Screens.Primary;
        if (screen is null) return false;
        var work = screen.WorkingArea;
        var hover = AppLogic.DockHoverBounds(_dockEdge,
            new RectD(work.X, work.Y, work.Width, work.Height), _dockCoordinate,
            screen.Scaling, _surface.DockVisibility <= 0.01, _settings.DockHoverHeight);
        if (hover.Contains(new PointD(cursor.X, cursor.Y))) return true;
        var pet = _surface.VisiblePetBounds;
        var scale = screen.Scaling;
        var globalPet = new RectD(Position.X + pet.X * scale, Position.Y + pet.Y * scale,
            pet.Width * scale, pet.Height * scale);
        return globalPet.Contains(new PointD(cursor.X, cursor.Y));
    }

    private static readonly TimeSpan RegionUpdateMinimumInterval = TimeSpan.FromMilliseconds(200);

    private void RefreshClickRegion()
    {
        if (!IsVisible) return;
        var now = DateTime.UtcNow;
        if (now - _lastRegionUpdateUtc < RegionUpdateMinimumInterval) return;
        var signature = _surface.GetClickRegionSignature();
        if (signature == _clickRegionSignature) return;
        _clickRegionSignature = signature;
        _lastRegionUpdateUtc = now;
        var rects = _surface.BuildClickRegionRects();
        _platform.UpdatePetWindowClickRegion(this, rects);
    }

    private void UpdateMousePassThrough()
    {
        if (!_platform.RequiresMousePassThroughPolling || !IsVisible) return;
        var cursor = _platform.GetCursorPosition();
        UpdateMousePassThrough(cursor, IsDocked && IsDockHovering(cursor));
    }

    private void UpdateMousePassThrough(PixelPoint cursor, bool dockHovering)
    {
        if (!_platform.RequiresMousePassThroughPolling || !IsVisible) return;
        var scaling = Math.Max(0.1, RenderScaling);
        var local = new Point((cursor.X - Position.X) / scaling, (cursor.Y - Position.Y) / scaling);
        var receiveMouse = IsDragActive || dockHovering || _surface.IsInteractivePoint(local);
        if (_lastMouseReception == receiveMouse) return;
        _lastMouseReception = receiveMouse;
        _platform.UpdatePetWindowMousePassThrough(this, receiveMouse);
    }

    private Screen? FindDockScreen() => FindScreen(_dockScreenIdentifier);

    private Screen? FindScreen(string? identifier)
    {
        if (string.IsNullOrWhiteSpace(identifier)) return null;
        var exact = Screens.All.FirstOrDefault(screen =>
            string.Equals(ScreenIdentifier(screen), identifier, StringComparison.Ordinal));
        if (exact is not null) return exact;

        var separator = identifier.IndexOf('|');
        var displayName = separator >= 0 ? identifier[..separator] : identifier;
        return Screens.All.FirstOrDefault(screen =>
            string.Equals(screen.DisplayName, displayName, StringComparison.Ordinal));
    }

    private static string ScreenIdentifier(Screen screen) =>
        $"{screen.DisplayName}|{screen.Bounds.X},{screen.Bounds.Y},{screen.Bounds.Width},{screen.Bounds.Height}";

    private PixelPoint GetWindowCenter() => new(Position.X + GetWindowPixelWidth() / 2,
        Position.Y + GetWindowPixelHeight() / 2);
    private PixelPoint GetFloatingAnchor() => new(Position.X + GetWindowPixelWidth() / 2,
        Position.Y + GetWindowPixelHeight());
    private int GetWindowPixelWidth(double? scaling = null) =>
        Math.Max(1, (int)Math.Round(LogicalWidth * (scaling ?? Math.Max(1, RenderScaling))));
    private int GetWindowPixelHeight(double? scaling = null) =>
        Math.Max(1, (int)Math.Round(LogicalHeight * (scaling ?? Math.Max(1, RenderScaling))));
}
