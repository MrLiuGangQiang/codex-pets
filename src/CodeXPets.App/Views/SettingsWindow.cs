using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using CodeXPets.Core.Configuration;

namespace CodeXPets.App.Views;

public sealed class SettingsWindow : Window
{
    private readonly NumericUpDown _hoverHeight;
    private readonly NumericUpDown _idleHide;
    private readonly NumericUpDown _revealSeconds;
    private readonly NumericUpDown _notificationSeconds;
    private readonly CheckBox _soundEnabled;
    private readonly TextBox _sessionsRoot;

    public SettingsWindow(AppSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);

        Title = "CodeXPets 设置";
        Width = 620;
        Height = 470;
        MinWidth = 560;
        MinHeight = 430;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        CanResize = true;
        ShowInTaskbar = false;

        _hoverHeight = CreateNumberBox(40, 1000, settings.DockHoverHeight);
        _idleHide = CreateNumberBox(0, 3600, settings.DockIdleHideSeconds);
        _revealSeconds = CreateNumberBox(1, 60, settings.DockRevealSeconds);
        _notificationSeconds = CreateNumberBox(1, 120, settings.DockNotificationSeconds);
        _soundEnabled = new CheckBox
        {
            Content = "播放开始、完成和异常语音提醒",
            IsChecked = settings.SoundEnabled,
            VerticalAlignment = VerticalAlignment.Center
        };
        _sessionsRoot = new TextBox
        {
            Text = settings.SessionsRoot,
            HorizontalAlignment = HorizontalAlignment.Stretch
        };

        var browse = new Button { Content = "浏览…", MinWidth = 82 };
        browse.Click += BrowseSessionsRoot;
        var rootPanel = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto"),
            ColumnSpacing = 8
        };
        rootPanel.Children.Add(_sessionsRoot);
        Grid.SetColumn(browse, 1);
        rootPanel.Children.Add(browse);

        var form = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,Auto,Auto,Auto,Auto,Auto"),
            ColumnDefinitions = new ColumnDefinitions("230,*"),
            RowSpacing = 14,
            ColumnSpacing = 14
        };
        AddRow(form, 0, "边缘唤出区域高度（像素）", _hoverHeight);
        AddRow(form, 1, "吸附隐藏（秒，0=关闭）", _idleHide);
        AddRow(form, 2, "鼠标唤出后保持（秒）", _revealSeconds);
        AddRow(form, 3, "任务状态云朵保持（秒）", _notificationSeconds);
        AddRow(form, 4, "Codex 会话目录", rootPanel);
        Grid.SetRow(_soundEnabled, 5);
        Grid.SetColumnSpan(_soundEnabled, 2);
        form.Children.Add(_soundEnabled);

        var hint = new TextBlock
        {
            Text = "CodeXPets 通过 FileSystemWatcher 增量读取该目录中的 JSONL 会话文件，" +
                   "不会启动额外服务，也不会修改 Codex 文件。",
            TextWrapping = TextWrapping.Wrap,
            Foreground = Brushes.Gray
        };

        var defaults = new Button { Content = "恢复默认", MinWidth = 90 };
        var save = new Button { Content = "保存", MinWidth = 90, IsDefault = true };
        var cancel = new Button { Content = "取消", MinWidth = 90, IsCancel = true };
        defaults.Click += (_, _) => SetValues(AppSettings.CreateDefault());
        save.Click += (_, _) => SaveAndClose();
        cancel.Click += (_, _) => Close();

        var buttons = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto"),
            ColumnSpacing = 10
        };
        buttons.Children.Add(defaults);
        Grid.SetColumn(cancel, 2);
        buttons.Children.Add(cancel);
        Grid.SetColumn(save, 3);
        buttons.Children.Add(save);

        var content = new Grid
        {
            Margin = new Thickness(22),
            RowDefinitions = new RowDefinitions("Auto,Auto,*,Auto"),
            RowSpacing = 14
        };
        var heading = new TextBlock
        {
            Text = "桌面宠物与会话监听",
            FontSize = 20,
            FontWeight = FontWeight.SemiBold
        };
        content.Children.Add(heading);
        Grid.SetRow(form, 1);
        content.Children.Add(form);
        Grid.SetRow(hint, 2);
        content.Children.Add(hint);
        Grid.SetRow(buttons, 3);
        content.Children.Add(buttons);
        Content = content;
    }

    public event EventHandler<AppSettings>? SettingsSaved;

    private async void BrowseSessionsRoot(
        object? sender,
        Avalonia.Interactivity.RoutedEventArgs eventArgs)
    {
        try
        {
            var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = "选择 Codex sessions 目录",
                AllowMultiple = false
            });
            var folder = folders.FirstOrDefault();
            if (folder is null) return;

            if (!folder.Path.IsFile)
            {
                new MessageWindow("无法使用该目录", "Codex sessions 必须是本地文件夹。").Show();
                return;
            }
            _sessionsRoot.Text = folder.Path.LocalPath;
        }
        catch (Exception exception)
        {
            new MessageWindow("无法选择会话目录", exception.Message).Show();
        }
    }
    private void SetValues(AppSettings settings)
    {
        var normalized = settings.Clone();
        normalized.Normalize();
        _hoverHeight.Value = normalized.DockHoverHeight;
        _idleHide.Value = normalized.DockIdleHideSeconds;
        _revealSeconds.Value = normalized.DockRevealSeconds;
        _notificationSeconds.Value = normalized.DockNotificationSeconds;
        _soundEnabled.IsChecked = normalized.SoundEnabled;
        _sessionsRoot.Text = normalized.SessionsRoot;
    }

    private void SaveAndClose()
    {
        var result = new AppSettings
        {
            DockHoverHeight = decimal.ToInt32(_hoverHeight.Value ?? 240),
            DockIdleHideSeconds = decimal.ToInt32(_idleHide.Value ?? 10),
            DockRevealSeconds = decimal.ToInt32(_revealSeconds.Value ?? 3),
            DockNotificationSeconds = decimal.ToInt32(_notificationSeconds.Value ?? 5),
            SoundEnabled = _soundEnabled.IsChecked == true,
            SessionsRoot = _sessionsRoot.Text ?? string.Empty
        };
        result.Normalize();
        SettingsSaved?.Invoke(this, result);
        Close();
    }

    private static NumericUpDown CreateNumberBox(int minimum, int maximum, int value) => new()
    {
        Minimum = minimum,
        Maximum = maximum,
        Value = value,
        Increment = 1,
        FormatString = "0",
        HorizontalAlignment = HorizontalAlignment.Left,
        Width = 120
    };

    private static void AddRow(Grid grid, int row, string label, Control editor)
    {
        var text = new TextBlock
        {
            Text = label,
            VerticalAlignment = VerticalAlignment.Center,
            TextWrapping = TextWrapping.NoWrap
        };
        Grid.SetRow(text, row);
        grid.Children.Add(text);
        Grid.SetRow(editor, row);
        Grid.SetColumn(editor, 1);
        grid.Children.Add(editor);
    }
}

