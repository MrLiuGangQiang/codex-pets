using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;

namespace CodeXPets.App.Views;

public sealed class MessageWindow : Window
{
    public MessageWindow(string title, string message, string? copyText = null)
    {
        Title = title;
        Width = 480;
        MinHeight = 180;
        SizeToContent = SizeToContent.Height;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        CanResize = false;
        ShowInTaskbar = false;
        var close = new Button
        {
            Content = "确定",
            MinWidth = 90,
            HorizontalAlignment = HorizontalAlignment.Right
        };
        close.Click += (_, _) => Close();
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 10
        };
        if (!string.IsNullOrWhiteSpace(copyText))
        {
            var copy = new Button { Content = "复制命令", MinWidth = 100 };
            copy.Click += async (_, _) =>
            {
                var clipboard = GetTopLevel(this)?.Clipboard;
                if (clipboard is not null) await clipboard.SetTextAsync(copyText);
            };
            buttons.Children.Add(copy);
        }
        buttons.Children.Add(close);
        Content = new StackPanel
        {
            Margin = new Thickness(22),
            Spacing = 18,
            Children =
            {
                new TextBlock
                {
                    Text = message,
                    TextWrapping = TextWrapping.Wrap,
                    FontSize = 14
                },
                buttons
            }
        };
    }
}
