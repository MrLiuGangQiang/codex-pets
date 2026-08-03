using Avalonia;
using Avalonia.Controls;
using Microsoft.Win32;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace CodeXPets.App.Infrastructure;

[SupportedOSPlatform("windows")]
internal sealed class WindowsPlatformService : IPlatformService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "CodeXPets";
    private const int GwlExStyle = -20;
    private const int GwlWndProc = -4;
    private const long WsExNoActivate = 0x08000000L;
    private const long WsExToolWindow = 0x00000080L;
    private const long WsExTransparent = 0x00000020L;
    private const uint WmNcHitTest = 0x0084;
    private const uint WmMouseActivate = 0x0021;
    private const nint HtClient = 1;
    private const nint HtTransparent = -1;
    private const nint MaNoActivate = 3;

    private Window? _petWindow;
    private Func<Point, bool>? _isInteractivePoint;
    private WndProcDelegate? _wndProc;
    private nint _originalWndProc;

    public string PlatformName => "Windows";
    public string RuntimeArchitecture => RuntimeInformation.ProcessArchitecture.ToString();
    public bool RequiresMousePassThroughPolling => false;

    public PixelPoint GetCursorPosition() => GetCursorPos(out var point)
        ? new PixelPoint(point.X, point.Y)
        : default;

    public void ConfigurePetWindow(Window window, Func<Point, bool> isInteractivePoint)
    {
        _petWindow = window;
        _isInteractivePoint = isInteractivePoint;
        var handle = window.TryGetPlatformHandle()?.Handle ?? nint.Zero;
        if (handle == nint.Zero) return;

        var styles = GetWindowLongPtr(handle, GwlExStyle).ToInt64();
        SetWindowLongPtr(handle, GwlExStyle, new nint(styles | WsExNoActivate | WsExToolWindow));
        _wndProc = WindowProc;
        _originalWndProc = SetWindowLongPtr(handle, GwlWndProc,
            Marshal.GetFunctionPointerForDelegate(_wndProc));
        window.Closed += (_, _) => RestoreWindowProcedure(handle);
    }

    public void UpdatePetWindowMousePassThrough(Window window, bool shouldReceiveMouse)
    {
        var handle = window.TryGetPlatformHandle()?.Handle ?? nint.Zero;
        if (handle == nint.Zero) return;
        var styles = GetWindowLongPtr(handle, GwlExStyle).ToInt64();
        var next = shouldReceiveMouse ? styles & ~WsExTransparent : styles | WsExTransparent;
        if (next != styles)
            SetWindowLongPtr(handle, GwlExStyle, new nint(next));
    }

    public void UpdatePetWindowClickRegion(Window window, IReadOnlyList<Rect> interactiveRectsDip)
    {
        var handle = window.TryGetPlatformHandle()?.Handle ?? nint.Zero;
        if (handle == nint.Zero) return;
        var scaling = Math.Max(0.1, window.RenderScaling);
        nint combined = nint.Zero;
        foreach (var r in interactiveRectsDip)
        {
            var x = (int)Math.Floor(r.X * scaling);
            var y = (int)Math.Floor(r.Y * scaling);
            var right = (int)Math.Ceiling((r.X + r.Width) * scaling);
            var bottom = (int)Math.Ceiling((r.Y + r.Height) * scaling);
            var w = Math.Max(1, right - x);
            var h = Math.Max(1, bottom - y);
            var hrgn = CreateRectRgn(x, y, right, bottom);
            if (combined == nint.Zero)
            {
                combined = hrgn;
            }
            else
            {
                CombineRgn(combined, combined, hrgn, 2 /* RGN_OR */);
                DeleteObject(hrgn);
            }
        }

        if (combined == nint.Zero)
            combined = CreateRectRgn(0, 0, 1, 1);
        SetWindowRgn(handle, combined, true);
    }

    public bool IsAutoStartEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: false);
        return key?.GetValue(ValueName) is string command && !string.IsNullOrWhiteSpace(command);
    }

    public void SetAutoStartEnabled(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true)
            ?? throw new InvalidOperationException("无法打开 Windows 启动项注册表。");
        if (!enabled)
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
            return;
        }

        var executable = Environment.ProcessPath
            ?? throw new InvalidOperationException("无法确定 CodeXPets 可执行文件路径。");
        key.SetValue(ValueName, $"\"{executable}\"");
    }

    public void OpenFolder(string path) => Process.Start(new ProcessStartInfo("explorer.exe")
    {
        UseShellExecute = true,
        ArgumentList = { path }
    });

    public void OpenUri(Uri uri) => Process.Start(
        new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });

    public void ShowDuplicateInstanceMessage() =>
        NativeMessageBox(nint.Zero, "CodeXPets 已经在任务栏里啦。", "CodeXPets", 0x40);

    private nint WindowProc(nint hwnd, uint message, nint wParam, nint lParam)
    {
        if (message == WmNcHitTest && _petWindow is not null && _isInteractivePoint is not null)
        {
            var packed = lParam.ToInt64();
            var screen = new NativePoint
            {
                X = unchecked((short)(packed & 0xffff)),
                Y = unchecked((short)((packed >> 16) & 0xffff))
            };
            if (ScreenToClient(hwnd, ref screen))
            {
                var scaling = Math.Max(0.1, _petWindow.RenderScaling);
                var point = new Point(screen.X / scaling, screen.Y / scaling);
                return _isInteractivePoint(point) ? HtClient : HtTransparent;
            }
        }

        if (message == WmMouseActivate) return MaNoActivate;
        return _originalWndProc != nint.Zero
            ? CallWindowProc(_originalWndProc, hwnd, message, wParam, lParam)
            : DefWindowProc(hwnd, message, wParam, lParam);
    }

    private void RestoreWindowProcedure(nint handle)
    {
        if (_originalWndProc != nint.Zero && IsWindow(handle))
            SetWindowLongPtr(handle, GwlWndProc, _originalWndProc);
        _originalWndProc = nint.Zero;
        _wndProc = null;
        _petWindow = null;
        _isInteractivePoint = null;
    }

    private delegate nint WndProcDelegate(nint hwnd, uint message, nint wParam, nint lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint { public int X; public int Y; }

    [DllImport("user32.dll")] private static extern bool GetCursorPos(out NativePoint point);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(nint window, ref NativePoint point);
    [DllImport("user32.dll")]
    private static extern nint CallWindowProc(nint previous, nint window,
        uint message, nint wParam, nint lParam);
    [DllImport("user32.dll")]
    private static extern nint DefWindowProc(nint window,
        uint message, nint wParam, nint lParam);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr64(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong32(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr64(nint window, int index, nint value);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern int SetWindowLong32(nint window, int index, int value);
    [DllImport("user32.dll")] private static extern bool IsWindow(nint window);
    [DllImport("gdi32.dll")] private static extern nint CreateRectRgn(int left, int top, int right, int bottom);
    [DllImport("gdi32.dll")] private static extern int CombineRgn(nint dest, nint src1, nint src2, int combineMode);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(nint objectHandle);
    [DllImport("user32.dll")] private static extern int SetWindowRgn(nint window, nint region, bool redraw);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "MessageBoxW")]
    private static extern int NativeMessageBox(nint window, string text, string caption, uint type);

    private static nint GetWindowLongPtr(nint window, int index) => IntPtr.Size == 8
        ? GetWindowLongPtr64(window, index)
        : new nint(GetWindowLong32(window, index));

    private static nint SetWindowLongPtr(nint window, int index, nint value) => IntPtr.Size == 8
        ? SetWindowLongPtr64(window, index, value)
        : new nint(SetWindowLong32(window, index, value.ToInt32()));
}
