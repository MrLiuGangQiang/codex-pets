using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Threading;
using CodeXPets.App.Infrastructure;
using CodeXPets.App.Services;
using CodeXPets.App.Views;
using CodeXPets.Core.Application;
using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace CodeXPets.App;

public sealed class AppController : IDisposable
{
    private const double SpriteFrameSeconds = 0.12;
    private static readonly TimeSpan SmoothAnimationInterval = TimeSpan.FromMilliseconds(50);
    private static readonly TimeSpan SpriteAnimationInterval = TimeSpan.FromMilliseconds(120);
    private static readonly TimeSpan HiddenOverlayInterval = TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan DormantInterval = TimeSpan.FromSeconds(1);
    private static readonly Uri LatestReleaseUri =
        new("https://github.com/MrLiuGangQiang/codex-pets/releases/latest");

    private readonly IClassicDesktopStyleApplicationLifetime _lifetime;
    private readonly IPlatformService _platform;
    private readonly ISettingsStore _settingsStore;
    private readonly ResourceCatalog _resources;
    private readonly SoundService _sound = new();
    private readonly VisualStateCoordinator _visualCoordinator = new();
    private readonly PetWindow _petWindow;
    private readonly TrayIcon _trayIcon;
    private readonly DispatcherTimer _animationTimer;
    private readonly Stopwatch _animationClock = Stopwatch.StartNew();

    private AppSettings _settings;
    private MonitorWorker _monitorWorker;
    private MonitorSnapshot _snapshot = MonitorSnapshot.Empty;
    private SettingsWindow? _settingsWindow;
    private DiagnosticsWindow? _diagnosticsWindow;
    private NativeMenuItem _nativeStatusItem = null!;
    private NativeMenuItem _nativeAssistantItem = null!;
    private NativeMenuItem _nativeSoundItem = null!;
    private NativeMenuItem _nativeStartupItem = null!;
    private MenuItem _contextStatusItem = null!;
    private MenuItem _contextAssistantItem = null!;
    private MenuItem _contextSoundItem = null!;
    private MenuItem _contextStartupItem = null!;
    private ReminderState? _lastVisualState;
    private long _lastAnimationTimestamp;
    private double _spriteAccumulator;
    private int _animationFrame;
    private int _petAnimationTick;
    private string _lastStatusText = string.Empty;
    private ReminderState? _lastTrayState;
    private int _lastTrayFrame = -1;
    private bool _hasMonitorSnapshot;
    private bool _disposed;

    public AppController(Application application, IClassicDesktopStyleApplicationLifetime lifetime)
    {
        ArgumentNullException.ThrowIfNull(application);
        ArgumentNullException.ThrowIfNull(lifetime);

        _lifetime = lifetime;
        _platform = PlatformServiceFactory.Create();
        _settingsStore = new JsonSettingsStore();
        _settings = LegacySettingsMigrator.LoadOrMigrate(_settingsStore);
        _resources = new ResourceCatalog();
        _petWindow = new PetWindow(_resources, _platform, _settings);
        _petWindow.PositionStateChanged += OnPetPositionChanged;

        _trayIcon = new TrayIcon
        {
            Icon = _resources.IdleIcon,
            ToolTipText = "正在检查 Codex",
            Menu = BuildNativeMenu(),
            IsVisible = true
        };
        TrayIcon.SetIcons(application, new TrayIcons { _trayIcon });
        _petWindow.SetPetContextMenu(BuildContextMenu());
        if (_settings.PetVisible)
        {
            _petWindow.ShowInactive();
        }

        _monitorWorker = CreateMonitorWorker(_settings.SessionsRoot);
        _animationTimer = new DispatcherTimer { Interval = SpriteAnimationInterval };
        _animationTimer.Tick += OnAnimationTick;
        _animationTimer.Start();
        RefreshVisual(forceText: true);
    }

    private NativeMenu BuildNativeMenu()
    {
        var menu = new NativeMenu();
        _nativeStatusItem = new NativeMenuItem("状态：正在检查 Codex…") { IsEnabled = false };
        _nativeAssistantItem = CreateNativeToggle("显示桌面宠物", _settings.PetVisible, ToggleAssistant);
        _nativeSoundItem = CreateNativeToggle("播放语音提醒", _settings.SoundEnabled, ToggleSound);
        _nativeStartupItem = CreateNativeToggle("开机自动运行", SafeIsAutoStartEnabled(), ToggleStartup);
        menu.Add(_nativeStatusItem);
        menu.Add(new NativeMenuItemSeparator());
        menu.Add(_nativeAssistantItem);
        menu.Add(_nativeSoundItem);
        menu.Add(_nativeStartupItem);
        menu.Add(CreateNativeItem("打开 Codex 会话目录", OpenSessionsFolder));
        menu.Add(CreateNativeItem("设置…", ShowSettings));
        menu.Add(CreateNativeItem("诊断信息…", ShowDiagnostics));
        menu.Add(CreateNativeItem("查看更新…", OpenLatestRelease));
        menu.Add(new NativeMenuItemSeparator());
        menu.Add(CreateNativeItem("退出", Shutdown));
        menu.NeedsUpdate += (_, _) => UpdateMenuChecks();
        return menu;
    }

    private ContextMenu BuildContextMenu()
    {
        _contextStatusItem = new MenuItem { Header = "状态：正在检查 Codex…", IsEnabled = false };
        _contextAssistantItem = CreateContextToggle("显示桌面宠物", _settings.PetVisible, ToggleAssistant);
        _contextSoundItem = CreateContextToggle("播放语音提醒", _settings.SoundEnabled, ToggleSound);
        _contextStartupItem = CreateContextToggle("开机自动运行", SafeIsAutoStartEnabled(), ToggleStartup);
        var menu = new ContextMenu
        {
            Items =
            {
                _contextStatusItem,
                new Separator(),
                _contextAssistantItem,
                _contextSoundItem,
                _contextStartupItem,
                CreateContextItem("打开 Codex 会话目录", OpenSessionsFolder),
                CreateContextItem("设置…", ShowSettings),
                CreateContextItem("诊断信息…", ShowDiagnostics),
                CreateContextItem("查看更新…", OpenLatestRelease),
                new Separator(),
                CreateContextItem("退出", Shutdown)
            }
        };
        menu.Opened += (_, _) => UpdateMenuChecks();
        return menu;
    }

    private MonitorWorker CreateMonitorWorker(string sessionsRoot)
    {
        var worker = new MonitorWorker(sessionsRoot);
        worker.Updated += OnMonitorUpdated;
        worker.Start();
        return worker;
    }

    private void OnMonitorUpdated(object? sender, MonitorUpdate update)
    {
        var source = sender;
        Dispatcher.UIThread.Post(() =>
        {
            if (!ReferenceEquals(source, _monitorWorker)) return;
            HandleMonitorUpdate(update);
        });
    }

    private void HandleMonitorUpdate(MonitorUpdate update)
    {
        if (_disposed) return;

        var firstSnapshot = !_hasMonitorSnapshot;
        _hasMonitorSnapshot = true;
        _snapshot = update.Snapshot;
        if (update.Events.Count == 0 && !firstSnapshot) return;

        foreach (var monitorEvent in update.Events)
        {
            if (monitorEvent != MonitorEventKind.StateChanged)
            {
                ApplyMonitorEvent(monitorEvent);
            }
        }

        if (_snapshot.ActiveCount > 0 &&
            update.Events.Contains(MonitorEventKind.StateChanged) &&
            string.Equals(_snapshot.LastEventType, "update_plan", StringComparison.Ordinal))
        {
            _visualCoordinator.RecordStarted();
        }

        RefreshVisual(forceText: firstSnapshot || update.Events.Count > 0);
    }

    private void ApplyMonitorEvent(MonitorEventKind monitorEvent)
    {
        var now = DateTimeOffset.Now;
        switch (monitorEvent)
        {
            case MonitorEventKind.TaskStarted:
                _visualCoordinator.RecordStarted();
                RevealAssistant();
                if (_settings.SoundEnabled)
                {
                    _sound.Play(NotificationSound.Started);
                }
                break;

            case MonitorEventKind.TaskCompleted:
                _visualCoordinator.RecordCompleted(
                    now,
                    TimeSpan.FromSeconds(AppLogic.CloudNotificationSeconds(
                        ReminderState.Completed,
                        _settings.DockNotificationSeconds)));
                RevealAssistant();
                if (_settings.SoundEnabled)
                {
                    _sound.Play(NotificationSound.Completed);
                }
                break;

            case MonitorEventKind.TaskAborted:
                _visualCoordinator.RecordAborted(
                    now,
                    TimeSpan.FromSeconds(AppLogic.CloudNotificationSeconds(
                        ReminderState.Error,
                        _settings.DockNotificationSeconds)));
                RevealAssistant();
                if (_settings.SoundEnabled)
                {
                    _sound.Play(NotificationSound.Error);
                }
                break;

        }
    }
    private void OnAnimationTick(object? sender, EventArgs eventArgs)
    {
        var timestamp = _animationClock.ElapsedTicks;
        var elapsed = _lastAnimationTimestamp == 0
            ? SpriteAnimationInterval.TotalSeconds
            : (timestamp - _lastAnimationTimestamp) / (double)Stopwatch.Frequency;
        _lastAnimationTimestamp = timestamp;
        elapsed = Math.Clamp(elapsed, 0.001, 0.250);
        _spriteAccumulator += elapsed;

        var spriteFrameChanged = false;
        while (_spriteAccumulator >= SpriteFrameSeconds)
        {
            _spriteAccumulator -= SpriteFrameSeconds;
            _animationFrame = (_animationFrame + 1) % _resources.BusyIcons.Count;
            _petAnimationTick = (_petAnimationTick + 1) % 6400;
            spriteFrameChanged = true;
        }

        var needsSmoothAnimation = _petWindow.Animate(_petAnimationTick, elapsed);
        if (spriteFrameChanged && _lastVisualState == ReminderState.Busy)
        {
            UpdateTrayIcon(ReminderState.Busy);
        }

        if (_lastVisualState is ReminderState.Completed or ReminderState.Error)
        {
            var nextState = _visualCoordinator.Select(_snapshot.ActiveCount, DateTimeOffset.Now);
            if (nextState != _lastVisualState)
            {
                RefreshVisual(forceText: true);
            }
        }

        UpdateAnimationTimerInterval(needsSmoothAnimation);
    }

    private void RefreshVisual(bool forceText)
    {
        var visualState = _visualCoordinator.Select(_snapshot.ActiveCount, DateTimeOffset.Now);
        if (_lastVisualState != visualState)
        {
            _petAnimationTick = 0;
            _animationFrame = 0;
            _spriteAccumulator = 0;
            _lastVisualState = visualState;
        }
        UpdateTrayIcon(visualState);

        var busyProgress = _snapshot.TotalPlanStepCount > 0
            ? $"{_snapshot.CompletedPlanStepCount}/{_snapshot.TotalPlanStepCount}"
            : null;
        var stateText = visualState switch
        {
            ReminderState.Error => "异常",
            ReminderState.Completed => "已完成",
            ReminderState.Busy => AppLogic.FormatBusyHeader(
                busyProgress,
                _snapshot.LatestEventActiveTitleIndex,
                _snapshot.ActiveCount),
            _ => "空闲"
        };
        var primaryActiveTitle = _snapshot.ActiveTitles.FirstOrDefault();
        var thoughtText = visualState switch
        {
            ReminderState.Error => "任务出现异常了。",
            ReminderState.Completed => "任务完成啦！",
            ReminderState.Busy => string.IsNullOrWhiteSpace(primaryActiveTitle)
                ? "正在认真处理你的任务…"
                : primaryActiveTitle,
            _ => "主人，现在没有在进行中的任务!别让我歇着!"
        };

        IReadOnlyList<string> titles;
        IReadOnlyList<string?>? progress = null;
        if (visualState == ReminderState.Error)
        {
            titles = [AppLogic.FormatAbnormalTaskText(_snapshot.LastAbortedTitle)];
        }
        else if (visualState == ReminderState.Completed &&
                 !string.IsNullOrWhiteSpace(_snapshot.LastCompletedTitle))
        {
            titles = [_snapshot.LastCompletedTitle];
        }
        else
        {
            titles = _snapshot.ActiveTitles;
            if (_snapshot.ActiveCount > 0)
            {
                progress = _snapshot.ActivePlanProgressLabels;
            }
        }

        var selectNewest = _visualCoordinator.ShowNewestTaskOnNextRefresh &&
                           visualState == ReminderState.Busy;
        var preferred = AppLogic.SelectPreferredTaskIndex(
            selectNewest,
            _snapshot.LatestEventActiveTitleIndex);
        _petWindow.UpdateStatus(
            stateText,
            thoughtText,
            visualState,
            titles,
            progress,
            selectNewest,
            preferred);
        if (selectNewest)
        {
            _visualCoordinator.ConsumeNewestTaskFocus();
        }

        if (forceText || !string.Equals(_lastStatusText, stateText, StringComparison.Ordinal))
        {
            _lastStatusText = stateText;
            _nativeStatusItem.Header = "状态：" + stateText;
            _contextStatusItem.Header = "状态：" + stateText;
            _trayIcon.ToolTipText = "CodeXPets · " + stateText;
        }

        UpdateAnimationTimerInterval(needsSmoothAnimation: false);
    }

    private void UpdateTrayIcon(ReminderState visualState)
    {
        var frame = visualState == ReminderState.Busy ? _animationFrame : -1;
        if (_lastTrayState == visualState && _lastTrayFrame == frame) return;

        _lastTrayState = visualState;
        _lastTrayFrame = frame;
        _trayIcon.Icon = visualState switch
        {
            ReminderState.Error => _resources.ErrorIcon,
            ReminderState.Completed => _resources.CompletedIcon,
            ReminderState.Busy => _resources.BusyIcons[_animationFrame],
            _ => _resources.IdleIcon
        };
    }

    private void UpdateAnimationTimerInterval(bool needsSmoothAnimation)
    {
        var next = needsSmoothAnimation
            ? SmoothAnimationInterval
            : _petWindow.IsVisible || _lastVisualState == ReminderState.Busy
                ? SpriteAnimationInterval
                : _lastVisualState is ReminderState.Completed or ReminderState.Error
                    ? HiddenOverlayInterval
                    : DormantInterval;
        if (_animationTimer.Interval != next)
        {
            _animationTimer.Interval = next;
        }
    }

    private void ToggleAssistant()
    {
        _settings.PetVisible = !_settings.PetVisible;
        if (_settings.PetVisible)
        {
            _petWindow.ShowInactive();
        }
        else
        {
            _petWindow.Hide();
        }
        UpdateAnimationTimerInterval(needsSmoothAnimation: false);
        SaveSettings();
        UpdateMenuChecks();
    }

    private void ToggleSound()
    {
        _settings.SoundEnabled = !_settings.SoundEnabled;
        SaveSettings();
        UpdateMenuChecks();
    }

    private void ToggleStartup()
    {
        try
        {
            _platform.SetAutoStartEnabled(!SafeIsAutoStartEnabled());
        }
        catch (Exception exception)
        {
            ShowMessage("设置开机启动失败", exception.Message);
        }
        UpdateMenuChecks();
    }

    private void OpenSessionsFolder()
    {
        if (!Directory.Exists(_settings.SessionsRoot))
        {
            ShowMessage("尚未找到 Codex 会话目录", _settings.SessionsRoot);
            return;
        }

        try
        {
            _platform.OpenFolder(_settings.SessionsRoot);
        }
        catch (Exception exception)
        {
            ShowMessage("无法打开会话目录", exception.Message);
        }
    }

    private void ShowSettings()
    {
        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        _settingsWindow = new SettingsWindow(_settings.Clone());
        _settingsWindow.SettingsSaved += (_, next) => ApplySettings(next);
        _settingsWindow.Closed += (_, _) => _settingsWindow = null;
        _settingsWindow.Show();
    }

    private void ApplySettings(AppSettings next)
    {
        var previousRoot = _settings.SessionsRoot;
        next.PetPosition = _settings.PetPosition;
        next.PetVisible = _settings.PetVisible;
        _settings.CopyFrom(next);
        SaveSettings();
        _petWindow.ApplySettings(_settings);

        var pathComparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        if (!string.Equals(previousRoot, _settings.SessionsRoot, pathComparison))
        {
            _monitorWorker.Updated -= OnMonitorUpdated;
            _monitorWorker.Dispose();
            _snapshot = MonitorSnapshot.Empty;
            _hasMonitorSnapshot = false;
            _monitorWorker = CreateMonitorWorker(_settings.SessionsRoot);
        }

        RefreshVisual(forceText: true);
        UpdateMenuChecks();
    }

    private void ShowDiagnostics()
    {
        if (_diagnosticsWindow is { IsVisible: true })
        {
            _diagnosticsWindow.Activate();
            return;
        }

        _diagnosticsWindow = new DiagnosticsWindow(BuildDiagnosticsText, OpenSessionsFolder);
        _diagnosticsWindow.Closed += (_, _) => _diagnosticsWindow = null;
        _diagnosticsWindow.Show();
    }

    private string BuildDiagnosticsText()
    {
        var assembly = Assembly.GetExecutingAssembly().GetName();
        var builder = new StringBuilder();
        builder.AppendLine($"CodeXPets v{assembly.Version?.ToString(3) ?? "未知"}");
        builder.AppendLine($"平台：{_platform.PlatformName}");
        builder.AppendLine($"进程架构：{_platform.RuntimeArchitecture}");
        builder.AppendLine($"系统架构：{RuntimeInformation.OSArchitecture}");
        builder.AppendLine($"运行时：{RuntimeInformation.FrameworkDescription}");
        builder.AppendLine($"操作系统：{RuntimeInformation.OSDescription}");
        builder.AppendLine($"可执行文件：{Environment.ProcessPath ?? "未知"}");
        builder.AppendLine($"设置文件：{_settingsStore.SettingsFilePath}");
        builder.AppendLine($"Codex Home：{CodexPaths.GetDefaultHome()}");
        builder.AppendLine($"Codex 配置：{CodexPaths.GetDefaultConfigFile()}");
        builder.AppendLine($"会话目录：{_settings.SessionsRoot}");
        builder.AppendLine($"会话目录存在：{(Directory.Exists(_settings.SessionsRoot) ? "是" : "否")}");
        builder.AppendLine($"桌宠：{(_settings.PetVisible ? "显示" : "隐藏")}");
        builder.AppendLine($"声音提醒：{(_settings.SoundEnabled ? "开启" : "关闭")}");
        builder.AppendLine($"边缘触发区：{_settings.DockHoverHeight} px");
        builder.AppendLine($"吸附自动隐藏：{(_settings.DockIdleHideSeconds <= 0 ? "关闭" : _settings.DockIdleHideSeconds + " 秒")}");
        builder.AppendLine($"鼠标唤出保持：{_settings.DockRevealSeconds} 秒");
        builder.AppendLine($"任务云朵保持：{_settings.DockNotificationSeconds} 秒");
        builder.AppendLine($"登录启动：{(SafeIsAutoStartEnabled() ? "开启" : "关闭")}");
        using (var process = Process.GetCurrentProcess())
        {
            builder.AppendLine($"进程工作集：{process.WorkingSet64 / 1024d / 1024d:F1} MB");
            builder.AppendLine($"进程私有内存：{process.PrivateMemorySize64 / 1024d / 1024d:F1} MB");
            builder.AppendLine($"托管堆：{GC.GetTotalMemory(false) / 1024d / 1024d:F1} MB");
            builder.AppendLine($"进程线程数：{process.Threads.Count}");
        }
        builder.AppendLine();
        builder.Append(_snapshot.DiagnosticsText);
        return builder.ToString();
    }

    private void OpenLatestRelease()
    {
        try
        {
            _platform.OpenUri(LatestReleaseUri);
        }
        catch (Exception exception)
        {
            ShowMessage("无法打开更新页面", exception.Message);
        }
    }

    private void RevealAssistant()
    {
        if (_settings.PetVisible)
        {
            _petWindow.ShowInactive();
        }
    }

    private void OnPetPositionChanged(object? sender, PetPositionState position)
    {
        _settings.PetPosition = position;
        SaveSettings();
    }

    private void SaveSettings()
    {
        try
        {
            _settingsStore.Save(_settings);
        }
        catch (Exception exception)
        {
            Debug.WriteLine(exception);
        }
    }

    private bool SafeIsAutoStartEnabled()
    {
        try
        {
            return _platform.IsAutoStartEnabled();
        }
        catch
        {
            return false;
        }
    }

    private void UpdateMenuChecks()
    {
        var startup = SafeIsAutoStartEnabled();
        _nativeAssistantItem.IsChecked = _settings.PetVisible;
        _nativeSoundItem.IsChecked = _settings.SoundEnabled;
        _nativeStartupItem.IsChecked = startup;
        _contextAssistantItem.IsChecked = _settings.PetVisible;
        _contextSoundItem.IsChecked = _settings.SoundEnabled;
        _contextStartupItem.IsChecked = startup;
    }

    private static void ShowMessage(string title, string message)
    {
        new MessageWindow(title, message).Show();
    }

    private void Shutdown()
    {
        if (_disposed) return;

        _petWindow.SaveCurrentPosition();
        Dispose();
        _lifetime.Shutdown();
    }

    public void Dispose()
    {
        if (_disposed) return;

        _disposed = true;
        _animationTimer.Stop();
        _animationTimer.Tick -= OnAnimationTick;
        _monitorWorker.Updated -= OnMonitorUpdated;
        _monitorWorker.Dispose();
        _sound.Dispose();
        _settingsWindow?.Close();
        _diagnosticsWindow?.Close();
        _petWindow.SaveCurrentPosition();
        _petWindow.PositionStateChanged -= OnPetPositionChanged;
        _petWindow.Close();
        _trayIcon.IsVisible = false;
        _trayIcon.Dispose();
        _resources.Dispose();
    }

    private static NativeMenuItem CreateNativeItem(string header, Action action)
    {
        var item = new NativeMenuItem(header);
        item.Click += (_, _) => action();
        return item;
    }

    private static NativeMenuItem CreateNativeToggle(string header, bool isChecked, Action action)
    {
        var item = CreateNativeItem(header, action);
        item.ToggleType = MenuItemToggleType.CheckBox;
        item.IsChecked = isChecked;
        return item;
    }

    private static MenuItem CreateContextItem(string header, Action action)
    {
        var item = new MenuItem { Header = header };
        item.Click += (_, _) => action();
        return item;
    }

    private static MenuItem CreateContextToggle(string header, bool isChecked, Action action)
    {
        var item = CreateContextItem(header, action);
        item.ToggleType = MenuItemToggleType.CheckBox;
        item.IsChecked = isChecked;
        return item;
    }
}






