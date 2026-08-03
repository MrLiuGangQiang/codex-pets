#nullable disable
using CodeXPets.Core.Configuration;
using CodeXPets.Core.Infrastructure;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace CodeXPets.Core.Monitoring;

public sealed class CodexSessionMonitor : IDisposable
{
    private const int ReadBufferSize = 65536;
    private const int MaximumBufferedLineCharacters = 262144;
    private const int MaximumTrackedFiles = 40;
    private const int FullDiscoveryIntervalSeconds = 600;
    private const int StaleTurnGraceSeconds = 600;
    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;
    private static readonly StringComparison PathComparison = OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
    private static readonly Regex HttpServerErrorRegex = new Regex(
        @"\bhttp(?: status)?\s*5\d\d\b",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex FailedTestsRegex = new Regex(
        @"\b[1-9]\d*\s+test\(s\)\s+failed\b",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly Regex NonZeroExitCodeRegex = new Regex(
        @"(?:^|[\r\n])\s*exit code:\s*[1-9]\d*\b",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    private static readonly string[] FailureMessagePrefixes =
    {
        "traceback (most recent call last):",
        "unhandled exception",
        "fatal error",
        "internal server error",
        "server error",
        "api error",
        "request failed",
        "stream error",
        "something went wrong",
        "服务端错误",
        "服务器错误",
        "内部服务器错误",
        "请求失败"
    };
    private static readonly string[] FailureMessageFragments =
    {
        "stream disconnected before completion",
        "connection reset by peer",
        "upstream service error",
        "service unavailable",
        "bad gateway",
        "gateway timeout",
        "rate limit exceeded",
        "error sending request"
    };
    private static readonly HashSet<string> FailureEventTypes = new HashSet<string>(
        new[] { "turn_aborted", "task_failed", "turn_failed", "stream_error", "request_error", "error" },
        StringComparer.Ordinal);
    private static readonly LegacyJsonSerializer Json = new LegacyJsonSerializer();
    private readonly string sessionsRoot;
    private readonly Dictionary<string, TailState> files = new Dictionary<string, TailState>(PathComparer);
    private readonly Dictionary<string, ActiveTurn> activeTurns = new Dictionary<string, ActiveTurn>(StringComparer.Ordinal);
    private readonly Dictionary<string, TaskPlanProgress> plansByFile =
        new Dictionary<string, TaskPlanProgress>(PathComparer);
    private string lastCompletedTitle;
    private string lastAbortedTitle;
    private string lastReadFile;
    private string lastEventType;
    private string lastEventFile;
    private string lastError;
    private long nextStartedSequence;
    private int parseErrorCount;
    private int readErrorCount;
    private int staleTurnCleanupCount;
    private DateTime lastDiscoveryUtc = DateTime.MinValue;
    private DateTime lastReadUtc = DateTime.MinValue;
    private DateTime lastEventUtc = DateTime.MinValue;
    private DateTime lastPollUtc = DateTime.MinValue;
    private DateTime nextDiscoveryUtc = DateTime.MinValue;
    private DateTime nextFullDiscoveryUtc = DateTime.MinValue;
    private DateTime nextStaleTurnCheckUtc = DateTime.MinValue;
    private bool disposed;

    public event EventHandler TaskStarted;
    public event EventHandler TaskCompleted;
    public event EventHandler TaskAborted;
    public event EventHandler StateChanged;
    public CodexSessionMonitor() : this(CodexPaths.GetDefaultSessionsRoot()) { }
    public CodexSessionMonitor(string sessionsRootPath)
    {
        sessionsRoot = CodexPaths.NormalizeSessionsRoot(sessionsRootPath);
        DiscoverFiles(true);
    }
    public int ActiveCount { get { return activeTurns.Count; } }
    public string LastCompletedTitle { get { return lastCompletedTitle; } }
    public string LastAbortedTitle { get { return lastAbortedTitle; } }
    public string LastEventType { get { return lastEventType; } }
    public string LastEventFile { get { return lastEventFile; } }
    public DateTime LastEventUtc { get { return lastEventUtc; } }

    public int GetActiveTitleIndex(string sourcePath)
    {
        if (String.IsNullOrEmpty(sourcePath)) return -1;
        int index = 0;
        foreach (ActiveTurn turn in OrderedActiveTurns())
        {
            if (String.Equals(turn.SourcePath, sourcePath, PathComparison))
                return index;
            index++;
        }
        return -1;
    }

    public IList<string> ActiveTitles
    {
        get
        {
            List<string> titles = new List<string>();
            foreach (ActiveTurn turn in OrderedActiveTurns())
                titles.Add(String.IsNullOrEmpty(turn.Title) ? "正在处理任务…" : turn.Title);
            return titles;
        }
    }

    public IList<string> ActivePlanProgressLabels
    {
        get
        {
            List<string> labels = new List<string>();
            foreach (ActiveTurn turn in OrderedActiveTurns())
                labels.Add(turn.Plan == null || turn.Plan.TotalSteps <= 1 ? null :
                    turn.Plan.CompletedSteps + "/" + turn.Plan.TotalSteps);
            return labels;
        }
    }

    public int TotalPlanStepCount
    {
        get { return activeTurns.Values.Sum(delegate (ActiveTurn turn) { return turn.Plan == null ? 0 : turn.Plan.TotalSteps; }); }
    }

    public int CompletedPlanStepCount
    {
        get { return activeTurns.Values.Sum(delegate (ActiveTurn turn) { return turn.Plan == null ? 0 : turn.Plan.CompletedSteps; }); }
    }

    public string PrimaryCurrentPlanStep
    {
        get
        {
            foreach (ActiveTurn turn in OrderedActiveTurns())
                if (turn.Plan != null && turn.Plan.TotalSteps > 0) return turn.Plan.CurrentStep;
            return null;
        }
    }

    private IEnumerable<ActiveTurn> OrderedActiveTurns()
    {
        return activeTurns.Values.OrderBy(delegate (ActiveTurn item) { return item.StartSequence; });
    }

    public string PrimaryActiveTitle
    {
        get
        {
            foreach (ActiveTurn turn in OrderedActiveTurns())
                if (!String.IsNullOrEmpty(turn.Title)) return turn.Title;
            return null;
        }
    }

    public string GetDiagnosticsText()
    {
        StringBuilder builder = new StringBuilder();
        builder.AppendLine("会话监听");
        builder.AppendLine("  目录：" + sessionsRoot);
        builder.AppendLine("  目录存在：" + (Directory.Exists(sessionsRoot) ? "是" : "否"));
        builder.AppendLine("  已跟踪文件：" + files.Count);
        builder.AppendLine("  活跃任务：" + activeTurns.Count);
        builder.AppendLine("  最近轮询：" + FormatUtc(lastPollUtc));
        builder.AppendLine("  最近扫描：" + FormatUtc(lastDiscoveryUtc));
        builder.AppendLine("  最近读取：" + FormatUtc(lastReadUtc));
        if (!String.IsNullOrEmpty(lastReadFile))
            builder.AppendLine("  最近读取文件：" + lastReadFile);
        builder.AppendLine("  最近事件：" +
            (String.IsNullOrEmpty(lastEventType) ? "无" : lastEventType + " · " + FormatUtc(lastEventUtc)));
        if (!String.IsNullOrEmpty(lastEventFile))
            builder.AppendLine("  最近事件文件：" + lastEventFile);
        builder.AppendLine("  JSON 解析错误：" + parseErrorCount);
        builder.AppendLine("  文件读取错误：" + readErrorCount);
        builder.AppendLine("  过期任务清理：" + staleTurnCleanupCount);
        if (!String.IsNullOrEmpty(lastError)) builder.AppendLine("  最近错误：" + lastError);
        if (activeTurns.Count > 0)
        {
            builder.AppendLine();
            builder.AppendLine("活跃任务明细");
            foreach (ActiveTurn turn in OrderedActiveTurns())
            {
                builder.AppendLine("  " + turn.TurnId + " | " +
                    (String.IsNullOrEmpty(turn.Title) ? "未命名任务" : turn.Title));
                builder.AppendLine("    最近活动：" + FormatUtc(turn.LastActivityUtc));
                builder.AppendLine("    文件：" + turn.SourcePath);
            }
        }
        return builder.ToString();
    }

    public void ReportUnexpectedError(string operation, Exception exception)
    {
        readErrorCount++;
        lastError = operation + "：" + (exception == null ? "未知错误" : exception.Message);
    }

    private static string FormatUtc(DateTime value)
    {
        return value == DateTime.MinValue ? "无" : value.ToLocalTime().ToString("yyyy-MM-dd HH:mm:ss",
            CultureInfo.InvariantCulture);
    }

    public void Poll()
    {
        if (disposed) return;
        DateTime now = DateTime.UtcNow;
        if (now >= nextDiscoveryUtc)
        {
            DiscoverFiles(false);
            nextDiscoveryUtc = now.AddSeconds(1.2);
        }
        foreach (TailState state in files.Values) ReadNewBytes(state, false);
        if (now >= nextStaleTurnCheckUtc)
        {
            nextStaleTurnCheckUtc = now.AddSeconds(30);
            ClearStaleTurns(now);
        }
        lastPollUtc = now;
    }

    private void DiscoverFiles(bool initial)
    {
        DateTime now = DateTime.UtcNow;
        lastDiscoveryUtc = now;
        if (!Directory.Exists(sessionsRoot)) return;

        // Keep only the newest tracked-file candidates while enumerating. A sessions
        // directory can accumulate many historical JSONL files; building and sorting
        // a list containing every path caused periodic CPU and memory spikes.
        PriorityQueue<string, long> newestCandidates = new PriorityQueue<string, long>();
        bool runFullDiscovery = initial || now >= nextFullDiscoveryUtc;
        if (runFullDiscovery)
        {
            nextFullDiscoveryUtc = now.AddSeconds(FullDiscoveryIntervalSeconds);
            try
            {
                AddNewestCandidates(newestCandidates, Directory.EnumerateFiles(sessionsRoot,
                    "*.jsonl", SearchOption.AllDirectories));
            }
            catch (Exception ex) { ReportUnexpectedError("完整扫描会话目录", ex); }
        }
        else
        {
            DateTime today = DateTime.Today;
            for (int daysAgo = 0; daysAgo <= 2; daysAgo++)
            {
                DateTime day = today.AddDays(-daysAgo);
                string dayFolder = Path.Combine(sessionsRoot, day.ToString("yyyy"),
                    day.ToString("MM"), day.ToString("dd"));
                if (!Directory.Exists(dayFolder)) continue;
                try
                {
                    AddNewestCandidates(newestCandidates, Directory.EnumerateFiles(dayFolder,
                        "*.jsonl", SearchOption.TopDirectoryOnly));
                }
                catch (Exception ex) { ReportUnexpectedError("扫描日期目录", ex); }
            }
        }

        while (newestCandidates.TryDequeue(out string path, out _))
        {
            if (files.ContainsKey(path)) continue;
            TailState state = new TailState(path);
            files[path] = state;
            ReadNewBytes(state, initial);
        }
        PruneTrackedFiles();
    }

    private static void AddNewestCandidates(PriorityQueue<string, long> candidates,
        IEnumerable<string> paths)
    {
        foreach (string path in paths)
        {
            long timestamp = SafeLastWriteTimeUtc(path).Ticks;
            if (candidates.Count < MaximumTrackedFiles)
            {
                candidates.Enqueue(path, timestamp);
            }
            else if (candidates.TryPeek(out _, out long oldestTimestamp) && timestamp > oldestTimestamp)
            {
                candidates.Dequeue();
                candidates.Enqueue(path, timestamp);
            }
        }
    }

    private void PruneTrackedFiles()
    {
        if (files.Count <= MaximumTrackedFiles) return;
        HashSet<string> activePaths = new HashSet<string>(activeTurns.Values
            .Select(delegate (ActiveTurn turn) { return turn.SourcePath; }),
            PathComparer);
        List<TailState> removable = files.Values
            .Where(delegate (TailState state) { return !activePaths.Contains(state.Path); })
            .OrderBy(delegate (TailState state) { return state.LastActivityUtc; }).ToList();
        foreach (TailState state in removable)
        {
            if (files.Count <= MaximumTrackedFiles) break;
            files.Remove(state.Path);
            plansByFile.Remove(state.Path);
        }
    }

    private static DateTime SafeLastWriteTimeUtc(string path)
    {
        try { return File.GetLastWriteTimeUtc(path); }
        catch { return DateTime.MinValue; }
    }

    private void ReadNewBytes(TailState state, bool suppressCompletionNotification)
    {
        try
        {
            FileInfo info = new FileInfo(state.Path);
            if (!info.Exists) return;
            long length = info.Length;
            DateTime writeUtc = info.LastWriteTimeUtc;
            if (length < state.Position)
            {
                RemoveTurnsForFile(state.Path);
                plansByFile.Remove(state.Path);
                state.Reset();
            }
            if (length == state.Position && writeUtc <= state.LastWriteUtc) return;
            if (length == state.Position)
            {
                state.LastWriteUtc = writeUtc;
                return;
            }

            using (FileStream stream = new FileStream(state.Path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete))
            {
                stream.Position = state.Position;
                int read;
                while ((read = stream.Read(state.ByteBuffer, 0, state.ByteBuffer.Length)) > 0)
                {
                    int consumed = 0;
                    while (consumed < read)
                    {
                        int bytesUsed;
                        int charsUsed;
                        bool completed;
                        state.Utf8Decoder.Convert(state.ByteBuffer, consumed, read - consumed,
                            state.CharacterBuffer, 0, state.CharacterBuffer.Length, false,
                            out bytesUsed, out charsUsed, out completed);
                        if (charsUsed > 0)
                            ConsumeText(state, new string(state.CharacterBuffer, 0, charsUsed),
                                suppressCompletionNotification);
                        consumed += bytesUsed;
                        if (bytesUsed == 0 && charsUsed == 0) break;
                    }
                    state.Position += read;
                }
            }
            DateTime readUtc = DateTime.UtcNow;
            // Codex can keep a JSONL file open for a long time. On Windows the
            // directory entry's LastWriteTime may then remain unchanged even while
            // the file length grows. A successful incremental read is direct proof
            // of live activity, so use the read time for normal polling. Historical
            // startup scans still use the file timestamp, preventing abandoned turns
            // from looking newly active just because CodeXPets was launched.
            DateTime activityUtc = suppressCompletionNotification && writeUtc != DateTime.MinValue
                ? writeUtc : readUtc;
            state.LastWriteUtc = writeUtc;
            state.LastActivityUtc = activityUtc;
            lastReadFile = state.Path;
            lastReadUtc = readUtc;
            TouchTurnsForFile(state.Path, activityUtc);
        }
        catch (IOException ex) { ReportUnexpectedError("读取会话文件", ex); }
        catch (UnauthorizedAccessException ex) { ReportUnexpectedError("访问会话文件", ex); }
    }

    private void ConsumeText(TailState state, string text, bool suppressCompletionNotification)
    {
        int offset = 0;
        while (offset < text.Length)
        {
            int newline = text.IndexOf('\n', offset);
            bool hasNewline = newline >= 0;
            int end = hasNewline ? newline : text.Length;
            int count = end - offset;
            if (!state.SkipCurrentLine && count > 0)
            {
                int remainingCapacity = MaximumBufferedLineCharacters - state.LineBuffer.Length;
                if (remainingCapacity > 0)
                {
                    int appendCount = Math.Min(remainingCapacity, count);
                    state.LineBuffer.Append(text, offset, appendCount);
                }
                if (!state.EventHandled)
                {
                    string candidate = state.LineBuffer.ToString();
                    if (TryProcessEvent(candidate, state.Path, suppressCompletionNotification))
                    {
                        state.EventHandled = true;
                        state.LineBuffer.Length = 0;
                        if (!hasNewline) state.SkipCurrentLine = true;
                    }
                    else if (state.LineBuffer.Length >= MaximumBufferedLineCharacters)
                    {
                        state.SkipCurrentLine = true;
                        state.LineBuffer.Length = 0;
                    }
                }
            }
            if (hasNewline)
            {
                if (!state.EventHandled && !state.SkipCurrentLine && state.LineBuffer.Length > 0)
                    TryProcessEvent(state.LineBuffer.ToString(), state.Path, suppressCompletionNotification);
                state.LineBuffer.Length = 0;
                state.SkipCurrentLine = false;
                state.EventHandled = false;
                offset = newline + 1;
            }
            else offset = text.Length;
        }
    }

    private bool TryProcessEvent(string lineStart, string sourcePath, bool suppressCompletionNotification)
    {
        lineStart = NormalizeJsonLine(lineStart);
        if (TryProcessPlanUpdate(lineStart, sourcePath, suppressCompletionNotification)) return true;
        if (lineStart.IndexOf("event_msg", StringComparison.Ordinal) < 0)
        {
            DateTime activityUtc;
            if (LooksLikeCompleteJson(lineStart) && TryGetLeadingEventUtc(lineStart, out activityUtc))
            {
                // Recent Codex builds can append to an open JSONL file without
                // changing LastWriteTime. Generic response_item records are still
                // direct evidence that the active turn in this file is alive.
                TouchTurnsForFile(sourcePath, activityUtc);
                return true;
            }
            return false;
        }

        IDictionary<string, object> root;
        try { root = Json.DeserializeObject(lineStart) as IDictionary<string, object>; }
        catch (ArgumentException ex)
        {
            if (LooksLikeCompleteJson(lineStart)) RecordParseError("解析 event_msg", ex);
            return false;
        }
        catch (InvalidOperationException ex)
        {
            if (LooksLikeCompleteJson(lineStart)) RecordParseError("解析 event_msg", ex);
            return false;
        }
        if (root == null || !String.Equals(GetString(root, "type"), "event_msg",
            StringComparison.Ordinal)) return false;
        IDictionary<string, object> payload = GetObjectMap(root, "payload");
        if (payload == null) return true;

        string eventType = GetString(payload, "type");
        if (String.IsNullOrEmpty(eventType)) return true;
        DateTime eventUtc = GetEventUtc(root);
        RecordEvent(eventType, sourcePath);
        if (String.Equals(eventType, "user_message", StringComparison.Ordinal))
        {
            string title = GetString(payload, "message");
            if (!String.IsNullOrWhiteSpace(title))
                SetTitleForFile(sourcePath, title, eventUtc);
            return true;
        }

        string turnId = GetString(payload, "turn_id");
        if (String.IsNullOrEmpty(turnId)) turnId = GetString(root, "turn_id");
        if (String.IsNullOrEmpty(turnId))
        {
            TouchTurnsForFile(sourcePath, eventUtc);
            return true;
        }

        int before = activeTurns.Count;
        if (eventType == "task_started")
        {
            bool added = false;
            ActiveTurn turn;
            if (!activeTurns.TryGetValue(turnId, out turn))
            {
                turn = new ActiveTurn(turnId, sourcePath, nextStartedSequence++, eventUtc);
                TaskPlanProgress rememberedPlan;
                if (plansByFile.TryGetValue(sourcePath, out rememberedPlan)) turn.Plan = rememberedPlan;
                activeTurns[turnId] = turn;
                added = true;
            }
            TouchTurn(turn, eventUtc);
            if (added && !suppressCompletionNotification)
            {
                EventHandler started = TaskStarted;
                if (started != null) started(this, EventArgs.Empty);
            }
        }
        else if (eventType == "task_complete")
        {
            ActiveTurn completedTurn;
            bool wasActive = activeTurns.TryGetValue(turnId, out completedTurn);
            bool abnormalCompletion = IsAbnormalTaskCompletion(lineStart);
            if (wasActive) activeTurns.Remove(turnId);
            if (wasActive && !suppressCompletionNotification)
            {
                if (abnormalCompletion)
                {
                    lastAbortedTitle = String.IsNullOrEmpty(completedTurn.Title)
                        ? "发生异常的任务" : completedTurn.Title;
                    EventHandler aborted = TaskAborted;
                    if (aborted != null) aborted(this, EventArgs.Empty);
                }
                else
                {
                    lastCompletedTitle = String.IsNullOrEmpty(completedTurn.Title)
                        ? "已完成的任务" : completedTurn.Title;
                    EventHandler completed = TaskCompleted;
                    if (completed != null) completed(this, EventArgs.Empty);
                }
            }
        }
        else if (FailureEventTypes.Contains(eventType))
        {
            ActiveTurn abortedTurn;
            bool wasActive = activeTurns.TryGetValue(turnId, out abortedTurn);
            if (wasActive) activeTurns.Remove(turnId);
            if (wasActive && !suppressCompletionNotification)
            {
                lastAbortedTitle = String.IsNullOrEmpty(abortedTurn.Title)
                    ? "发生异常的任务" : abortedTurn.Title;
                EventHandler aborted = TaskAborted;
                if (aborted != null) aborted(this, EventArgs.Empty);
            }
        }
        else
        {
            ActiveTurn activeTurn;
            if (activeTurns.TryGetValue(turnId, out activeTurn))
                TouchTurn(activeTurn, eventUtc);
            else
                TouchTurnsForFile(sourcePath, eventUtc);
        }
        if (before != activeTurns.Count)
        {
            EventHandler changed = StateChanged;
            if (changed != null) changed(this, EventArgs.Empty);
        }
        return true;
    }

    private void RecordEvent(string eventType, string sourcePath)
    {
        lastEventType = eventType;
        lastEventFile = sourcePath;
        lastEventUtc = DateTime.UtcNow;
    }

    private void RecordParseError(string operation, Exception exception)
    {
        parseErrorCount++;
        lastError = operation + "：" + (exception == null ? "未知错误" : exception.Message);
    }

    private static bool IsAbnormalTaskCompletion(string lineStart)
    {
        try
        {
            IDictionary<string, object> root = Json.DeserializeObject(lineStart) as IDictionary<string, object>;
            IDictionary<string, object> payload = GetObjectMap(root, "payload");
            if (payload == null) return false;
            string status = GetString(payload, "status");
            if (String.Equals(status, "failed", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(status, "error", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(status, "aborted", StringComparison.OrdinalIgnoreCase) ||
                String.Equals(status, "cancelled", StringComparison.OrdinalIgnoreCase)) return true;
            object errorValue;
            if (payload.TryGetValue("error", out errorValue) && errorValue != null &&
                !String.IsNullOrWhiteSpace(errorValue.ToString())) return true;
            return LooksLikeFailureMessage(GetString(payload, "last_agent_message"));
        }
        catch (ArgumentException) { return false; }
        catch (InvalidOperationException) { return false; }
    }

    internal static bool LooksLikeFailureMessage(string message)
    {
        if (String.IsNullOrWhiteSpace(message)) return false;
        string trimmed = message.Trim();
        string lower = trimmed.ToLowerInvariant();
        foreach (string prefix in FailureMessagePrefixes)
            if (lower.StartsWith(prefix, StringComparison.Ordinal)) return true;
        foreach (string fragment in FailureMessageFragments)
            if (lower.IndexOf(fragment, StringComparison.Ordinal) >= 0) return true;

        if (HttpServerErrorRegex.IsMatch(lower)) return true;
        if (FailedTestsRegex.IsMatch(lower)) return true;
        if (NonZeroExitCodeRegex.IsMatch(lower)) return true;
        return false;
    }

    private bool TryProcessPlanUpdate(string lineStart, string sourcePath, bool suppressNotification)
    {
        if (lineStart.IndexOf("update_plan", StringComparison.Ordinal) < 0 ||
            lineStart.IndexOf("function_call", StringComparison.Ordinal) < 0) return false;
        try
        {
            IDictionary<string, object> root = Json.DeserializeObject(lineStart) as IDictionary<string, object>;
            IDictionary<string, object> payload = GetObjectMap(root, "payload");
            if (payload == null || !String.Equals(GetString(payload, "type"), "function_call", StringComparison.Ordinal) ||
                !String.Equals(GetString(payload, "name"), "update_plan", StringComparison.Ordinal)) return false;
            RecordEvent("update_plan", sourcePath);
            DateTime eventUtc = GetEventUtc(root);

            string arguments = GetString(payload, "arguments");
            IDictionary<string, object> argumentsRoot = String.IsNullOrEmpty(arguments) ? null :
                Json.DeserializeObject(arguments) as IDictionary<string, object>;
            object planValue;
            object[] planItems = argumentsRoot != null && argumentsRoot.TryGetValue("plan", out planValue)
                ? planValue as object[] : null;
            if (planItems == null) return true;

            IDictionary<string, object> metadata = GetObjectMap(payload, "internal_chat_message_metadata_passthrough");
            string turnId = GetString(metadata, "turn_id");
            ActiveTurn turn;
            if (String.IsNullOrEmpty(turnId) || !activeTurns.TryGetValue(turnId, out turn))
            {
                turn = activeTurns.Values.Where(delegate (ActiveTurn item)
                {
                    return String.Equals(item.SourcePath, sourcePath, PathComparison);
                }).OrderByDescending(delegate (ActiveTurn item) { return item.StartSequence; }).FirstOrDefault();
            }
            if (turn == null) return true;
            TouchTurn(turn, eventUtc);

            int completed = 0;
            string currentStep = null;
            string firstPendingStep = null;
            foreach (object item in planItems)
            {
                IDictionary<string, object> step = item as IDictionary<string, object>;
                if (step == null) continue;
                string status = GetString(step, "status");
                string stepText = GetString(step, "step");
                if (String.Equals(status, "completed", StringComparison.Ordinal)) completed++;
                else if (String.Equals(status, "in_progress", StringComparison.Ordinal) && String.IsNullOrEmpty(currentStep))
                    currentStep = stepText;
                else if (String.Equals(status, "pending", StringComparison.Ordinal) && String.IsNullOrEmpty(firstPendingStep))
                    firstPendingStep = stepText;
            }
            if (String.IsNullOrEmpty(currentStep)) currentStep = firstPendingStep;
            TaskPlanProgress next = new TaskPlanProgress(planItems.Length, completed, currentStep);
            plansByFile[sourcePath] = next;
            bool changed = turn.Plan == null || !turn.Plan.Equals(next);
            turn.Plan = next;
            if (changed && !suppressNotification)
            {
                EventHandler stateChanged = StateChanged;
                if (stateChanged != null) stateChanged(this, EventArgs.Empty);
            }
            return true;
        }
        catch (ArgumentException ex)
        {
            if (LooksLikeCompleteJson(lineStart)) RecordParseError("解析 update_plan", ex);
            return false;
        }
        catch (InvalidOperationException ex)
        {
            if (LooksLikeCompleteJson(lineStart)) RecordParseError("解析 update_plan", ex);
            return false;
        }
    }

    private static bool TryGetLeadingEventUtc(string value, out DateTime eventUtc)
    {
        eventUtc = DateTime.MinValue;
        if (String.IsNullOrWhiteSpace(value)) return false;

        int cursor = 0;
        while (cursor < value.Length && Char.IsWhiteSpace(value[cursor])) cursor++;
        if (cursor < value.Length && value[cursor] == '﻿') cursor++;
        while (cursor < value.Length && Char.IsWhiteSpace(value[cursor])) cursor++;
        if (cursor >= value.Length || value[cursor] != '{') return false;
        cursor++;
        while (cursor < value.Length && Char.IsWhiteSpace(value[cursor])) cursor++;

        const string timestampKey = "\"timestamp\"";
        if (cursor + timestampKey.Length > value.Length ||
            String.CompareOrdinal(value, cursor, timestampKey, 0, timestampKey.Length) != 0) return false;
        cursor += timestampKey.Length;
        while (cursor < value.Length && Char.IsWhiteSpace(value[cursor])) cursor++;
        if (cursor >= value.Length || value[cursor] != ':') return false;
        cursor++;
        while (cursor < value.Length && Char.IsWhiteSpace(value[cursor])) cursor++;
        if (cursor >= value.Length || value[cursor] != '"') return false;
        int endQuote = value.IndexOf('"', cursor + 1);
        if (endQuote <= cursor + 1) return false;

        DateTimeOffset parsed;
        if (!DateTimeOffset.TryParse(value.Substring(cursor + 1, endQuote - cursor - 1),
            CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal |
            DateTimeStyles.AdjustToUniversal, out parsed)) return false;
        eventUtc = parsed.UtcDateTime;
        return true;
    }

    private static string NormalizeJsonLine(string value)
    {
        return String.IsNullOrEmpty(value) ? String.Empty :
            value.TrimStart('\uFEFF', ' ', '\t', '\r');
    }

    private static bool LooksLikeCompleteJson(string value)
    {
        if (String.IsNullOrWhiteSpace(value)) return false;
        string trimmed = value.TrimEnd();
        return trimmed.Length > 1 && trimmed[trimmed.Length - 1] == '}';
    }

    private static IDictionary<string, object> GetObjectMap(IDictionary<string, object> map, string key)
    {
        object value;
        return map != null && map.TryGetValue(key, out value) ? value as IDictionary<string, object> : null;
    }

    private static string GetString(IDictionary<string, object> map, string key)
    {
        object value;
        return map != null && map.TryGetValue(key, out value) && value != null ? value.ToString() : null;
    }

    private static DateTime GetEventUtc(IDictionary<string, object> root)
    {
        string raw = GetString(root, "timestamp");
        DateTimeOffset parsed;
        if (!String.IsNullOrWhiteSpace(raw) && DateTimeOffset.TryParse(raw,
            CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal |
            DateTimeStyles.AdjustToUniversal, out parsed)) return parsed.UtcDateTime;
        return DateTime.UtcNow;
    }

    private void RemoveTurnsForFile(string path)
    {
        List<string> ids = activeTurns.Values
            .Where(delegate (ActiveTurn turn) { return String.Equals(turn.SourcePath, path, PathComparison); })
            .Select(delegate (ActiveTurn turn) { return turn.TurnId; }).ToList();
        if (ids.Count == 0) return;
        foreach (string id in ids) activeTurns.Remove(id);
        EventHandler changed = StateChanged;
        if (changed != null) changed(this, EventArgs.Empty);
    }

    private void SetTitleForFile(string path, string title, DateTime activityUtc)
    {
        ActiveTurn turn = activeTurns.Values
            .Where(delegate (ActiveTurn item)
            {
                return String.Equals(item.SourcePath, path, PathComparison);
            })
            .OrderByDescending(delegate (ActiveTurn item) { return item.StartSequence; })
            .FirstOrDefault();
        if (turn == null) return;
        TouchTurn(turn, activityUtc);
        if (!String.IsNullOrEmpty(turn.Title)) return;
        turn.Title = title;
        EventHandler changed = StateChanged;
        if (changed != null) changed(this, EventArgs.Empty);
    }

    private static void TouchTurn(ActiveTurn turn, DateTime activityUtc)
    {
        if (turn == null || activityUtc == DateTime.MinValue) return;
        if (turn.LastActivityUtc == DateTime.MinValue || activityUtc > turn.LastActivityUtc)
            turn.LastActivityUtc = activityUtc;
    }

    private void TouchTurnsForFile(string path, DateTime activityUtc)
    {
        foreach (ActiveTurn turn in activeTurns.Values)
            if (String.Equals(turn.SourcePath, path, PathComparison))
                TouchTurn(turn, activityUtc);
    }

    internal static bool IsTurnStale(DateTime lastActivityUtc, DateTime fileWriteUtc,
        DateTime nowUtc, int graceSeconds)
    {
        if (graceSeconds <= 0) return false;
        DateTime cutoff = nowUtc.AddSeconds(-graceSeconds);
        return lastActivityUtc < cutoff && fileWriteUtc < cutoff;
    }

    private void ClearStaleTurns(DateTime nowUtc)
    {
        if (activeTurns.Count == 0) return;
        List<string> staleIds = new List<string>();
        foreach (ActiveTurn turn in activeTurns.Values)
        {
            DateTime fileWriteUtc = SafeLastWriteTimeUtc(turn.SourcePath);
            if (IsTurnStale(turn.LastActivityUtc, fileWriteUtc, nowUtc,
                StaleTurnGraceSeconds)) staleIds.Add(turn.TurnId);
        }
        if (staleIds.Count == 0) return;
        foreach (string id in staleIds) activeTurns.Remove(id);
        staleTurnCleanupCount += staleIds.Count;
        EventHandler changed = StateChanged;
        if (changed != null) changed(this, EventArgs.Empty);
    }

    public void Dispose()
    {
        disposed = true;
        files.Clear();
        activeTurns.Clear();
        plansByFile.Clear();
    }

    private sealed class TailState
    {
        public readonly string Path;
        public long Position;
        public readonly StringBuilder LineBuffer = new StringBuilder(512);
        public readonly byte[] ByteBuffer = new byte[ReadBufferSize];
        public readonly char[] CharacterBuffer = new char[ReadBufferSize];
        public Decoder Utf8Decoder = Encoding.UTF8.GetDecoder();
        public DateTime LastWriteUtc;
        public DateTime LastActivityUtc;
        public bool SkipCurrentLine;
        public bool EventHandled;

        public TailState(string path)
        {
            Path = path;
            LastWriteUtc = SafeLastWriteTimeUtc(path);
            LastActivityUtc = LastWriteUtc == DateTime.MinValue
                ? DateTime.UtcNow : LastWriteUtc;
        }

        public void Reset()
        {
            Position = 0;
            LineBuffer.Length = 0;
            Utf8Decoder = Encoding.UTF8.GetDecoder();
            LastWriteUtc = DateTime.MinValue;
            LastActivityUtc = DateTime.UtcNow;
            SkipCurrentLine = false;
            EventHandled = false;
        }
    }

    private sealed class ActiveTurn
    {
        public readonly string TurnId;
        public readonly string SourcePath;
        public readonly long StartSequence;
        public readonly DateTime StartedUtc;
        public string Title;
        public TaskPlanProgress Plan;
        public DateTime LastActivityUtc;

        public ActiveTurn(string turnId, string sourcePath, long startSequence, DateTime startedUtc)
        {
            TurnId = turnId;
            SourcePath = sourcePath;
            StartSequence = startSequence;
            StartedUtc = startedUtc == DateTime.MinValue ? DateTime.UtcNow : startedUtc;
            LastActivityUtc = DateTime.MinValue;
        }
    }

    private sealed class TaskPlanProgress
    {
        public readonly int TotalSteps;
        public readonly int CompletedSteps;
        public readonly string CurrentStep;

        public TaskPlanProgress(int totalSteps, int completedSteps, string currentStep)
        {
            TotalSteps = totalSteps;
            CompletedSteps = completedSteps;
            CurrentStep = currentStep;
        }

        public override bool Equals(object obj)
        {
            TaskPlanProgress other = obj as TaskPlanProgress;
            return other != null && TotalSteps == other.TotalSteps && CompletedSteps == other.CompletedSteps &&
                String.Equals(CurrentStep, other.CurrentStep, StringComparison.Ordinal);
        }

        public override int GetHashCode()
        {
            return TotalSteps ^ CompletedSteps ^ (CurrentStep == null ? 0 : CurrentStep.GetHashCode());
        }
    }
}
