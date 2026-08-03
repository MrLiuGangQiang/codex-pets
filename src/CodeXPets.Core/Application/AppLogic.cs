using CodeXPets.Core.Domain;

namespace CodeXPets.Core.Application;

public static class AppLogic
{
    private const string BusyHeaderSessionSeparator = " • ";

    public static string FormatAbnormalTaskText(string? title)
    {
        var normalized = string.IsNullOrWhiteSpace(title) ? "未知任务" : title.Trim();
        return "任务失败：" + normalized;
    }

    public static int SelectPreferredTaskIndex(bool focusLatestTask, int latestTaskIndex) =>
        focusLatestTask ? latestTaskIndex : -1;

    public static int ReconcileTaskSelection(
        ReminderState state,
        IReadOnlyList<string> titles,
        int previousIndex,
        string? previouslySelectedTitle,
        bool selectNewestTask,
        int preferredTaskIndex)
    {
        ArgumentNullException.ThrowIfNull(titles);
        if (titles.Count == 0 || state != ReminderState.Busy)
        {
            return 0;
        }

        if (preferredTaskIndex >= 0 && preferredTaskIndex < titles.Count)
        {
            return preferredTaskIndex;
        }

        if (selectNewestTask)
        {
            return titles.Count - 1;
        }

        if (!string.IsNullOrEmpty(previouslySelectedTitle))
        {
            for (var index = 0; index < titles.Count; index++)
            {
                if (string.Equals(titles[index], previouslySelectedTitle, StringComparison.Ordinal))
                {
                    return index;
                }
            }
        }

        return Math.Clamp(previousIndex, 0, titles.Count - 1);
    }

    public static ReminderState SelectVisualState(
        int activeCount,
        bool abnormalRecently,
        bool completedRecently,
        ReminderState latestChangedState)
    {
        if (latestChangedState == ReminderState.Error && abnormalRecently)
        {
            return ReminderState.Error;
        }

        if (latestChangedState == ReminderState.Completed && completedRecently)
        {
            return ReminderState.Completed;
        }

        if (latestChangedState == ReminderState.Busy && activeCount > 0)
        {
            return ReminderState.Busy;
        }

        return activeCount > 0 ? ReminderState.Busy : ReminderState.Idle;
    }

    public static int CloudNotificationSeconds(ReminderState state, int configuredSeconds)
    {
        var safe = Math.Max(1, configuredSeconds);
        return state == ReminderState.Error ? Math.Max(10, safe) : safe;
    }

    public static bool ShouldShowThoughtBubble(
        bool isDocked,
        ReminderState state,
        DateTimeOffset now,
        DateTimeOffset dockThoughtUntil)
    {
        if (!isDocked)
        {
            return true;
        }

        var hasTaskState = state is ReminderState.Busy or ReminderState.Completed or ReminderState.Error;
        return hasTaskState && now < dockThoughtUntil;
    }

    public static bool ShouldShowDock(
        DateTimeOffset lastContentChange,
        DateTimeOffset now,
        bool isDragging,
        bool isHovering,
        DateTimeOffset hoverRevealUntil,
        int idleHideSeconds) =>
        isDragging || isHovering || now < hoverRevealUntil || idleHideSeconds <= 0 ||
        now - lastContentChange < TimeSpan.FromSeconds(idleHideSeconds);

    public static DockEdge SelectSnapEdge(PointD cursor, RectD workArea, double snapDistance)
    {
        if (Math.Abs(cursor.X - workArea.Left) <= snapDistance)
        {
            return DockEdge.Left;
        }

        if (Math.Abs(cursor.X - workArea.Right) <= snapDistance)
        {
            return DockEdge.Right;
        }

        return DockEdge.None;
    }

    public static bool ShouldMirrorFloatingSprite(PointD anchor, RectD workArea) =>
        anchor.X < workArea.CenterX;

    public static RectD DockHoverBounds(
        DockEdge edge,
        RectD workArea,
        double dockY,
        double scale,
        bool fullyHidden,
        int hoverHeight)
    {
        var width = fullyHidden ? Math.Max(18, 28 * scale) : Math.Max(40, 56 * scale);
        var normalizedHeight = Math.Clamp(hoverHeight, 40, 1000);
        var halfHeight = Math.Max(20, normalizedHeight * scale / 2d);
        var x = edge == DockEdge.Left ? workArea.Left : workArea.Right - width;
        var top = Math.Max(workArea.Top, dockY - halfHeight);
        var bottom = Math.Min(workArea.Bottom, dockY + halfHeight);
        return new RectD(x, top, width, Math.Max(1, bottom - top));
    }

    public static bool IsTaskSwitchPoint(
        bool isDocked,
        bool bubbleVisible,
        ReminderState state,
        int taskCount,
        RectD bubbleBounds,
        RectD contentBounds,
        PointD point)
    {
        if (!bubbleVisible || taskCount <= 1 || state == ReminderState.Idle)
        {
            return false;
        }

        return (isDocked ? bubbleBounds : contentBounds).Contains(point);
    }

    public static string FormatBusyHeader(string? stepProgress, int sessionIndex, int sessionCount)
    {
        var header = string.IsNullOrWhiteSpace(stepProgress)
            ? "进行中"
            : $"进行中({stepProgress})";
        if (sessionCount <= 1) return header;

        var safeIndex = Math.Clamp(sessionIndex, 0, sessionCount - 1);
        return $"{header}{BusyHeaderSessionSeparator}{safeIndex + 1}/{sessionCount}";
    }

    public static int SelectDockSpriteIndex(DockEdge edge, ReminderState state, int frame)
    {
        var phase = Math.Abs(frame) % 20;
        var expression = phase is 11 or 14
            ? 1
            : state switch
            {
                ReminderState.Completed => 2,
                ReminderState.Error => 3,
                _ => 0
            };
        return (edge == DockEdge.Right ? 4 : 0) + expression;
    }

    public static int SelectFloatingSpriteRow(ReminderState state) => state switch
    {
        ReminderState.Completed => 1,
        ReminderState.Busy => 2,
        ReminderState.Error => 3,
        _ => 0
    };

    public static int SelectFloatingFrame(ReminderState state, int animationTick)
    {
        if (state == ReminderState.Busy)
        {
            var phase = Math.Abs(animationTick) % 64;
            if (phase < 40) return (phase / 2) % 4;
            if (phase < 44) return 4;
            if (phase < 60) return 5 + ((phase - 44) / 4) % 2;
            return 7;
        }

        return state == ReminderState.Completed
            ? Math.Abs(animationTick / 3) % 4
            : Math.Abs(animationTick / 3) % 8;
    }
}

