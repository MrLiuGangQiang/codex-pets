using Avalonia;
using Avalonia.Controls;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace CodeXPets.App.Infrastructure;

[SupportedOSPlatform("macos")]
internal sealed class MacPlatformService : IPlatformService
{
    private static readonly nint ServiceManagementHandle = LoadServiceManagement();

    public string PlatformName => "macOS";
    public string RuntimeArchitecture => RuntimeInformation.ProcessArchitecture.ToString();
    public bool RequiresMousePassThroughPolling => true;

    public PixelPoint GetCursorPosition()
    {
        var eventHandle = CGEventCreate(nint.Zero);
        if (eventHandle == nint.Zero) return default;
        try
        {
            var point = CGEventGetLocation(eventHandle);
            return new PixelPoint((int)Math.Round(point.X), (int)Math.Round(point.Y));
        }
        finally
        {
            CFRelease(eventHandle);
        }
    }

    public void ConfigurePetWindow(Window window, Func<Point, bool> isInteractivePoint)
    {
    }

    public void UpdatePetWindowMousePassThrough(Window window, bool shouldReceiveMouse)
    {
        var handle = window.TryGetPlatformHandle()?.Handle ?? nint.Zero;
        if (handle == nint.Zero) return;
        var shouldIgnoreMouse = shouldReceiveMouse == false;
        objc_msgSend_bool(handle, sel_registerName("setIgnoresMouseEvents:"), shouldIgnoreMouse);
    }

    public void UpdatePetWindowClickRegion(Window window, IReadOnlyList<Rect> interactiveRectsDip)
    {
        // macOS uses setIgnoresMouseEvents: polling for click-through.
    }

    public bool IsAutoStartEnabled()
    {
        var service = GetMainAppService();
        if (service == nint.Zero) return false;
        return objc_msgSend_nint(service, sel_registerName("status")) == 1;
    }

    public void SetAutoStartEnabled(bool enabled)
    {
        var service = GetMainAppService();
        if (service == nint.Zero)
            throw new PlatformNotSupportedException("当前 macOS 不支持 SMAppService。");
        nint error = nint.Zero;
        var selector = sel_registerName(enabled ? "registerAndReturnError:" : "unregisterAndReturnError:");
        if (objc_msgSend_error(service, selector, ref error) == false)
            throw new InvalidOperationException("macOS 登录启动设置失败。");
    }

    public void OpenFolder(string path) => StartOpen(path);
    public void OpenUri(Uri uri) => StartOpen(uri.AbsoluteUri);
    public void ShowDuplicateInstanceMessage() => Console.Error.WriteLine("CodeXPets 已经在菜单栏里运行。");

    private static void StartOpen(string value)
    {
        var info = new ProcessStartInfo("/usr/bin/open") { UseShellExecute = false };
        info.ArgumentList.Add(value);
        Process.Start(info);
    }

    private static nint GetMainAppService()
    {
        _ = ServiceManagementHandle;
        var serviceClass = objc_getClass("SMAppService");
        return serviceClass == nint.Zero ? nint.Zero :
            objc_msgSend_nint(serviceClass, sel_registerName("mainAppService"));
    }

    private static nint LoadServiceManagement()
    {
        try
        {
            return NativeLibrary.Load("/System/Library/Frameworks/ServiceManagement.framework/ServiceManagement");
        }
        catch
        {
            return nint.Zero;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct CGPoint
    {
        public readonly double X;
        public readonly double Y;
    }

    [DllImport("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")]
    private static extern nint CGEventCreate(nint source);

    [DllImport("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")]
    private static extern CGPoint CGEventGetLocation(nint eventHandle);

    [DllImport("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")]
    private static extern void CFRelease(nint value);

    [DllImport("/usr/lib/libobjc.A.dylib")]
    private static extern nint sel_registerName(string selectorName);

    [DllImport("/usr/lib/libobjc.A.dylib")]
    private static extern nint objc_getClass(string className);

    [DllImport("/usr/lib/libobjc.A.dylib", EntryPoint = "objc_msgSend")]
    private static extern void objc_msgSend_bool(nint receiver, nint selector,
        [MarshalAs(UnmanagedType.I1)] bool value);

    [DllImport("/usr/lib/libobjc.A.dylib", EntryPoint = "objc_msgSend")]
    private static extern nint objc_msgSend_nint(nint receiver, nint selector);

    [DllImport("/usr/lib/libobjc.A.dylib", EntryPoint = "objc_msgSend")]
    [return: MarshalAs(UnmanagedType.I1)]
    private static extern bool objc_msgSend_error(nint receiver, nint selector, ref nint error);
}

