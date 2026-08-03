using CodeXPets.Core.Domain;

namespace CodeXPets.Core.Application;

public sealed class VisualStateCoordinator
{
    private DateTimeOffset _completedUntil = DateTimeOffset.MinValue;
    private DateTimeOffset _abnormalUntil = DateTimeOffset.MinValue;

    public ReminderState LatestChangedState { get; private set; } = ReminderState.Idle;
    public bool ShowNewestTaskOnNextRefresh { get; private set; }

    public void RecordStarted()
    {
        LatestChangedState = ReminderState.Busy;
        ShowNewestTaskOnNextRefresh = true;
        _completedUntil = DateTimeOffset.MinValue;
        _abnormalUntil = DateTimeOffset.MinValue;
    }

    public void RecordCompleted(DateTimeOffset now, TimeSpan visibleFor)
    {
        LatestChangedState = ReminderState.Completed;
        ShowNewestTaskOnNextRefresh = false;
        _completedUntil = now + visibleFor;
        _abnormalUntil = DateTimeOffset.MinValue;
    }

    public void RecordAborted(DateTimeOffset now, TimeSpan visibleFor)
    {
        LatestChangedState = ReminderState.Error;
        ShowNewestTaskOnNextRefresh = false;
        _abnormalUntil = now + visibleFor;
        _completedUntil = DateTimeOffset.MinValue;
    }

    public ReminderState Select(int activeCount, DateTimeOffset now) =>
        AppLogic.SelectVisualState(
            activeCount,
            abnormalRecently: now < _abnormalUntil,
            completedRecently: now < _completedUntil,
            LatestChangedState);

    public void ConsumeNewestTaskFocus() => ShowNewestTaskOnNextRefresh = false;
}
