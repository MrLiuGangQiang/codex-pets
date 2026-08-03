using CodeXPets.Core.Application;
using CodeXPets.Core.Configuration;
using CodeXPets.Core.Domain;

namespace CodeXPets.Core.Tests;

public sealed class AppLogicTests
{
    [Fact]
    public void LatestTaskChangeControlsDisplayedState()
    {
        Assert.Equal(ReminderState.Error,
            AppLogic.SelectVisualState(1, abnormalRecently: true, completedRecently: false,
                ReminderState.Error));
        Assert.Equal(ReminderState.Completed,
            AppLogic.SelectVisualState(1, abnormalRecently: false, completedRecently: true,
                ReminderState.Completed));
        Assert.Equal(ReminderState.Busy,
            AppLogic.SelectVisualState(1, abnormalRecently: false, completedRecently: false,
                ReminderState.Completed));
    }

    [Fact]
    public void DockingAndCloudRulesMatchTheWindowsPrototype()
    {
        var work = new RectD(0, 0, 1920, 1080);
        Assert.Equal(DockEdge.Left, AppLogic.SelectSnapEdge(new PointD(8, 400), work, 36));
        Assert.Equal(DockEdge.Right, AppLogic.SelectSnapEdge(new PointD(1900, 400), work, 36));
        Assert.Equal(DockEdge.None, AppLogic.SelectSnapEdge(new PointD(900, 10), work, 36));
        Assert.True(AppLogic.ShouldMirrorFloatingSprite(new PointD(300, 500), work));
        Assert.False(AppLogic.ShouldMirrorFloatingSprite(new PointD(1500, 500), work));
        Assert.Equal(10, AppLogic.CloudNotificationSeconds(ReminderState.Error, 5));
        Assert.Equal(5, AppLogic.CloudNotificationSeconds(ReminderState.Completed, 5));
    }

    [Fact]
    public void BusyHeaderAndSessionCounterUseSeparateCompactLabels()
    {
        Assert.Equal("进行中(1/3) • 2/4", AppLogic.FormatBusyHeader("1/3", 1, 4));
        Assert.Equal("进行中 • 2/4", AppLogic.FormatBusyHeader(null, 1, 4));
        Assert.Equal("进行中(1/3)", AppLogic.FormatBusyHeader("1/3", 0, 1));
    }

    [Fact]
    public void SettingsAreNormalized()
    {
        var settings = new AppSettings
        {
            DockHoverHeight = -1,
            DockIdleHideSeconds = 9000,
            DockRevealSeconds = 0,
            DockNotificationSeconds = 1000,
            SessionsRoot = string.Empty
        };
        settings.Normalize();
        Assert.Equal(40, settings.DockHoverHeight);
        Assert.Equal(3600, settings.DockIdleHideSeconds);
        Assert.Equal(1, settings.DockRevealSeconds);
        Assert.Equal(120, settings.DockNotificationSeconds);
        Assert.Equal(CodexPaths.GetDefaultSessionsRoot(), settings.SessionsRoot);
    }
    [Fact]
    public void ManualTaskSelectionSurvivesPeriodicRefresh()
    {
        var titles = new[] { "任务 A", "任务 B", "任务 C" };
        var selected = AppLogic.ReconcileTaskSelection(ReminderState.Busy, titles,
            previousIndex: 1, previouslySelectedTitle: "任务 B",
            selectNewestTask: false, preferredTaskIndex: -1);
        Assert.Equal(1, selected);

        var reordered = new[] { "任务 B", "任务 C", "任务 D" };
        selected = AppLogic.ReconcileTaskSelection(ReminderState.Busy, reordered,
            previousIndex: 1, previouslySelectedTitle: "任务 B",
            selectNewestTask: false, preferredTaskIndex: -1);
        Assert.Equal(0, selected);
    }

    [Fact]
    public void LatestEventFocusAndSpriteMappingsAreDeterministic()
    {
        var titles = new[] { "任务 A", "任务 B", "任务 C" };
        Assert.Equal(2, AppLogic.ReconcileTaskSelection(ReminderState.Busy, titles,
            previousIndex: 0, previouslySelectedTitle: "任务 A",
            selectNewestTask: true, preferredTaskIndex: -1));
        Assert.Equal(1, AppLogic.ReconcileTaskSelection(ReminderState.Busy, titles,
            previousIndex: 0, previouslySelectedTitle: "任务 A",
            selectNewestTask: true, preferredTaskIndex: 1));
        Assert.Equal(0, AppLogic.ReconcileTaskSelection(ReminderState.Completed, titles,
            previousIndex: 2, previouslySelectedTitle: "任务 C",
            selectNewestTask: false, preferredTaskIndex: -1));

        Assert.Equal(0, AppLogic.SelectFloatingSpriteRow(ReminderState.Idle));
        Assert.Equal(1, AppLogic.SelectFloatingSpriteRow(ReminderState.Completed));
        Assert.Equal(2, AppLogic.SelectFloatingSpriteRow(ReminderState.Busy));
        Assert.Equal(3, AppLogic.SelectFloatingSpriteRow(ReminderState.Error));
        Assert.InRange(AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Error, 0), 0, 3);
        Assert.InRange(AppLogic.SelectDockSpriteIndex(DockEdge.Right, ReminderState.Error, 0), 4, 7);
    }

    [Fact]

    public void DockVisibilityAndInteractionRulesRemainPrototypeCompatible()

    {

        var start = new DateTimeOffset(2026, 8, 2, 0, 0, 0, TimeSpan.Zero);

        Assert.True(AppLogic.ShouldShowDock(start, start.AddSeconds(9.9), false, false,
            DateTimeOffset.MinValue, 10));
        Assert.False(AppLogic.ShouldShowDock(start, start.AddSeconds(10.1), false, false,
            DateTimeOffset.MinValue, 10));
        Assert.True(AppLogic.ShouldShowDock(start, start.AddHours(1), false, false,
            DateTimeOffset.MinValue, 0));

        var expired = start.AddSeconds(20);

        Assert.True(AppLogic.ShouldShowThoughtBubble(false, ReminderState.Idle, expired, DateTimeOffset.MinValue));

        Assert.False(AppLogic.ShouldShowThoughtBubble(true, ReminderState.Idle, expired, expired.AddSeconds(5)));

        Assert.True(AppLogic.ShouldShowThoughtBubble(true, ReminderState.Busy, expired, expired.AddSeconds(5)));

        Assert.False(AppLogic.ShouldShowThoughtBubble(true, ReminderState.Busy,

            expired.AddSeconds(6), expired.AddSeconds(5)));

        Assert.True(AppLogic.ShouldShowDock(start, expired, true, false, DateTimeOffset.MinValue, 10));

        Assert.True(AppLogic.ShouldShowDock(start, expired, false, true, DateTimeOffset.MinValue, 10));

        Assert.True(AppLogic.ShouldShowDock(start, expired, false, false, expired.AddSeconds(2), 10));

        Assert.False(AppLogic.ShouldShowDock(start, expired, false, false, DateTimeOffset.MinValue, 10));

    }

    [Fact]

    public void DockHotspotsAndCloudSwitchAreasStayLocalAndPredictable()

    {

        var work = new RectD(100, 50, 1200, 800);

        var left = AppLogic.DockHoverBounds(DockEdge.Left, work, 400, 1, true, 240);

        var right = AppLogic.DockHoverBounds(DockEdge.Right, work, 400, 1, true, 240);

        Assert.True(left.Contains(new PointD(101, 510)));

        Assert.False(left.Contains(new PointD(101, 530)));

        Assert.False(left.Contains(new PointD(200, 400)));

        Assert.True(right.Contains(new PointD(1299, 400)));

        Assert.False(right.Contains(new PointD(1200, 400)));

        var bubble = new RectD(10, 20, 300, 150);

        var content = new RectD(100, 70, 150, 60);

        var edgePoint = new PointD(15, 25);

        Assert.True(AppLogic.IsTaskSwitchPoint(true, true, ReminderState.Busy, 2, bubble, content, edgePoint));

        Assert.False(AppLogic.IsTaskSwitchPoint(false, true, ReminderState.Busy, 2, bubble, content, edgePoint));

        Assert.False(AppLogic.IsTaskSwitchPoint(true, false, ReminderState.Busy, 2, bubble, content, edgePoint));

        Assert.False(AppLogic.IsTaskSwitchPoint(true, true, ReminderState.Busy, 1, bubble, content, edgePoint));

    }

    [Fact]

    public void ErrorTextAndDockExpressionsMatchTheOriginalContract()

    {

        Assert.Equal("任务失败：构建安装包", AppLogic.FormatAbnormalTaskText("构建安装包"));

        Assert.Equal("任务失败：未知任务", AppLogic.FormatAbnormalTaskText(null));

        Assert.Equal(1, AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Busy, 11));
        Assert.Equal(0, AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Busy, 12));
        Assert.Equal(1, AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Busy, 14));
        Assert.Equal(2, AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Completed, 0));
        Assert.Equal(3, AppLogic.SelectDockSpriteIndex(DockEdge.Left, ReminderState.Error, 0));

    }

}
