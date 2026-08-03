using Avalonia;
using Avalonia.Media.Imaging;
using CodeXPets.App.Views;
using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;

namespace CodeXPets.App.Services;

public static class PreviewRenderer
{
    private static readonly PreviewDefinition[] Definitions =
    [
        new("idle.png", ReminderState.Idle, "空闲", "主人，现在没有在进行中的任务!别让我歇着!", [], [], false, DockEdge.None),
        new("busy.png", ReminderState.Busy, "进行中", "重构跨平台桌面宠物",
            ["重构跨平台桌面宠物"], ["1/3"], false, DockEdge.None),
        new("floating-left.png", ReminderState.Busy, "进行中", "左侧未吸附",
            ["检查左侧猫头与思考点对齐"], ["1/2"], false, DockEdge.None,
            MirrorFloatingSprite: true),
        new("multi-session.png", ReminderState.Busy, "进行中", "处理多个任务",
            ["第一个会话任务", "第二个会话任务"], ["1/3", "2/4"], false, DockEdge.None),
        new("completed.png", ReminderState.Completed, "已完成", "任务完成啦！",
            ["重构跨平台桌面宠物"], [], false, DockEdge.None),
        new("error.png", ReminderState.Error, "异常", "任务异常了",
            ["任务失败：构建发布包"], [], false, DockEdge.None),
        new("dock-left.png", ReminderState.Busy, "进行中", "正在处理任务",
            ["Windows ARM64 发布"], ["2/4"], true, DockEdge.Left),
        new("dock-right.png", ReminderState.Error, "异常", "任务异常了",
            ["任务失败：macOS 公证"], [], true, DockEdge.Right),
        new("dock-left-below.png", ReminderState.Busy, "进行中", "正在处理任务",
            ["Windows ARM64 发布"], ["2/4"], true, DockEdge.Left, true),
        new("dock-right-below.png", ReminderState.Error, "异常", "任务异常了",
            ["任务失败：macOS 公证"], [], true, DockEdge.Right, true),
        new("dock-left-hide-75.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Left, false, 0.75, 11),
        new("dock-left-hide-50.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Left, false, 0.50, 11),
        new("dock-left-hide-25.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Left, false, 0.25, 11),
        new("dock-right-hide-75.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Right, false, 0.75, 11),
        new("dock-right-hide-50.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Right, false, 0.50, 11),
        new("dock-right-hide-25.png", ReminderState.Idle, "空闲", "", [], [],
            true, DockEdge.Right, false, 0.25, 11)
    ];

    public static void SaveAll(string folder)
    {
        Directory.CreateDirectory(folder);
        using var resources = new ResourceCatalog();
        foreach (var definition in Definitions)
        {
            using var bitmap = Render(resources, definition);
            using var stream = File.Create(Path.Combine(folder, definition.FileName));
            bitmap.Save(stream, new PngBitmapEncoderOptions());
        }
    }

    public static bool ValidateAll(out string error)
    {
        try
        {
            using var resources = new ResourceCatalog();
            foreach (var definition in Definitions)
            {
                using var bitmap = Render(resources, definition);
                if (bitmap.PixelSize != new PixelSize(840, 520))
                {
                    error = "离屏渲染尺寸错误：" + definition.FileName;
                    return false;
                }
            }

            if (!ValidateBodyViewportAndScrollLoop(resources, out error))
            {
                return false;
            }

            if (!ValidateFixedSessionRotation(resources, out error))
            {
                return false;
            }

            var defaults = AppSettings.CreateDefault();
            if (new SettingsWindow(defaults).Content is null ||
                new DiagnosticsWindow(() => "diagnostics", () => { }).Content is null ||
                new MessageWindow("CodeXPets", "message").Content is null)
            {
                error = "设置、诊断或消息窗口初始化失败。";
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

    private static bool ValidateBodyViewportAndScrollLoop(ResourceCatalog resources, out string error)
    {
        var surface = new PetSurface(resources)
        {
            Width = PetWindow.LogicalWidth,
            Height = PetWindow.LogicalHeight
        };
        surface.UpdateStatus("进行中", string.Empty, ReminderState.Busy,
            ["这是一段用于验证正文循环滚动的长任务内容，超过三行后必须完整裁切，并在滚动到底后重新从第一行开始循环显示。"],
            [], false, -1);
        surface.Measure(new Size(PetWindow.LogicalWidth, PetWindow.LogicalHeight));
        surface.Arrange(new Rect(0, 0, PetWindow.LogicalWidth, PetWindow.LogicalHeight));

        if (Math.Abs(surface.ContentBounds.Height - 45) > 0.01)
        {
            error = $"正文视口高度错误：{surface.ContentBounds.Height}，预期严格显示三行（45）。";
            return false;
        }

        var moved = false;
        var looped = false;
        for (var tick = 0; tick < 400; tick++)
        {
            surface.Animate(tick, 0.1);
            if (surface.ScrollOffset > 1)
            {
                moved = true;
            }
            else if (moved && surface.ScrollOffset < 0.01)
            {
                looped = true;
                break;
            }
        }

        if (!moved || !looped)
        {
            error = "正文长文本未完成循环滚动。";
            return false;
        }

        var repeated = false;
        for (var tick = 400; tick < 520; tick++)
        {
            surface.Animate(tick, 0.1);
            if (surface.ScrollOffset > 1)
            {
                repeated = true;
                break;
            }
        }

        if (!repeated)
        {
            error = "正文滚动到底后没有重新开始下一轮滚动。";
            return false;
        }

        error = string.Empty;
        return true;
    }

    private static bool ValidateFixedSessionRotation(ResourceCatalog resources, out string error)
    {
        static PetSurface CreateSurface(ResourceCatalog catalog, IReadOnlyList<string> titles,
            IReadOnlyList<string?> progress)
        {
            var surface = new PetSurface(catalog)
            {
                Width = PetWindow.LogicalWidth,
                Height = PetWindow.LogicalHeight
            };
            surface.UpdateStatus("进行中", string.Empty, ReminderState.Busy,
                titles, progress, false, -1);
            surface.Measure(new Size(PetWindow.LogicalWidth, PetWindow.LogicalHeight));
            surface.Arrange(new Rect(0, 0, PetWindow.LogicalWidth, PetWindow.LogicalHeight));
            return surface;
        }

        string[] shortTitles = ["短标题", "第二个会话"];
        string[] longTitles =
        [
            "这是用于验证固定切换时间的超长会话标题，它会换行并滚动，但不能因此延后或提前切换会话。",
            "第二个会话"
        ];
        var shortSurface = CreateSurface(resources, shortTitles, ["1/2", null]);
        var longSurface = CreateSurface(resources, longTitles, ["1/9", null]);

        for (var tick = 0; tick < 30; tick++)
        {
            shortSurface.Animate(tick, 0.1);
            longSurface.Animate(tick, 0.1);
        }

        // Progress metadata can update during a session without restarting its fixed timer.
        shortSurface.UpdateStatus("进行中", string.Empty, ReminderState.Busy,
            shortTitles, ["2/2", null], false, -1);

        for (var tick = 30; tick < 59; tick++)
        {
            shortSurface.Animate(tick, 0.1);
            longSurface.Animate(tick, 0.1);
        }

        if (shortSurface.SelectedTaskIndex != 0 || longSurface.SelectedTaskIndex != 0)
        {
            error = "会话在固定的 6 秒间隔之前提前切换。";
            return false;
        }

        shortSurface.Animate(59, 0.1);
        longSurface.Animate(59, 0.1);
        if (shortSurface.SelectedTaskIndex != 1 || longSurface.SelectedTaskIndex != 1)
        {
            error = "长短标题未在相同的固定 6 秒时刻切换会话。";
            return false;
        }

        error = string.Empty;
        return true;
    }

    private static RenderTargetBitmap Render(ResourceCatalog resources, PreviewDefinition definition)
    {
        var surface = new PetSurface(resources)
        {
            Width = PetWindow.LogicalWidth,
            Height = PetWindow.LogicalHeight,
            IsDocked = definition.Docked,
            DockEdge = definition.Edge,
            DockBubbleBelow = definition.BubbleBelow,
            DockVisibility = definition.DockVisibility,
            MirrorFloatingSprite = definition.MirrorFloatingSprite,
            DockThoughtUntil = DateTimeOffset.Now.AddMinutes(1)
        };
        surface.UpdateStatus(definition.Status, definition.Thought, definition.State,
            definition.Titles, definition.Progress, false, -1);
        surface.Animate(definition.AnimationTick, 0);
        surface.Measure(new Size(PetWindow.LogicalWidth, PetWindow.LogicalHeight));
        surface.Arrange(new Rect(0, 0, PetWindow.LogicalWidth, PetWindow.LogicalHeight));
        var bitmap = new RenderTargetBitmap(new PixelSize(840, 520), new Vector(192, 192));
        bitmap.Render(surface);
        return bitmap;
    }

    private sealed record PreviewDefinition(
        string FileName,
        ReminderState State,
        string Status,
        string Thought,
        IReadOnlyList<string> Titles,
        IReadOnlyList<string?> Progress,
        bool Docked,
        DockEdge Edge,
        bool BubbleBelow = false,
        double DockVisibility = 1,
        int AnimationTick = 0,
        bool MirrorFloatingSprite = false);
}
