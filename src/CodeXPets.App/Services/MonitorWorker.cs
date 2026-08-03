using CodeXPets.Core.Monitoring;
using System.Diagnostics;

namespace CodeXPets.App.Services;

public enum MonitorEventKind
{
    StateChanged,
    TaskStarted,
    TaskCompleted,
    TaskAborted
}

public sealed record MonitorSnapshot(
    int ActiveCount,
    IReadOnlyList<string> ActiveTitles,
    IReadOnlyList<string?> ActivePlanProgressLabels,
    int TotalPlanStepCount,
    int CompletedPlanStepCount,
    string? LastCompletedTitle,
    string? LastAbortedTitle,
    string? LastEventType,
    int LatestEventActiveTitleIndex,
    string DiagnosticsText)
{
    public static MonitorSnapshot Empty { get; } = new(
        0,
        [],
        [],
        0,
        0,
        null,
        null,
        null,
        -1,
        "JSONL 监控尚未启动。");
}

public sealed record MonitorUpdate(
    IReadOnlyList<MonitorEventKind> Events,
    MonitorSnapshot Snapshot);

/// <summary>
/// Watches Codex JSONL session files and performs periodic recovery polling when file-system
/// notifications are unavailable or lost.
/// </summary>
public sealed class MonitorWorker : IDisposable
{
    private static readonly TimeSpan RecoveryPollInterval = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan EventDebounceInterval = TimeSpan.FromMilliseconds(25);
    private static readonly TimeSpan MinimumPollInterval = TimeSpan.FromMilliseconds(250);

    private readonly CancellationTokenSource _cancellation = new();
    private readonly SemaphoreSlim _changeSignal = new(0, 1);
    private readonly string _sessionsRoot;

    private Task? _workerTask;
    private FileSystemWatcher? _sessionWatcher;
    private int _changePending;
    private int _synchronizationResourcesDisposed;
    private bool _started;
    private bool _disposed;

    public MonitorWorker(string sessionsRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sessionsRoot);
        _sessionsRoot = Path.GetFullPath(sessionsRoot);
    }

    public event EventHandler<MonitorUpdate>? Updated;

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_started) return;

        _started = true;
        _workerTask = Task.Run(RunAsync);
    }

    private async Task RunAsync()
    {
        var cancellationToken = _cancellation.Token;
        CodexSessionMonitor? monitor = null;

        try
        {
            monitor = new CodexSessionMonitor(_sessionsRoot);
            var pendingEvents = new List<MonitorEventKind>();
            monitor.TaskStarted += (_, _) => pendingEvents.Add(MonitorEventKind.TaskStarted);
            monitor.TaskCompleted += (_, _) => pendingEvents.Add(MonitorEventKind.TaskCompleted);
            monitor.TaskAborted += (_, _) => pendingEvents.Add(MonitorEventKind.TaskAborted);
            monitor.StateChanged += (_, _) => pendingEvents.Add(MonitorEventKind.StateChanged);

            PostUpdate([MonitorEventKind.StateChanged], Capture(monitor));
            long lastPollTimestamp = 0;

            while (!cancellationToken.IsCancellationRequested)
            {
                EnsureWatcher();
                var signaled = await _changeSignal.WaitAsync(RecoveryPollInterval, cancellationToken)
                    .ConfigureAwait(false);
                Interlocked.Exchange(ref _changePending, 0);

                if (signaled)
                {
                    await Task.Delay(EventDebounceInterval, cancellationToken).ConfigureAwait(false);
                }

                if (lastPollTimestamp != 0)
                {
                    var remaining = MinimumPollInterval - Stopwatch.GetElapsedTime(lastPollTimestamp);
                    if (remaining > TimeSpan.Zero)
                    {
                        await Task.Delay(remaining, cancellationToken).ConfigureAwait(false);
                    }
                }

                try
                {
                    monitor.Poll();
                }
                catch (Exception exception)
                {
                    monitor.ReportUnexpectedError("读取 Codex 会话", exception);
                    pendingEvents.Add(MonitorEventKind.StateChanged);
                }

                lastPollTimestamp = Stopwatch.GetTimestamp();
                // Monitoring produces many file-change notifications that do not alter
                // any UI-visible state. Avoid allocating a snapshot and posting an
                // empty update to the UI thread for each of those notifications.
                if (pendingEvents.Count > 0)
                {
                    PostUpdate(pendingEvents.ToArray(), Capture(monitor));
                    pendingEvents.Clear();
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            PostUpdate(
                [MonitorEventKind.StateChanged],
                MonitorSnapshot.Empty with
                {
                    DiagnosticsText = "JSONL 监听启动失败：" + exception.Message
                });
        }
        finally
        {
            DisposeWatcher();
            monitor?.Dispose();
        }
    }

    private void EnsureWatcher()
    {
        if (_disposed || _sessionWatcher is not null || !Directory.Exists(_sessionsRoot)) return;

        FileSystemWatcher? watcher = null;
        try
        {
            watcher = new FileSystemWatcher(_sessionsRoot, "*.jsonl")
            {
                IncludeSubdirectories = true,
                NotifyFilter = NotifyFilters.FileName |
                               NotifyFilters.CreationTime |
                               NotifyFilters.LastWrite |
                               NotifyFilters.Size,
                InternalBufferSize = 16 * 1024
            };
            watcher.Changed += OnFileChanged;
            watcher.Created += OnFileChanged;
            watcher.Deleted += OnFileChanged;
            watcher.Renamed += OnFileRenamed;
            watcher.Error += OnWatcherError;
            _sessionWatcher = watcher;
            watcher.EnableRaisingEvents = true;
        }
        catch (Exception)
        {
            if (ReferenceEquals(_sessionWatcher, watcher))
            {
                _sessionWatcher = null;
            }
            DisposeWatcherInstance(watcher);
            // Recovery polling remains active even when a watcher cannot be created.
        }
    }

    private void OnFileChanged(object sender, FileSystemEventArgs eventArgs) => SignalChange();

    private void OnFileRenamed(object sender, RenamedEventArgs eventArgs) => SignalChange();

    private void OnWatcherError(object sender, ErrorEventArgs eventArgs)
    {
        DisposeWatcher();
        SignalChange();
    }

    private void SignalChange()
    {
        if (_disposed || Interlocked.Exchange(ref _changePending, 1) != 0) return;

        try
        {
            _changeSignal.Release();
        }
        catch (ObjectDisposedException)
        {
        }
        catch (SemaphoreFullException)
        {
        }
    }

    private void DisposeWatcher()
    {
        DisposeWatcherInstance(Interlocked.Exchange(ref _sessionWatcher, null));
    }

    private void DisposeWatcherInstance(FileSystemWatcher? watcher)
    {
        if (watcher is null) return;

        try
        {
            watcher.EnableRaisingEvents = false;
        }
        catch (ObjectDisposedException)
        {
        }
        watcher.Changed -= OnFileChanged;
        watcher.Created -= OnFileChanged;
        watcher.Deleted -= OnFileChanged;
        watcher.Renamed -= OnFileRenamed;
        watcher.Error -= OnWatcherError;
        watcher.Dispose();
    }

    private static MonitorSnapshot Capture(CodexSessionMonitor monitor)
    {
        var sourcePath = monitor.LastEventFile;
        return new MonitorSnapshot(
            monitor.ActiveCount,
            monitor.ActiveTitles.ToArray(),
            monitor.ActivePlanProgressLabels.ToArray(),
            monitor.TotalPlanStepCount,
            monitor.CompletedPlanStepCount,
            monitor.LastCompletedTitle,
            monitor.LastAbortedTitle,
            monitor.LastEventType,
            monitor.GetActiveTitleIndex(sourcePath),
            monitor.GetDiagnosticsText());
    }

    private void PostUpdate(IReadOnlyList<MonitorEventKind> events, MonitorSnapshot snapshot)
    {
        if (!_disposed)
        {
            Updated?.Invoke(this, new MonitorUpdate(events, snapshot));
        }
    }

    public void Dispose()
    {
        if (_disposed) return;

        _disposed = true;
        _cancellation.Cancel();
        DisposeWatcher();

        var workerTask = _workerTask;
        if (workerTask is null)
        {
            DisposeSynchronizationResources();
            return;
        }

        try
        {
            workerTask.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException)
        {
            // The worker reports operational failures through MonitorSnapshot diagnostics.
        }

        if (workerTask.IsCompleted)
        {
            DisposeSynchronizationResources();
        }
        else
        {
            _ = workerTask.ContinueWith(
                _ => DisposeSynchronizationResources(),
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }
    }

    private void DisposeSynchronizationResources()
    {
        if (Interlocked.Exchange(ref _synchronizationResourcesDisposed, 1) != 0) return;

        _changeSignal.Dispose();
        _cancellation.Dispose();
    }
}

