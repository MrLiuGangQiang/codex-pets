using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace CodeXPets.App.Views;

public sealed class DiagnosticsWindow : Window
{
    private readonly Func<string> _diagnosticsProvider;
    private readonly TextBox _text;
    private readonly DispatcherTimer _timer;

    public DiagnosticsWindow(Func<string> diagnosticsProvider, Action openSessionsFolder)
    {
        _diagnosticsProvider = diagnosticsProvider;
        Title = "CodeXPets 诊断信息";
        Width = 780;
        Height = 560;
        MinWidth = 620;
        MinHeight = 420;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        ShowInTaskbar = false;
        CanMinimize = false;

        _text = new TextBox
        {
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.NoWrap,
            FontFamily = FontFamily.Parse("Consolas, Menlo, monospace"),
            FontSize = 12
        };
        ScrollViewer.SetHorizontalScrollBarVisibility(_text, Avalonia.Controls.Primitives.ScrollBarVisibility.Auto);
        ScrollViewer.SetVerticalScrollBarVisibility(_text, Avalonia.Controls.Primitives.ScrollBarVisibility.Auto);
        var copy = new Button { Content = "复制", MinWidth = 82 };
        var refresh = new Button { Content = "刷新", MinWidth = 82 };
        var folder = new Button { Content = "打开会话目录", MinWidth = 120 };
        var close = new Button { Content = "关闭", MinWidth = 82 };
        copy.Click += async (_, _) =>
        {
            var clipboard = GetTopLevel(this)?.Clipboard;
            if (clipboard is not null) await clipboard.SetTextAsync(_text.Text ?? string.Empty);
        };
        refresh.Click += (_, _) => Refresh();
        folder.Click += (_, _) => openSessionsFolder();
        close.Click += (_, _) => Close();

        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 10,
            Children = { copy, refresh, folder, new Border { HorizontalAlignment = HorizontalAlignment.Stretch }, close }
        };
        Content = new Grid
        {
            Margin = new Thickness(16),
            RowDefinitions = new RowDefinitions("*,Auto"),
            RowSpacing = 12,
            Children = { _text, buttons }
        };
        Grid.SetRow(buttons, 1);

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => Refresh();
        Opened += (_, _) => { Refresh(); _timer.Start(); };
        Closed += (_, _) => _timer.Stop();
    }

    private void Refresh()
    {
        _text.Text = _diagnosticsProvider();
        _text.CaretIndex = 0;
    }
}
