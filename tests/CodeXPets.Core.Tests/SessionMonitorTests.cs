using CodeXPets.Core.Monitoring;
using System.Text;

namespace CodeXPets.Core.Tests;

public sealed class SessionMonitorTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(),
        "CodeXPetsTests_" + Guid.NewGuid().ToString("N"));

    [Fact]
    public void TracksStartTitlePlanCompletionAndAbort()
    {
        var file = CreateSessionFile("rollout-main.jsonl",
            "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n" +
            "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Hello title test\"}}\n");

        using var monitor = new CodexSessionMonitor(_root);
        var completed = 0;
        var aborted = 0;
        monitor.TaskCompleted += (_, _) => completed++;
        monitor.TaskAborted += (_, _) => aborted++;

        Assert.Equal(1, monitor.ActiveCount);
        Assert.Equal("Hello title test", monitor.PrimaryActiveTitle);

        File.AppendAllText(file,
            "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"response_item\",\"payload\":{" +
            "\"type\":\"function_call\",\"name\":\"update_plan\"," +
            "\"arguments\":\"{\\\"plan\\\":[{\\\"step\\\":\\\"Inspect\\\",\\\"status\\\":\\\"completed\\\"}," +
            "{\\\"step\\\":\\\"Build feature\\\",\\\"status\\\":\\\"in_progress\\\"}," +
                "{\\\"step\\\":\\\"Test\\\",\\\"status\\\":\\\"pending\\\"}]}\"," +
            "\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"A\"}}}\n", Encoding.UTF8);
        monitor.Poll();

        Assert.Equal(3, monitor.TotalPlanStepCount);
        Assert.Equal(1, monitor.CompletedPlanStepCount);
        Assert.Equal("Build feature", monitor.PrimaryCurrentPlanStep);
        Assert.Equal("1/3", Assert.Single(monitor.ActivePlanProgressLabels));

        File.AppendAllText(file,
            "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\",\"last_agent_message\":\"ok\"}}\n",
            Encoding.UTF8);
        monitor.Poll();
        Assert.Equal(0, monitor.ActiveCount);
        Assert.Equal(1, completed);
        Assert.Equal("Hello title test", monitor.LastCompletedTitle);

        File.AppendAllText(file,
            "{\"timestamp\":\"2026-08-01T00:00:05Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n" +
            "{\"timestamp\":\"2026-08-01T00:00:06Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Abort title test\"}}\n" +
            "{\"timestamp\":\"2026-08-01T00:00:07Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"B\"}}\n",
            Encoding.UTF8);
        monitor.Poll();
        Assert.Equal(0, monitor.ActiveCount);
        Assert.Equal(1, aborted);
        Assert.Equal("Abort title test", monitor.LastAbortedTitle);
    }

    [Fact]
    public void IgnoresQuotedToolOutputAndDetectsFailureCompletion()
    {
        var file = CreateSessionFile("rollout-failure.jsonl",
            "{\"type\":\"response_item\",\"payload\":{\"text\":\"event_msg task_started fake\"}}\n" +
            "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"F\"}}\n" +
            "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Failure case\"}}\n");
        using var monitor = new CodexSessionMonitor(_root);
        var completed = 0;
        var aborted = 0;
        monitor.TaskCompleted += (_, _) => completed++;
        monitor.TaskAborted += (_, _) => aborted++;

        File.AppendAllText(file,
            "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"F\",\"last_agent_message\":\"Internal server error\"}}\n",
            Encoding.UTF8);
        monitor.Poll();

        Assert.Equal(0, monitor.ActiveCount);
        Assert.Equal(0, completed);
        Assert.Equal(1, aborted);
        Assert.Equal("Failure case", monitor.LastAbortedTitle);
        Assert.True(CodexSessionMonitor.LooksLikeFailureMessage("HTTP status 503"));
        Assert.False(CodexSessionMonitor.LooksLikeFailureMessage("Fixed server error handling successfully"));
    }

    [Fact]
    public void KeepsActiveTurnWhenOpenJsonlHasFrozenLastWriteTime()
    {
        var started = DateTimeOffset.UtcNow.AddMinutes(-20);
        var recentActivity = DateTimeOffset.UtcNow.AddMinutes(-1);
        var file = CreateSessionFile("rollout-open-file.jsonl",
            $"{{\"timestamp\":\"{started:O}\",\"type\":\"event_msg\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"OPEN\"}}}}\n" +
            $"{{\"timestamp\":\"{started.AddSeconds(1):O}\",\"type\":\"event_msg\",\"payload\":{{\"type\":\"user_message\",\"message\":\"Still running\"}}}}\n" +
            $"{{\"timestamp\":\"{recentActivity:O}\",\"type\":\"response_item\",\"payload\":{{\"type\":\"reasoning\",\"summary\":[]}}}}\n");
        File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddMinutes(-30));

        using var monitor = new CodexSessionMonitor(_root);
        Assert.Equal(1, monitor.ActiveCount);
        monitor.Poll();

        Assert.Equal(1, monitor.ActiveCount);
        Assert.Equal("Still running", monitor.PrimaryActiveTitle);
    }

    [Fact]
    public void EmitsLifecycleEventsInFileOrder()
    {
        var file = CreateSessionFile("rollout-order.jsonl", string.Empty);
        using var monitor = new CodexSessionMonitor(_root);
        var events = new List<string>();
        monitor.TaskStarted += (_, _) => events.Add("started");
        monitor.TaskCompleted += (_, _) => events.Add("completed");
        monitor.TaskAborted += (_, _) => events.Add("aborted");

        File.AppendAllText(file,
            "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n" +
            "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\"}}\n" +
            "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n",
            Encoding.UTF8);
        monitor.Poll();

        Assert.Equal(new[] { "started", "completed", "started" }, events);
        Assert.Equal(1, monitor.ActiveCount);
    }
    private string CreateSessionFile(string name, string content)
    {
        var day = Path.Combine(_root, DateTime.Today.ToString("yyyy"),
            DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
        Directory.CreateDirectory(day);
        var file = Path.Combine(day, name);
        File.WriteAllText(file, content, Encoding.UTF8);
        return file;
    }

    public void Dispose()
    {
        if (Directory.Exists(_root)) Directory.Delete(_root, true);
    }
}


