using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Globalization;
using System.Linq;
using System.Media;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using System.Web.Script.Serialization;
using MediaPlayer = System.Windows.Media.MediaPlayer;
using MediaExceptionEventArgs = System.Windows.Media.ExceptionEventArgs;
using Dispatcher = System.Windows.Threading.Dispatcher;
using DispatcherPriority = System.Windows.Threading.DispatcherPriority;
using Microsoft.Win32;

namespace CodeXPets
{
    internal static class Program
    {
        private const string MutexName = @"Local\CodeXPetsPortable_4B6B725D_C578_47C7_A88D_AA6E548D1ED8";

        [STAThread]
        private static void Main(string[] args)
        {
            if (args.Length >= 2 && String.Equals(args[0], "--preview", StringComparison.OrdinalIgnoreCase))
            {
                Directory.CreateDirectory(args[1]);
                SavePreview(args[1]);
                return;
            }

            bool createdNew;
            using (Mutex mutex = new Mutex(true, MutexName, out createdNew))
            {
                if (!createdNew)
                {
                    MessageBox.Show("CodeXPets 已经在任务栏里啦。", "CodeXPets",
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                try
                {
                    using (ReminderApplicationContext context = new ReminderApplicationContext())
                        Application.Run(context);
                }
                catch (Exception ex)
                {
                    try { File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "startup-error.txt"), ex.ToString(), Encoding.UTF8); }
                    catch { }
                    MessageBox.Show(ex.ToString(), "CodeXPets 启动失败", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        private static void SavePreview(string folder)
        {
            using (Bitmap idle = StatusIconFactory.CreateBitmap(256, ReminderState.Idle, 0))
                idle.Save(Path.Combine(folder, "idle.png"), ImageFormat.Png);
            for (int i = 0; i < StatusIconFactory.BusyFrameCount; i++)
                using (Bitmap busy = StatusIconFactory.CreateBitmap(256, ReminderState.Busy, i))
                    busy.Save(Path.Combine(folder, "busy-" + i + ".png"), ImageFormat.Png);
            using (Bitmap completed = StatusIconFactory.CreateBitmap(256, ReminderState.Completed, 0))
                completed.Save(Path.Combine(folder, "completed.png"), ImageFormat.Png);
            using (Bitmap error = StatusIconFactory.CreateBitmap(256, ReminderState.Error, 0))
                error.Save(Path.Combine(folder, "error.png"), ImageFormat.Png);
        }
    }

    internal sealed class PetPositionState
    {
        public readonly DockEdge DockEdge;
        public readonly string ScreenDeviceName;
        public readonly double RelativeX;
        public readonly double RelativeY;

        public PetPositionState(DockEdge dockEdge, string screenDeviceName,
            double relativeX, double relativeY)
        {
            DockEdge = dockEdge;
            ScreenDeviceName = screenDeviceName ?? String.Empty;
            RelativeX = PetPositionSettings.Clamp01(relativeX);
            RelativeY = PetPositionSettings.Clamp01(relativeY);
        }
    }

    internal static class PetPositionSettings
    {
        private const string SettingsKeyPath = @"Software\CodeXPets";
        private const string PositionValueName = "PetPositionV1";

        public static PetPositionState Load()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(SettingsKeyPath, false))
                {
                    string value = key == null ? null : key.GetValue(PositionValueName) as string;
                    return Deserialize(value);
                }
            }
            catch { return null; }
        }

        public static void Save(PetPositionState state)
        {
            if (state == null) return;
            try
            {
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(SettingsKeyPath))
                    if (key != null) key.SetValue(PositionValueName, Serialize(state),
                        RegistryValueKind.String);
            }
            catch { }
        }

        internal static string Serialize(PetPositionState state)
        {
            if (state == null) return String.Empty;
            string mode = state.DockEdge == DockEdge.Left ? "L" :
                state.DockEdge == DockEdge.Right ? "R" : "F";
            string device = Convert.ToBase64String(Encoding.UTF8.GetBytes(
                state.ScreenDeviceName ?? String.Empty));
            return String.Join(";", new[]
            {
                "1", mode, device,
                Clamp01(state.RelativeX).ToString("R", CultureInfo.InvariantCulture),
                Clamp01(state.RelativeY).ToString("R", CultureInfo.InvariantCulture)
            });
        }

        internal static PetPositionState Deserialize(string value)
        {
            if (String.IsNullOrWhiteSpace(value)) return null;
            try
            {
                string[] parts = value.Split(';');
                if (parts.Length != 5 || parts[0] != "1") return null;
                DockEdge edge = parts[1] == "L" ? DockEdge.Left :
                    parts[1] == "R" ? DockEdge.Right : DockEdge.None;
                string device = Encoding.UTF8.GetString(Convert.FromBase64String(parts[2]));
                double relativeX;
                double relativeY;
                if (!Double.TryParse(parts[3], NumberStyles.Float,
                    CultureInfo.InvariantCulture, out relativeX) ||
                    !Double.TryParse(parts[4], NumberStyles.Float,
                    CultureInfo.InvariantCulture, out relativeY)) return null;
                return new PetPositionState(edge, device, relativeX, relativeY);
            }
            catch (FormatException) { return null; }
        }

        internal static double Clamp01(double value)
        {
            if (Double.IsNaN(value) || Double.IsInfinity(value)) return 0.5D;
            return Math.Max(0D, Math.Min(1D, value));
        }
    }

    internal static class AppInfo
    {
        public const string ProductName = "CodeXPets";
        public const string Repository = "https://github.com/MrLiuGangQiang/codex-pets";

        public static string Version
        {
            get
            {
                Version version = Assembly.GetExecutingAssembly().GetName().Version;
                return version == null ? "未知版本" : version.ToString(3);
            }
        }

        public static string DisplayName
        {
            get { return ProductName + " v" + Version; }
        }
    }

    internal sealed class CodeXPetsSettings
    {
        private const string SettingsKeyPath = @"Software\CodeXPets";
        private const string HoverHeightValueName = "DockHoverHeight";
        private const string IdleHideValueName = "DockIdleHideSeconds";
        private const string RevealValueName = "DockRevealSeconds";
        private const string NotificationValueName = "DockNotificationSeconds";
        private const string SoundValueName = "SoundEnabled";
        private const string SessionsRootValueName = "SessionsRoot";

        public int DockHoverHeight { get; set; }
        public int DockIdleHideSeconds { get; set; }
        public int DockRevealSeconds { get; set; }
        public int DockNotificationSeconds { get; set; }
        public bool SoundEnabled { get; set; }
        public string SessionsRoot { get; set; }

        public static CodeXPetsSettings CreateDefault()
        {
            return new CodeXPetsSettings
            {
                DockHoverHeight = 240,
                DockIdleHideSeconds = 10,
                DockRevealSeconds = 3,
                DockNotificationSeconds = 5,
                SoundEnabled = true,
                SessionsRoot = CodexSessionMonitor.GetDefaultSessionsRoot()
            };
        }

        public static CodeXPetsSettings Load()
        {
            CodeXPetsSettings result = CreateDefault();
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(SettingsKeyPath, false))
                {
                    if (key != null)
                    {
                        result.DockHoverHeight = ReadInt(key, HoverHeightValueName,
                            result.DockHoverHeight);
                        result.DockIdleHideSeconds = ReadInt(key, IdleHideValueName,
                            result.DockIdleHideSeconds);
                        result.DockRevealSeconds = ReadInt(key, RevealValueName,
                            result.DockRevealSeconds);
                        result.DockNotificationSeconds = ReadInt(key, NotificationValueName,
                            result.DockNotificationSeconds);
                        result.SoundEnabled = ReadInt(key, SoundValueName,
                            result.SoundEnabled ? 1 : 0) != 0;
                        string root = key.GetValue(SessionsRootValueName) as string;
                        if (!String.IsNullOrWhiteSpace(root)) result.SessionsRoot = root;
                    }
                }
            }
            catch { }
            result.Normalize();
            return result;
        }

        public CodeXPetsSettings Clone()
        {
            CodeXPetsSettings copy = new CodeXPetsSettings();
            copy.CopyFrom(this);
            return copy;
        }

        public void CopyFrom(CodeXPetsSettings other)
        {
            if (other == null) return;
            DockHoverHeight = other.DockHoverHeight;
            DockIdleHideSeconds = other.DockIdleHideSeconds;
            DockRevealSeconds = other.DockRevealSeconds;
            DockNotificationSeconds = other.DockNotificationSeconds;
            SoundEnabled = other.SoundEnabled;
            SessionsRoot = other.SessionsRoot;
            Normalize();
        }

        public void Normalize()
        {
            DockHoverHeight = Math.Max(40, Math.Min(1000, DockHoverHeight));
            DockIdleHideSeconds = Math.Max(0, Math.Min(3600, DockIdleHideSeconds));
            DockRevealSeconds = Math.Max(1, Math.Min(60, DockRevealSeconds));
            DockNotificationSeconds = Math.Max(1, Math.Min(120, DockNotificationSeconds));
            SessionsRoot = NormalizePath(SessionsRoot);
        }

        public void Save()
        {
            Normalize();
            try
            {
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(SettingsKeyPath))
                {
                    if (key == null) return;
                    key.SetValue(HoverHeightValueName, DockHoverHeight, RegistryValueKind.DWord);
                    key.SetValue(IdleHideValueName, DockIdleHideSeconds, RegistryValueKind.DWord);
                    key.SetValue(RevealValueName, DockRevealSeconds, RegistryValueKind.DWord);
                    key.SetValue(NotificationValueName, DockNotificationSeconds, RegistryValueKind.DWord);
                    key.SetValue(SoundValueName, SoundEnabled ? 1 : 0, RegistryValueKind.DWord);
                    key.SetValue(SessionsRootValueName, SessionsRoot ?? String.Empty,
                        RegistryValueKind.String);
                }
            }
            catch { }
        }

        private static int ReadInt(RegistryKey key, string valueName, int fallback)
        {
            try
            {
                object value = key.GetValue(valueName);
                return value == null ? fallback : Convert.ToInt32(value, CultureInfo.InvariantCulture);
            }
            catch { return fallback; }
        }

        private static string NormalizePath(string path)
        {
            if (String.IsNullOrWhiteSpace(path)) return CodexSessionMonitor.GetDefaultSessionsRoot();
            try
            {
                string expanded = Environment.ExpandEnvironmentVariables(path.Trim());
                return Path.GetFullPath(expanded);
            }
            catch { return CodexSessionMonitor.GetDefaultSessionsRoot(); }
        }
    }

    internal sealed class ReminderApplicationContext : ApplicationContext
    {
        private readonly NotifyIcon trayIcon;
        private readonly System.Windows.Forms.Timer timer;
        private readonly System.Windows.Forms.Timer animationTimer;
        private readonly CodexSessionMonitor monitor;
        private readonly Icon idleIcon;
        private readonly Icon completedIcon;
        private readonly Icon errorIcon;
        private readonly Icon[] busyIcons;
        private readonly ToolStripMenuItem statusItem;
        private readonly ToolStripMenuItem soundItem;
        private readonly ToolStripMenuItem startupItem;
        private readonly DesktopAssistantForm assistant;
        private readonly ToolStripMenuItem assistantItem;
        private readonly CodeXPetsSettings appSettings;
        private bool soundEnabled;
        private int animationFrame;
        private int petAnimationTick;
        private ReminderState lastVisualState = (ReminderState)(-1);
        private readonly Stopwatch animationClock = Stopwatch.StartNew();
        private long lastAnimationTimestamp;
        private double spriteAnimationSeconds;
        private string lastStatusText = "";
        private DateTime completedUntilUtc = DateTime.MinValue;
        private DateTime abnormalUntilUtc = DateTime.MinValue;
        // The most recent task-related change owns the visual state until it expires.
        private ReminderState latestChangedState = ReminderState.Idle;
        private string latestChangedSourcePath;
        private DateTime lastVisualRefreshUtc = DateTime.MinValue;
        private bool showNewestTaskOnNextRefresh;
        private bool disposed;

        public ReminderApplicationContext()
        {
            appSettings = CodeXPetsSettings.Load();
            soundEnabled = appSettings.SoundEnabled;

            idleIcon = StatusIconFactory.CreateIcon(64, ReminderState.Idle, 0);
            completedIcon = StatusIconFactory.CreateIcon(64, ReminderState.Completed, 0);
            errorIcon = StatusIconFactory.CreateIcon(64, ReminderState.Error, 0);
            busyIcons = new Icon[StatusIconFactory.BusyFrameCount];
            for (int i = 0; i < busyIcons.Length; i++)
                busyIcons[i] = StatusIconFactory.CreateIcon(64, ReminderState.Busy, i);

            statusItem = new ToolStripMenuItem("状态：正在检查 Codex…");
            statusItem.Enabled = false;
            soundItem = new ToolStripMenuItem("播放语音提醒");
            soundItem.Checked = soundEnabled;
            soundItem.CheckOnClick = true;
            soundItem.Click += delegate
            {
                soundEnabled = soundItem.Checked;
                appSettings.SoundEnabled = soundEnabled;
                appSettings.Save();
            };

            startupItem = new ToolStripMenuItem("开机自动运行");
            StartupManager.MigrateLegacyEntry();
            startupItem.Checked = StartupManager.IsEnabled();
            startupItem.CheckOnClick = true;
            startupItem.Click += delegate
            {
                try { StartupManager.SetEnabled(startupItem.Checked); }
                catch (Exception ex)
                {
                    startupItem.Checked = StartupManager.IsEnabled();
                    MessageBox.Show("设置开机启动失败：\r\n" + ex.Message, AppInfo.ProductName,
                        MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            };

            ToolStripMenuItem openFolderItem = new ToolStripMenuItem("打开 Codex 会话目录");
            openFolderItem.Click += delegate { OpenSessionsFolder(); };
            ToolStripMenuItem settingsItem = new ToolStripMenuItem("设置…");
            settingsItem.Click += delegate { ShowSettings(); };
            ToolStripMenuItem diagnosticsItem = new ToolStripMenuItem("诊断信息…");
            diagnosticsItem.Click += delegate { ShowDiagnostics(); };
            ToolStripMenuItem updateItem = new ToolStripMenuItem("查看更新…");
            updateItem.Click += delegate { OpenLatestRelease(); };
            ToolStripMenuItem exitItem = new ToolStripMenuItem("退出");
            exitItem.Click += delegate { ExitThread(); };

            ContextMenuStrip menu = new ContextMenuStrip();
            menu.Items.Add(statusItem);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add(soundItem);
            menu.Items.Add(startupItem);
            menu.Items.Add(openFolderItem);
            menu.Items.Add(settingsItem);
            menu.Items.Add(diagnosticsItem);
            menu.Items.Add(updateItem);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add(exitItem);

            trayIcon = new NotifyIcon();
            trayIcon.Icon = idleIcon;
            trayIcon.Text = "正在检查";
            trayIcon.ContextMenuStrip = menu;
            trayIcon.Visible = true;

            assistant = new DesktopAssistantForm(menu, true, appSettings);
            assistantItem = new ToolStripMenuItem("显示桌面助手");
            assistantItem.Checked = true;
            assistantItem.CheckOnClick = true;
            assistantItem.Click += delegate
            {
                if (assistantItem.Checked) assistant.ShowInactive();
                else assistant.Hide();
            };
            menu.Items.Insert(2, assistantItem);

            assistant.ShowInactive();

            monitor = new CodexSessionMonitor(appSettings.SessionsRoot);
            monitor.TaskStarted += OnTaskStarted;
            monitor.TaskCompleted += OnTaskCompleted;
            monitor.TaskAborted += OnTaskAborted;
            monitor.StateChanged += OnStateChanged;
            timer = new System.Windows.Forms.Timer();
            timer.Interval = 300;
            timer.Tick += OnPollTick;
            timer.Start();

            animationTimer = new System.Windows.Forms.Timer();
            // 30 FPS is smooth enough for the small sprite and scrolling text,
            // while substantially reducing idle wake-ups compared with 60 FPS.
            animationTimer.Interval = 33;
            animationTimer.Tick += OnAnimationTick;
            animationTimer.Start();
            RefreshVisual(true);
        }

        private void OpenSessionsFolder()
        {
            string folder = appSettings.SessionsRoot;
            if (Directory.Exists(folder))
                Process.Start("explorer.exe", "\"" + folder + "\"");
            else
                MessageBox.Show("还没有找到 Codex 会话目录：\r\n" + folder,
                    AppInfo.ProductName, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void ShowSettings()
        {
            using (CodeXPetsSettingsForm dialog = new CodeXPetsSettingsForm(appSettings))
            {
                if (dialog.ShowDialog() != DialogResult.OK) return;
                string previousRoot = appSettings.SessionsRoot;
                appSettings.CopyFrom(dialog.Result);
                appSettings.Save();
                soundEnabled = appSettings.SoundEnabled;
                soundItem.Checked = soundEnabled;
                assistant.ApplySettings();
                if (!String.Equals(previousRoot, appSettings.SessionsRoot,
                    StringComparison.OrdinalIgnoreCase))
                    monitor.SetSessionsRoot(appSettings.SessionsRoot);
                RefreshVisual(true);
            }
        }

        private void ShowDiagnostics()
        {
            using (CodeXPetsDiagnosticsForm dialog =
                new CodeXPetsDiagnosticsForm(monitor, appSettings))
                dialog.ShowDialog();
        }

        private static void OpenLatestRelease()
        {
            try { Process.Start(AppInfo.Repository + "/releases/latest"); }
            catch (Exception ex)
            {
                MessageBox.Show("无法打开更新页面：\r\n" + ex.Message, AppInfo.ProductName,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void OnTaskStarted(object sender, EventArgs e)
        {
            RecordLatestTaskChange(ReminderState.Busy);
            latestChangedSourcePath = monitor.LastEventFile;
            showNewestTaskOnNextRefresh = true;
            RevealAssistant();
            RefreshVisual(true);
            if (soundEnabled) CompletionVoice.QueueStart();
        }

        private void OnTaskCompleted(object sender, EventArgs e)
        {
            RecordLatestTaskChange(ReminderState.Completed);
            RevealAssistant();
            RefreshVisual(true);
            if (soundEnabled) CompletionVoice.QueueComplete();
        }

        private void OnTaskAborted(object sender, EventArgs e)
        {
            RecordLatestTaskChange(ReminderState.Error);
            RevealAssistant();
            RefreshVisual(true);
            if (soundEnabled) CompletionVoice.QueueError();
        }

        private void RecordLatestTaskChange(ReminderState state)
        {
            latestChangedState = state;
            if (state == ReminderState.Busy)
            {
                completedUntilUtc = DateTime.MinValue;
                abnormalUntilUtc = DateTime.MinValue;
            }
            else if (state == ReminderState.Completed)
            {
                completedUntilUtc = DateTime.UtcNow.AddSeconds(5);
                abnormalUntilUtc = DateTime.MinValue;
            }
            else if (state == ReminderState.Error)
            {
                abnormalUntilUtc = DateTime.UtcNow.AddSeconds(10);
                completedUntilUtc = DateTime.MinValue;
            }
        }

        private void RevealAssistant()
        {
            if (assistantItem.Checked) assistant.ShowInactive();
        }

        private void OnStateChanged(object sender, EventArgs e)
        {
            // A plan update is also a fresh activity change, so it takes the visual
            // focus back from an older completion or error notification.
            if (String.Equals(monitor.LastEventType, "update_plan", StringComparison.Ordinal))
            {
                RecordLatestTaskChange(ReminderState.Busy);
                latestChangedSourcePath = monitor.LastEventFile;
                showNewestTaskOnNextRefresh = true;
            }
            RefreshVisual(true);
        }
        private void OnPollTick(object sender, EventArgs e)
        {
            try { monitor.Poll(); }
            catch (Exception ex) { monitor.ReportUnexpectedError("轮询会话", ex); }
        }
        private void OnAnimationTick(object sender, EventArgs e)
        {
            long timestamp = animationClock.ElapsedTicks;
            float elapsedSeconds = lastAnimationTimestamp == 0 ? 0.033F :
                (float)((timestamp - lastAnimationTimestamp) / (double)Stopwatch.Frequency);
            lastAnimationTimestamp = timestamp;
            elapsedSeconds = Math.Max(0.001F, Math.Min(0.100F, elapsedSeconds));

            spriteAnimationSeconds += elapsedSeconds;
            while (spriteAnimationSeconds >= 0.12)
            {
                spriteAnimationSeconds -= 0.12;
                animationFrame = (animationFrame + 1) % busyIcons.Length;
                petAnimationTick = (petAnimationTick + 1) % 6400;
            }
            assistant.Animate(petAnimationTick, elapsedSeconds);

            DateTime now = DateTime.UtcNow;
            double refreshMilliseconds = monitor.ActiveCount > 0 ? 120D : 250D;
            if (lastVisualRefreshUtc == DateTime.MinValue ||
                (now - lastVisualRefreshUtc).TotalMilliseconds >= refreshMilliseconds)
                RefreshVisual(false);
        }
        private void RefreshVisual(bool forceText)
        {
            int active = monitor.ActiveCount;
            DateTime now = DateTime.UtcNow;
            if (abnormalUntilUtc != DateTime.MinValue && now >= abnormalUntilUtc)
                abnormalUntilUtc = DateTime.MinValue;
            if (completedUntilUtc != DateTime.MinValue && now >= completedUntilUtc)
                completedUntilUtc = DateTime.MinValue;
            lastVisualRefreshUtc = now;
            bool abnormalRecently = now < abnormalUntilUtc;
            bool completedRecently = now < completedUntilUtc;
            ReminderState visualState = SelectVisualState(active, abnormalRecently,
                completedRecently, latestChangedState);
            if (visualState != lastVisualState)
            {
                petAnimationTick = 0;
                lastVisualState = visualState;
            }
            Icon currentIcon = visualState == ReminderState.Error ? errorIcon
                : visualState == ReminderState.Completed ? completedIcon
                : visualState == ReminderState.Busy ? busyIcons[animationFrame] : idleIcon;
            if (!Object.ReferenceEquals(trayIcon.Icon, currentIcon))
                trayIcon.Icon = currentIcon;
            string stateText;
            if (visualState == ReminderState.Busy)
            {
                int completedSteps = monitor.CompletedPlanStepCount;
                int totalSteps = monitor.TotalPlanStepCount;
                stateText = active == 1 ? "进行中" : "进行中（" + active + " 个会话）";
                if (active == 1 && totalSteps > 0)
                    stateText += " · 步骤 " + completedSteps + "/" + totalSteps;
            }
            else if (visualState == ReminderState.Error) stateText = "异常";
            else if (visualState == ReminderState.Completed) stateText = "已完成";
            else stateText = "空闲";
            string thoughtText = visualState == ReminderState.Error ? "任务异常了"
                : visualState == ReminderState.Completed ? "任务完成啦！"
                : visualState == ReminderState.Busy
                    ? (String.IsNullOrEmpty(monitor.PrimaryActiveTitle)
                        ? "正在认真处理你的任务…" : monitor.PrimaryActiveTitle)
                    : "等你交给我下一个任务";
            if (assistantItem.Checked)
            {
                if (!assistant.Visible) assistant.ShowInactive();
                IList<string> displayedTitles;
                IList<string> displayedProgress = null;
                if (visualState == ReminderState.Error)
                    displayedTitles = new[] { FormatAbnormalTaskText(monitor.LastAbortedTitle) };
                else if (visualState == ReminderState.Completed &&
                    !String.IsNullOrEmpty(monitor.LastCompletedTitle))
                    displayedTitles = new[] { monitor.LastCompletedTitle };
                else if (active > 0)
                {
                    displayedTitles = monitor.ActiveTitles;
                    displayedProgress = monitor.ActivePlanProgressLabels;
                }
                else displayedTitles = monitor.ActiveTitles;
                bool selectNewestTask = showNewestTaskOnNextRefresh && visualState == ReminderState.Busy;
                int preferredTaskIndex = SelectPreferredTaskIndex(selectNewestTask,
                    monitor.GetActiveTitleIndex(latestChangedSourcePath));
                assistant.UpdateStatus(stateText, thoughtText, visualState, displayedTitles,
                    displayedProgress, selectNewestTask, preferredTaskIndex);
                if (selectNewestTask) showNewestTaskOnNextRefresh = false;
            }
            if (forceText || stateText != lastStatusText)
            {
                statusItem.Text = "状态：" + stateText;
                SetTooltip(stateText);
                lastStatusText = stateText;
            }
        }
        internal static string FormatAbnormalTaskText(string title)
        {
            return "任务失败：" + (String.IsNullOrWhiteSpace(title) ? "未知任务" : title.Trim());
        }

        internal static int SelectPreferredTaskIndex(bool focusLatestTask, int latestTaskIndex)
        {
            // Focusing the latest task is a one-shot reaction to a new task event.
            // Periodic refreshes must not overwrite a user's manual cloud selection.
            return focusLatestTask ? latestTaskIndex : -1;
        }

        internal static ReminderState SelectVisualState(int activeCount,
            bool abnormalRecently, bool completedRecently, ReminderState latestChangedState)
        {
            // The most recently changed task owns the status. Once its notification
            // expires, fall back to any work that is still active.
            if (latestChangedState == ReminderState.Error && abnormalRecently)
                return ReminderState.Error;
            if (latestChangedState == ReminderState.Completed && completedRecently)
                return ReminderState.Completed;
            if (latestChangedState == ReminderState.Busy && activeCount > 0)
                return ReminderState.Busy;
            return activeCount > 0 ? ReminderState.Busy : ReminderState.Idle;
        }

        private void SetTooltip(string text)
        {
            if (text.Length > 63) text = text.Substring(0, 63);
            trayIcon.Text = text;
        }
        protected override void ExitThreadCore()
        {
            if (timer != null) timer.Stop();
            if (animationTimer != null) animationTimer.Stop();
            trayIcon.Visible = false;
            base.ExitThreadCore();
        }
        protected override void Dispose(bool disposing)
        {
            if (disposing && !disposed)
            {
                disposed = true;
                if (timer != null) timer.Dispose();
                if (animationTimer != null) animationTimer.Dispose();
                if (monitor != null) monitor.Dispose();
                if (assistant != null) assistant.Dispose();
                if (trayIcon != null)
                {
                    trayIcon.Visible = false;
                    if (trayIcon.ContextMenuStrip != null) trayIcon.ContextMenuStrip.Dispose();
                    trayIcon.Dispose();
                }
                if (idleIcon != null) idleIcon.Dispose();
                if (completedIcon != null) completedIcon.Dispose();
                if (errorIcon != null) errorIcon.Dispose();
                if (busyIcons != null)
                    foreach (Icon icon in busyIcons)
                        if (icon != null) icon.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    internal sealed class CodeXPetsSettingsForm : Form
    {
        private readonly NumericUpDown hoverHeightBox;
        private readonly NumericUpDown idleHideBox;
        private readonly NumericUpDown revealBox;
        private readonly NumericUpDown notificationBox;
        private readonly CheckBox soundBox;
        private readonly TextBox sessionsRootBox;

        public CodeXPetsSettings Result { get; private set; }

        public CodeXPetsSettingsForm(CodeXPetsSettings current)
        {
            Result = (current ?? CodeXPetsSettings.CreateDefault()).Clone();
            Text = AppInfo.DisplayName + " 设置";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(560, 355);
            Font = SystemFonts.MessageBoxFont;

            hoverHeightBox = CreateNumberBox(40, 1000, 10);
            idleHideBox = CreateNumberBox(0, 3600, 1);
            revealBox = CreateNumberBox(1, 60, 1);
            notificationBox = CreateNumberBox(1, 120, 1);
            soundBox = new CheckBox { AutoSize = true, Text = "播放开始、完成和异常语音提醒" };
            sessionsRootBox = new TextBox { Dock = DockStyle.Fill };

            TableLayoutPanel table = new TableLayoutPanel();
            table.Dock = DockStyle.Fill;
            table.Padding = new Padding(14);
            table.ColumnCount = 2;
            table.RowCount = 8;
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 160F));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            for (int row = 0; row < 6; row++)
                table.RowStyles.Add(new RowStyle(SizeType.Absolute, 38F));
            table.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            table.RowStyles.Add(new RowStyle(SizeType.Absolute, 42F));
            Controls.Add(table);

            AddSettingRow(table, 0, "边缘触发区高度：", hoverHeightBox);
            AddSettingRow(table, 1, "吸附自动隐藏（秒）：", idleHideBox);
            AddSettingRow(table, 2, "鼠标唤出保持（秒）：", revealBox);
            AddSettingRow(table, 3, "任务云朵保持（秒）：", notificationBox);
            AddSettingRow(table, 4, "声音：", soundBox);

            FlowLayoutPanel folderPanel = new FlowLayoutPanel();
            folderPanel.Dock = DockStyle.Fill;
            folderPanel.FlowDirection = FlowDirection.LeftToRight;
            folderPanel.WrapContents = false;
            folderPanel.Margin = new Padding(0);
            sessionsRootBox.Width = 300;
            Button browseButton = new Button { Text = "浏览…", AutoSize = true };
            browseButton.Click += BrowseSessionsRoot;
            folderPanel.Controls.Add(sessionsRootBox);
            folderPanel.Controls.Add(browseButton);
            AddSettingRow(table, 5, "Codex 会话目录：", folderPanel);

            Label note = new Label();
            note.AutoSize = true;
            note.MaximumSize = new Size(500, 0);
            note.Text = @"自动隐藏设为 0 表示吸附后始终显示。会话目录修改后会立即重新扫描；默认目录为 %USERPROFILE%\.codex\sessions。";
            note.ForeColor = SystemColors.GrayText;
            note.Margin = new Padding(3, 8, 3, 3);
            table.Controls.Add(note, 0, 6);
            table.SetColumnSpan(note, 2);

            FlowLayoutPanel buttons = new FlowLayoutPanel();
            buttons.Dock = DockStyle.Fill;
            buttons.FlowDirection = FlowDirection.RightToLeft;
            buttons.WrapContents = false;
            Button okButton = new Button { Text = "确定", AutoSize = true };
            Button cancelButton = new Button { Text = "取消", AutoSize = true, DialogResult = DialogResult.Cancel };
            Button defaultsButton = new Button { Text = "恢复默认", AutoSize = true };
            okButton.Click += SaveAndClose;
            defaultsButton.Click += delegate { SetValues(CodeXPetsSettings.CreateDefault()); };
            buttons.Controls.Add(okButton);
            buttons.Controls.Add(cancelButton);
            buttons.Controls.Add(defaultsButton);
            table.Controls.Add(buttons, 0, 7);
            table.SetColumnSpan(buttons, 2);
            AcceptButton = okButton;
            CancelButton = cancelButton;

            SetValues(Result);
        }

        private static NumericUpDown CreateNumberBox(int minimum, int maximum, int increment)
        {
            return new NumericUpDown
            {
                Minimum = minimum,
                Maximum = maximum,
                Increment = increment,
                Width = 120,
                ThousandsSeparator = true
            };
        }

        private static void AddSettingRow(TableLayoutPanel table, int row, string labelText,
            Control control)
        {
            Label label = new Label();
            label.Text = labelText;
            label.TextAlign = ContentAlignment.MiddleRight;
            label.Dock = DockStyle.Fill;
            control.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            table.Controls.Add(label, 0, row);
            table.Controls.Add(control, 1, row);
        }

        private void SetValues(CodeXPetsSettings values)
        {
            CodeXPetsSettings normalized = values.Clone();
            normalized.Normalize();
            hoverHeightBox.Value = normalized.DockHoverHeight;
            idleHideBox.Value = normalized.DockIdleHideSeconds;
            revealBox.Value = normalized.DockRevealSeconds;
            notificationBox.Value = normalized.DockNotificationSeconds;
            soundBox.Checked = normalized.SoundEnabled;
            sessionsRootBox.Text = normalized.SessionsRoot;
        }

        private void BrowseSessionsRoot(object sender, EventArgs e)
        {
            using (FolderBrowserDialog dialog = new FolderBrowserDialog())
            {
                dialog.Description = "选择 Codex sessions 目录";
                if (Directory.Exists(sessionsRootBox.Text))
                    dialog.SelectedPath = sessionsRootBox.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK)
                    sessionsRootBox.Text = dialog.SelectedPath;
            }
        }

        private void SaveAndClose(object sender, EventArgs e)
        {
            Result.DockHoverHeight = Decimal.ToInt32(hoverHeightBox.Value);
            Result.DockIdleHideSeconds = Decimal.ToInt32(idleHideBox.Value);
            Result.DockRevealSeconds = Decimal.ToInt32(revealBox.Value);
            Result.DockNotificationSeconds = Decimal.ToInt32(notificationBox.Value);
            Result.SoundEnabled = soundBox.Checked;
            Result.SessionsRoot = sessionsRootBox.Text;
            Result.Normalize();
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    internal sealed class CodeXPetsDiagnosticsForm : Form
    {
        private readonly CodexSessionMonitor monitor;
        private readonly CodeXPetsSettings settings;
        private readonly TextBox diagnosticsBox;
        private readonly System.Windows.Forms.Timer refreshTimer;

        public CodeXPetsDiagnosticsForm(CodexSessionMonitor sessionMonitor,
            CodeXPetsSettings currentSettings)
        {
            monitor = sessionMonitor;
            settings = currentSettings;
            Text = AppInfo.DisplayName + " 诊断信息";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.Sizable;
            ShowInTaskbar = false;
            MinimizeBox = false;
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(700, 470);
            Font = SystemFonts.MessageBoxFont;

            diagnosticsBox = new TextBox();
            diagnosticsBox.Dock = DockStyle.Fill;
            diagnosticsBox.Multiline = true;
            diagnosticsBox.ReadOnly = true;
            diagnosticsBox.ScrollBars = ScrollBars.Both;
            diagnosticsBox.WordWrap = false;
            diagnosticsBox.Font = new Font("Consolas", 9F);
            Controls.Add(diagnosticsBox);

            FlowLayoutPanel buttons = new FlowLayoutPanel();
            buttons.Dock = DockStyle.Bottom;
            buttons.Height = 44;
            buttons.Padding = new Padding(6);
            buttons.FlowDirection = FlowDirection.RightToLeft;
            Button closeButton = new Button { Text = "关闭", AutoSize = true, DialogResult = DialogResult.OK };
            Button copyButton = new Button { Text = "复制", AutoSize = true };
            Button refreshButton = new Button { Text = "刷新", AutoSize = true };
            Button openButton = new Button { Text = "打开会话目录", AutoSize = true };
            copyButton.Click += delegate
            {
                try { if (!String.IsNullOrEmpty(diagnosticsBox.Text)) Clipboard.SetText(diagnosticsBox.Text); }
                catch { }
            };
            refreshButton.Click += delegate { RefreshDiagnostics(); };
            openButton.Click += delegate
            {
                if (Directory.Exists(settings.SessionsRoot))
                    Process.Start("explorer.exe", "\"" + settings.SessionsRoot + "\"");
            };
            buttons.Controls.Add(closeButton);
            buttons.Controls.Add(copyButton);
            buttons.Controls.Add(refreshButton);
            buttons.Controls.Add(openButton);
            Controls.Add(buttons);
            AcceptButton = closeButton;

            refreshTimer = new System.Windows.Forms.Timer();
            refreshTimer.Interval = 1000;
            refreshTimer.Tick += delegate { RefreshDiagnostics(); };
            refreshTimer.Start();
            RefreshDiagnostics();
        }

        private void RefreshDiagnostics()
        {
            StringBuilder builder = new StringBuilder();
            builder.AppendLine(AppInfo.DisplayName);
            builder.AppendLine("运行目录：" + AppDomain.CurrentDomain.BaseDirectory);
            builder.AppendLine("操作系统：" + Environment.OSVersion);
            builder.AppendLine("CLR：" + Environment.Version);
            builder.AppendLine("声音提醒：" + (settings.SoundEnabled ? "开启" : "关闭"));
            builder.AppendLine("触发区高度：" + settings.DockHoverHeight + " px");
            builder.AppendLine("自动隐藏：" + (settings.DockIdleHideSeconds <= 0
                ? "关闭" : settings.DockIdleHideSeconds + " 秒"));
            builder.AppendLine();
            builder.Append(monitor.GetDiagnosticsText());
            diagnosticsBox.Text = builder.ToString();
            diagnosticsBox.SelectionStart = 0;
            diagnosticsBox.SelectionLength = 0;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (refreshTimer != null) refreshTimer.Dispose();
                if (diagnosticsBox != null && diagnosticsBox.Font != null)
                    diagnosticsBox.Font.Dispose();
            }
            base.Dispose(disposing);
        }
    }

    internal sealed class DesktopAssistantForm : Form
    {
        private const string SpriteResource = "white-cat-spritesheet.png";
        private const string DockSpriteResource = "cat-dock-spritesheet.png";
        private const string CloudResource = "cloud-bubble.png";
        private const int DockSpriteCellSize = 256;
        private const int DockExpressionCount = 4;
        private const int SpriteColumns = 8;
        private const int SpriteRows = 4;
        private const int SpriteCellWidth = 192;
        private const int SpriteCellHeight = 208;
        private const int IdleSpriteRow = 0;
        private const int CompletedSpriteRow = 1;
        private const int BusySpriteRow = 2;
        private const int ErrorSpriteRow = 3;
        private const float ScrollStartHoldSeconds = 1.9F;
        private const float ScrollEndHoldSeconds = 1.7F;
        private const float ShortTaskDisplaySeconds = 6F;
        private const float ScrollSpeedPixelsPerSecond = 15F;
        private const int TextSupersample = 3;
        private const int WmMouseActivate = 0x0021;
        private const int WmNcHitTest = 0x0084;
        private const int HtClient = 1;
        private const int HtTransparent = -1;
        private const float SpriteScale = 1.35F;
        private const int DefaultDockHoverHeight = 240;
        private const int DefaultDockIdleHideSeconds = 10;
        private const float DockSlideInDurationSeconds = 0.30F;
        private const float DockSlideOutDurationSeconds = 0.55F;
        private static readonly Color HeaderTextColor = Color.FromArgb(34, 49, 67);
        private static readonly Color ContentTextColor = Color.FromArgb(45, 60, 78);
        private static readonly Color DotOutlineColor = Color.FromArgb(42, 50, 60);
        private static readonly Color DotFillColor = Color.FromArgb(241, 248, 255);
        private static readonly Color BulbGlowColor = Color.FromArgb(83, 169, 236);
        private static readonly Color BulbHighlightColor = Color.FromArgb(202, 232, 255);
        private readonly Bitmap spriteSheet;
        private readonly Bitmap dockSpriteSheet;
        private readonly Bitmap cloudBubble;
        private readonly SpriteFrameMetrics[,] spriteMetrics;
        private readonly Rectangle[] dockSpriteOpaqueBounds;
        private readonly bool positionPersistenceEnabled;
        private readonly CodeXPetsSettings appSettings;
        private float uiScale = 1F;
        private int bubbleWidth = 320;
        private int bubbleHeight = 112;
        private int petWidth = 96;
        private int petHeight = 104;
        private int contentViewportWidth = 190;
        private int contentViewportHeight = 44;
        private string statusText = "空闲";
        private string thoughtText = "等你交给我下一个任务";
        private ReminderState currentState = ReminderState.Idle;
        private readonly List<string> taskTitles = new List<string>();
        private readonly List<string> taskProgressLabels = new List<string>();
        private int taskIndex;
        private float scrollOffset;
        private float scrollHoldSeconds;
        private float scrollCycleSeconds;
        private bool scrollAtEnd;
        private string lastDisplayedText = String.Empty;
        private string wrappedSource = null;
        private string wrappedText = String.Empty;
        private int wrappedWidth = -1;
        private float wrappedScale;
        private string measuredSource = String.Empty;
        private int measuredWidth = -1;
        private int measuredHeight;
        private float measuredScale;
        private Bitmap smoothTextBitmap;
        private string smoothTextSource = String.Empty;
        private int smoothTextWidth = -1;
        private float smoothTextScale;
        private float smoothTextLineHeight;
        private int smoothTextSourceLineHeight;
        private string[] smoothTextLines = new string[0];
        private Point dragStartCursor;
        private Point dragStartLocation;
        private Point preferredAnchor;
        private bool preferredAnchorInitialized;
        private bool dragPending;
        private bool dragging;
        private bool dragStartedDocked;
        private int animationFrame;
        private DockEdge dockEdge;
        private int dockCoordinate;
        private string dockScreenDeviceName;
        private bool dockBubbleBelow;
        private float dockVisibility = 1F;
        private DateTime dockLastContentChangeUtc = DateTime.UtcNow;
        private DateTime dockThoughtUntilUtc = DateTime.MinValue;
        private DateTime dockHoverRevealUntilUtc = DateTime.MinValue;

        private struct SpriteFrameMetrics
        {
            public readonly float AnchorX;
            public readonly float Bottom;
            public readonly Rectangle OpaqueBounds;
            public readonly bool HasPixels;

            public SpriteFrameMetrics(float anchorX, float bottom, Rectangle opaqueBounds, bool hasPixels)
            {
                AnchorX = anchorX;
                Bottom = bottom;
                OpaqueBounds = opaqueBounds;
                HasPixels = hasPixels;
            }

            public static SpriteFrameMetrics CreateDefault()
            {
                return new SpriteFrameMetrics(SpriteCellWidth / 2F, SpriteCellHeight,
                    new Rectangle(0, 0, SpriteCellWidth, SpriteCellHeight), false);
            }
        }

        public DesktopAssistantForm(ContextMenuStrip menu, bool persistPosition = true,
            CodeXPetsSettings settings = null)
        {
            positionPersistenceEnabled = persistPosition;
            appSettings = settings ?? CodeXPetsSettings.CreateDefault();
            appSettings.Normalize();
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            ShowInTaskbar = false;
            TopMost = true;
            AutoScaleMode = AutoScaleMode.None;
            Width = 122;
            Height = 116;
            MinimumSize = Size.Empty;
            MaximumSize = Size.Empty;
            BackColor = Color.Black;
            ContextMenuStrip = menu;
            DoubleBuffered = true;
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
            spriteSheet = LoadBitmapResource(SpriteResource);
            dockSpriteSheet = LoadBitmapResource(DockSpriteResource);
            cloudBubble = LoadBitmapResource(CloudResource);
            spriteMetrics = AnalyzeSpriteFrames(spriteSheet);
            dockSpriteOpaqueBounds = AnalyzeDockSpriteBounds(dockSpriteSheet);

            Rectangle workArea = Screen.PrimaryScreen.WorkingArea;
            Location = new Point(workArea.Right - Width - 24, workArea.Bottom - Height - 24);
            RememberCurrentAnchor();
            RecalculateAdaptiveLayout();
            if (!positionPersistenceEnabled || !RestoreSavedPosition()) RememberCurrentAnchor();

            MouseDown += StartDrag;
            MouseMove += Drag;
            MouseUp += EndDrag;
            MouseCaptureChanged += AssistantMouseCaptureChanged;
            Shown += delegate { RecalculateAdaptiveLayout(); RenderLayered(); };
        }

        public void ShowInactive()
        {
            if (IsDisposed) return;
            if (!Visible) Show();
            RecalculateAdaptiveLayout();
            if (IsHandleCreated)
                SetWindowPos(Handle, new IntPtr(-1), 0, 0, 0, 0,
                    0x0001 | 0x0002 | 0x0010 | 0x0040);
        }

        public void ApplySettings()
        {
            appSettings.Normalize();
            RecalculateAdaptiveLayout();
            RenderLayered();
        }

        public void UpdateStatus(string status, string thought, ReminderState state, IList<string> titles,
            IList<string> progressLabels, bool selectNewestTask, int preferredTaskIndex)
        {
            if (IsDisposed) return;
            string nextStatus = status ?? "空闲";
            string nextThought = thought ?? String.Empty;
            List<string> nextTitles = titles == null ? new List<string>() : new List<string>(titles);
            List<string> nextProgress = progressLabels == null ? new List<string>() : new List<string>(progressLabels);
            while (nextProgress.Count < nextTitles.Count) nextProgress.Add(null);
            if (nextProgress.Count > nextTitles.Count)
                nextProgress.RemoveRange(nextTitles.Count, nextProgress.Count - nextTitles.Count);
            int previousTaskIndex = taskIndex;
            string selectedTitle = GetSelectedTaskTitle();
            bool changed = !String.Equals(statusText, nextStatus, StringComparison.Ordinal) ||
                !String.Equals(thoughtText, nextThought, StringComparison.Ordinal) ||
                currentState != state || taskTitles.Count != nextTitles.Count ||
                taskProgressLabels.Count != nextProgress.Count;
            if (!changed)
            {
                for (int i = 0; i < taskTitles.Count; i++)
                    if (!String.Equals(taskTitles[i], nextTitles[i], StringComparison.Ordinal) ||
                        !String.Equals(taskProgressLabels[i], nextProgress[i], StringComparison.Ordinal)) { changed = true; break; }
            }
            if (changed && IsDocked && state != ReminderState.Idle)
            {
                DateTime changedAt = DateTime.UtcNow;
                dockLastContentChangeUtc = changedAt;
                dockThoughtUntilUtc = changedAt.AddSeconds(GetCloudNotificationSeconds(
                    state, appSettings.DockNotificationSeconds));
            }
            statusText = nextStatus;
            thoughtText = nextThought;
            currentState = state;
            taskTitles.Clear();
            taskTitles.AddRange(nextTitles);
            taskProgressLabels.Clear();
            taskProgressLabels.AddRange(nextProgress);
            int nextTaskIndex;
            if (currentState != ReminderState.Busy)
                nextTaskIndex = 0;
            else if (preferredTaskIndex >= 0 && preferredTaskIndex < taskTitles.Count)
                nextTaskIndex = preferredTaskIndex;
            else if (selectNewestTask && taskTitles.Count > 0)
                nextTaskIndex = taskTitles.Count - 1;
            else
            {
                int matchingIndex = FindTaskIndex(selectedTitle);
                nextTaskIndex = matchingIndex >= 0 ? matchingIndex :
                    Math.Max(0, Math.Min(previousTaskIndex, taskTitles.Count - 1));
            }
            bool taskSelectionChanged = taskIndex != nextTaskIndex;
            taskIndex = nextTaskIndex;
            if (changed || taskSelectionChanged) ResetScroll();
            bool layoutChanged = RecalculateAdaptiveLayout();
            if (changed || taskSelectionChanged || layoutChanged) RenderLayered();
        }

        internal static bool ShouldRenderAnimation(bool isBusy, bool frameChanged,
            bool scrollChanged, bool dockAnimationChanged)
        {
            return frameChanged || scrollChanged || dockAnimationChanged;
        }

        public void Animate(int frame, float elapsedSeconds)
        {
            bool frameChanged = animationFrame != frame;
            animationFrame = frame;
            bool scrollChanged = currentState == ReminderState.Busy &&
                AdvanceScroll(elapsedSeconds);
            bool dockAnimationChanged = AdvanceDockAnimation(elapsedSeconds);
            if (!ShouldRenderAnimation(currentState == ReminderState.Busy, frameChanged,
                scrollChanged, dockAnimationChanged)) return;
            RenderLayered();
        }

        private bool AdvanceDockAnimation(float elapsedSeconds)
        {
            if (!IsDocked) return false;
            DateTime now = DateTime.UtcNow;
            bool hovering = IsDockHovering();
            if (hovering) dockHoverRevealUntilUtc = now.AddSeconds(appSettings.DockRevealSeconds);
            bool shouldBeVisible = ShouldShowDock(dockLastContentChangeUtc, now,
                IsDragActive, hovering, dockHoverRevealUntilUtc,
                appSettings.DockIdleHideSeconds);
            float target = shouldBeVisible ? 1F : 0F;
            if (Math.Abs(dockVisibility - target) < 0.001F)
            {
                dockVisibility = target;
                return false;
            }
            float duration = target > dockVisibility
                ? DockSlideInDurationSeconds : DockSlideOutDurationSeconds;
            float step = elapsedSeconds / duration;
            dockVisibility = target > dockVisibility
                ? Math.Min(target, dockVisibility + step)
                : Math.Max(target, dockVisibility - step);
            return true;
        }

        private bool RecalculateAdaptiveLayout()
        {
            float nextScale = GetUiScale();
            Point anchor = GetPreferredAnchor();
            Screen screen = Screen.FromPoint(anchor);
            Rectangle workArea = screen.WorkingArea;

            // Keep one stable window size so status and title updates never move the pet.
            int nextPetWidth = Math.Max(72, (int)Math.Round(96F * nextScale));
            int nextPetHeight = Math.Max(78, (int)Math.Round(104F * nextScale));
            int edgeMargin = Math.Max(12, (int)Math.Round(16F * nextScale));
            int availableWidth = Math.Max(1, workArea.Width - edgeMargin * 2);
            int desiredBubbleWidth = Math.Max(260, (int)Math.Round(300F * nextScale));
            int maxBubbleWidth = Math.Max(1, Math.Min(availableWidth,
                (int)Math.Round(340F * nextScale)));
            int nextBubbleWidth = Math.Min(desiredBubbleWidth, maxBubbleWidth);
            if (nextBubbleWidth < 1) nextBubbleWidth = availableWidth;

            float sourceCloudRatio = cloudBubble == null ? 0.35F :
                (float)cloudBubble.Height / cloudBubble.Width;
            // Stretch the source cloud vertically so the bubble looks rounder
            // instead of like a flat banner.
            float roundedCloudRatio = Math.Max(sourceCloudRatio, 0.50F);
            int nextBubbleHeight = Math.Max((int)Math.Round(132F * nextScale),
                (int)Math.Round(nextBubbleWidth * roundedCloudRatio));
            // Keep roughly one fewer Chinese character on each line.  Use a
            // DPI-scaled character allowance instead of fixed pixels so the
            // wrapping remains consistent across displays and resolutions.
            int baseViewportWidth = (int)Math.Round(nextBubbleWidth * 0.56F);
            int oneCharacterWidth = Math.Max(8, (int)Math.Round(12F * nextScale));
            int minimumViewportWidth = Math.Max(108,
                (int)Math.Round(nextBubbleWidth * 0.42F));
            int nextViewportWidth = Math.Max(minimumViewportWidth,
                baseViewportWidth - oneCharacterWidth);
            int nextViewportHeight = Math.Max((int)Math.Round(58F * nextScale),
                (int)Math.Round(nextBubbleHeight * 0.44F));
            int nextWidth = Math.Max(nextBubbleWidth,
                nextPetWidth + (int)Math.Round(24F * nextScale));
            int nextHeight = nextBubbleHeight + (int)Math.Round(8F * nextScale) +
                nextPetHeight;

            bool changed = Width != nextWidth || Height != nextHeight ||
                Math.Abs(uiScale - nextScale) > 0.01F ||
                petWidth != nextPetWidth || petHeight != nextPetHeight ||
                bubbleWidth != nextBubbleWidth || bubbleHeight != nextBubbleHeight ||
                contentViewportWidth != nextViewportWidth ||
                contentViewportHeight != nextViewportHeight;
            if (!changed) return false;

            // Defer DPI/layout changes until the current pointer interaction ends.
            if (IsDragActive) return false;

            uiScale = nextScale;
            petWidth = nextPetWidth;
            petHeight = nextPetHeight;
            bubbleWidth = nextBubbleWidth;
            bubbleHeight = nextBubbleHeight;
            contentViewportWidth = nextViewportWidth;
            contentViewportHeight = nextViewportHeight;
            InvalidateTextMeasurement();

            int nextLeft = anchor.X - nextWidth / 2;
            int nextTop = anchor.Y - nextHeight;
            nextLeft = Math.Max(workArea.Left,
                Math.Min(nextLeft, workArea.Right - nextWidth));
            nextTop = Math.Max(workArea.Top,
                Math.Min(nextTop, workArea.Bottom - nextHeight));
            SetBounds(nextLeft, nextTop, nextWidth, nextHeight, BoundsSpecified.All);
            if (IsDocked) PositionDockedWindow();
            // Store the actual on-screen pet anchor after clamping.  Subsequent
            // polling ticks therefore have no reason to move the form.
            if (!IsDocked) RememberCurrentAnchor();
            ResetScroll();
            return true;
        }

        private Point GetPreferredAnchor()
        {
            if (!preferredAnchorInitialized) RememberCurrentAnchor();
            return preferredAnchor;
        }

        private void RememberCurrentAnchor()
        {
            if (IsDocked) return;
            preferredAnchor = new Point(Left + Width / 2, Top + Height);
            preferredAnchorInitialized = true;
        }

        private static Screen FindScreenByDeviceName(string deviceName)
        {
            if (String.IsNullOrEmpty(deviceName)) return null;
            foreach (Screen screen in Screen.AllScreens)
                if (String.Equals(screen.DeviceName, deviceName,
                    StringComparison.OrdinalIgnoreCase)) return screen;
            return null;
        }

        private bool RestoreSavedPosition()
        {
            PetPositionState state = PetPositionSettings.Load();
            if (state == null) return false;
            Screen screen = FindScreenByDeviceName(state.ScreenDeviceName) ?? Screen.PrimaryScreen;
            if (screen == null) return false;
            Rectangle workArea = screen.WorkingArea;
            if (state.DockEdge == DockEdge.Left || state.DockEdge == DockEdge.Right)
            {
                dockEdge = state.DockEdge;
                dockScreenDeviceName = screen.DeviceName;
                dockCoordinate = workArea.Top + (int)Math.Round(state.RelativeY * workArea.Height);
                dockVisibility = 1F;
                dockLastContentChangeUtc = DateTime.UtcNow;
                dockThoughtUntilUtc = DateTime.MinValue;
                dockHoverRevealUntilUtc = DateTime.MinValue;
                PositionDockedWindow();
                return true;
            }

            int anchorX = workArea.Left + (int)Math.Round(state.RelativeX * workArea.Width);
            int anchorY = workArea.Top + (int)Math.Round(state.RelativeY * workArea.Height);
            int x = Math.Max(workArea.Left, Math.Min(anchorX - Width / 2,
                workArea.Right - Width));
            int y = Math.Max(workArea.Top, Math.Min(anchorY - Height,
                workArea.Bottom - Height));
            dockEdge = DockEdge.None;
            dockScreenDeviceName = null;
            SetBounds(x, y, Width, Height, BoundsSpecified.Location);
            RememberCurrentAnchor();
            return true;
        }

        private void SaveCurrentPosition()
        {
            if (!positionPersistenceEnabled) return;
            try
            {
                Screen screen = IsDocked
                    ? (FindScreenByDeviceName(dockScreenDeviceName) ?? Screen.FromRectangle(Bounds))
                    : Screen.FromRectangle(Bounds);
                if (screen == null) return;
                Rectangle workArea = screen.WorkingArea;
                if (workArea.Width <= 0 || workArea.Height <= 0) return;
                if (IsDocked)
                {
                    double relativeY = (dockCoordinate - workArea.Top) /
                        (double)workArea.Height;
                    PetPositionSettings.Save(new PetPositionState(dockEdge,
                        screen.DeviceName, 0.5D, relativeY));
                }
                else
                {
                    Point anchor = GetPreferredAnchor();
                    double relativeX = (anchor.X - workArea.Left) /
                        (double)workArea.Width;
                    double relativeY = (anchor.Y - workArea.Top) /
                        (double)workArea.Height;
                    PetPositionSettings.Save(new PetPositionState(DockEdge.None,
                        screen.DeviceName, relativeX, relativeY));
                }
            }
            catch { }
        }

        private float GetUiScale()
        {
            try
            {
                if (IsHandleCreated)
                {
                    uint dpi = GetDpiForWindow(Handle);
                    if (dpi >= 96) return Math.Max(1F, Math.Min(2F, dpi / 96F));
                }
            }
            catch { }
            try
            {
                using (Graphics graphics = Graphics.FromHwnd(IntPtr.Zero))
                    return Math.Max(1F, Math.Min(2F, graphics.DpiX / 96F));
            }
            catch { return 1F; }
        }

        private void ClampToWorkingArea()
        {
            if (IsDocked)
            {
                PositionDockedWindow();
                return;
            }
            Rectangle workArea = Screen.FromRectangle(Bounds).WorkingArea;
            int x = Math.Max(workArea.Left, Math.Min(Left, workArea.Right - Width));
            int y = Math.Max(workArea.Top, Math.Min(Top, workArea.Bottom - Height));
            if (x != Left || y != Top) Location = new Point(x, y);
        }

        private void PositionDockedWindow()
        {
            if (!IsDocked) return;
            Screen screen = FindScreenByDeviceName(dockScreenDeviceName) ??
                Screen.FromRectangle(Bounds);
            if (screen == null) return;
            dockScreenDeviceName = screen.DeviceName;
            Rectangle workArea = screen.WorkingArea;
            dockBubbleBelow = false;
            Rectangle visible = GetDockedPoseVisibleBounds();
            int candidateTop = dockCoordinate - (visible.Top + visible.Height / 2);
            if (candidateTop < workArea.Top)
            {
                dockBubbleBelow = true;
                visible = GetDockedPoseVisibleBounds();
            }
            int x = dockEdge == DockEdge.Left ? workArea.Left : workArea.Right - Width;
            int y = dockCoordinate - (visible.Top + visible.Height / 2);
            x = Math.Max(workArea.Left, Math.Min(x, workArea.Right - Width));
            y = Math.Max(workArea.Top, Math.Min(y, workArea.Bottom - Height));
            if (x != Left || y != Top) Location = new Point(x, y);
        }

        private bool TrySnapToEdge(Point cursor)
        {
            Rectangle workArea = Screen.FromPoint(cursor).WorkingArea;
            int snapDistance = Math.Max(24, (int)Math.Round(36F * uiScale));
            DockEdge edge = SelectSnapEdge(cursor, workArea, snapDistance);
            if (edge == DockEdge.None) return false;
            dockEdge = edge;
            dockScreenDeviceName = Screen.FromPoint(cursor).DeviceName;
            dockCoordinate = cursor.Y;
            dockVisibility = 1F;
            dockLastContentChangeUtc = DateTime.UtcNow;
            dockThoughtUntilUtc = DateTime.MinValue;
            dockHoverRevealUntilUtc = DateTime.MinValue;
            PositionDockedWindow();
            ResetScroll();
            RenderLayered();
            return true;
        }

        private void UndockForDrag(Point cursor)
        {
            if (!IsDocked) return;
            dockEdge = DockEdge.None;
            dockScreenDeviceName = null;
            dockBubbleBelow = false;
            dockVisibility = 1F;
            dockThoughtUntilUtc = DateTime.MinValue;
            dockHoverRevealUntilUtc = DateTime.MinValue;
            int x = cursor.X - Width / 2;
            int y = cursor.Y - Height + Math.Max(24, petHeight / 2);
            Location = new Point(x, y);
            dragStartCursor = cursor;
            dragStartLocation = Location;
            preferredAnchor = new Point(Left + Width / 2, Top + Height);
            preferredAnchorInitialized = true;
            RenderLayered();
        }

        internal bool IsDocked
        {
            get { return dockEdge != DockEdge.None; }
        }

        internal static DockEdge SelectSnapEdge(Point cursor, Rectangle workArea, int snapDistance)
        {
            if (Math.Abs(cursor.X - workArea.Left) <= snapDistance) return DockEdge.Left;
            if (Math.Abs(cursor.X - workArea.Right) <= snapDistance) return DockEdge.Right;
            return DockEdge.None;
        }

        internal static bool ShouldKeepDockVisible(DateTime lastContentChangeUtc,
            DateTime nowUtc)
        {
            return ShouldKeepDockVisible(lastContentChangeUtc, nowUtc,
                DefaultDockIdleHideSeconds);
        }

        internal static bool ShouldKeepDockVisible(DateTime lastContentChangeUtc,
            DateTime nowUtc, int idleHideSeconds)
        {
            return idleHideSeconds <= 0 ||
                (nowUtc - lastContentChangeUtc).TotalSeconds < idleHideSeconds;
        }

        internal static bool ShouldShowDock(DateTime lastContentChangeUtc,
            DateTime nowUtc, bool isDragging, bool isHovering, DateTime hoverRevealUntilUtc)
        {
            return ShouldShowDock(lastContentChangeUtc, nowUtc, isDragging, isHovering,
                hoverRevealUntilUtc, DefaultDockIdleHideSeconds);
        }

        internal static bool ShouldShowDock(DateTime lastContentChangeUtc,
            DateTime nowUtc, bool isDragging, bool isHovering, DateTime hoverRevealUntilUtc,
            int idleHideSeconds)
        {
            return isDragging || isHovering || nowUtc < hoverRevealUntilUtc ||
                ShouldKeepDockVisible(lastContentChangeUtc, nowUtc, idleHideSeconds);
        }

        internal static Rectangle GetDockHoverBounds(DockEdge edge, Rectangle workArea,
            int dockY, float scale, bool fullyHidden)
        {
            return GetDockHoverBounds(edge, workArea, dockY, scale, fullyHidden,
                DefaultDockHoverHeight);
        }

        internal static Rectangle GetDockHoverBounds(DockEdge edge, Rectangle workArea,
            int dockY, float scale, bool fullyHidden, int hoverHeight)
        {
            // Keep the activation target centered on the cat's last docked height
            // instead of reacting anywhere along the whole screen edge.
            int width = fullyHidden
                ? Math.Max(18, (int)Math.Round(28F * scale))
                : Math.Max(40, (int)Math.Round(56F * scale));
            int normalizedHeight = Math.Max(40, Math.Min(1000, hoverHeight));
            int halfHeight = Math.Max(20,
                (int)Math.Round(normalizedHeight * scale / 2F));
            int x = edge == DockEdge.Left ? workArea.Left : workArea.Right - width;
            int top = Math.Max(workArea.Top, dockY - halfHeight);
            int bottom = Math.Min(workArea.Bottom, dockY + halfHeight);
            return Rectangle.FromLTRB(x, top, x + width, Math.Max(top + 1, bottom));
        }

        private bool IsDockHovering()
        {
            if (!IsDocked) return false;
            Point cursor = Cursor.Position;
            Rectangle workArea = Screen.FromRectangle(Bounds).WorkingArea;
            Rectangle hoverBounds = GetDockHoverBounds(dockEdge, workArea,
                dockCoordinate, uiScale, dockVisibility <= 0.01F,
                appSettings.DockHoverHeight);
            if (hoverBounds.Contains(cursor)) return true;

            Rectangle petBounds = GetPetVisibleBounds();
            petBounds.Offset(Location);
            return petBounds.Contains(cursor);
        }

        private static Bitmap LoadBitmapResource(string resourceName)
        {
            using (Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(resourceName))
            {
                if (stream == null) return null;
                using (Bitmap source = new Bitmap(stream)) return new Bitmap(source);
            }
        }

        private static SpriteFrameMetrics[,] AnalyzeSpriteFrames(Bitmap sheet)
        {
            SpriteFrameMetrics[,] result = new SpriteFrameMetrics[SpriteRows, SpriteColumns];
            for (int row = 0; row < SpriteRows; row++)
                for (int column = 0; column < SpriteColumns; column++)
                    result[row, column] = SpriteFrameMetrics.CreateDefault();
            if (sheet == null) return result;

            Rectangle bounds = new Rectangle(0, 0, sheet.Width, sheet.Height);
            BitmapData data = null;
            try
            {
                data = sheet.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                int stride = data.Stride;
                int absoluteStride = Math.Abs(stride);
                byte[] pixels = new byte[absoluteStride * sheet.Height];
                Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
                int headZoneHeight = Math.Max(1, (int)Math.Round(SpriteCellHeight * 0.32F));

                for (int row = 0; row < SpriteRows; row++)
                {
                    for (int column = 0; column < SpriteColumns; column++)
                    {
                        int minX = SpriteCellWidth;
                        int minY = SpriteCellHeight;
                        int maxX = -1;
                        int maxY = -1;
                        long allX = 0;
                        int allCount = 0;
                        long headX = 0;
                        int headCount = 0;
                        for (int y = 0; y < SpriteCellHeight; y++)
                        {
                            int sheetY = row * SpriteCellHeight + y;
                            if (sheetY >= sheet.Height) break;
                            int rowOffset = stride >= 0
                                ? sheetY * stride
                                : (sheet.Height - 1 - sheetY) * absoluteStride;
                            for (int x = 0; x < SpriteCellWidth; x++)
                            {
                                int sheetX = column * SpriteCellWidth + x;
                                if (sheetX >= sheet.Width) break;
                                int alphaIndex = rowOffset + sheetX * 4 + 3;
                                if (alphaIndex < 0 || alphaIndex >= pixels.Length || pixels[alphaIndex] < 20)
                                    continue;
                                if (x < minX) minX = x;
                                if (x > maxX) maxX = x;
                                if (y < minY) minY = y;
                                if (y > maxY) maxY = y;
                                allX += x;
                                allCount++;
                                if (y < headZoneHeight)
                                {
                                    headX += x;
                                    headCount++;
                                }
                            }
                        }
                        if (allCount == 0) continue;
                        float anchorX = headCount > 0
                            ? (float)headX / headCount
                            : (float)allX / allCount;
                        result[row, column] = new SpriteFrameMetrics(anchorX, maxY + 1,
                            Rectangle.FromLTRB(minX, minY, maxX + 1, maxY + 1), true);
                    }
                }
            }
            catch
            {
                // Default cell-centred metrics still render safely if analysis fails.
            }
            finally
            {
                if (data != null) sheet.UnlockBits(data);
            }
            return result;
        }

        private static Rectangle[] AnalyzeDockSpriteBounds(Bitmap sheet)
        {
            Rectangle[] result = new Rectangle[DockExpressionCount * 2];
            for (int index = 0; index < result.Length; index++)
            {
                Rectangle cell = new Rectangle(index * DockSpriteCellSize, 0,
                    DockSpriteCellSize, DockSpriteCellSize);
                int minX = cell.Right;
                int minY = cell.Bottom;
                int maxX = cell.Left - 1;
                int maxY = cell.Top - 1;
                if (sheet != null)
                {
                    for (int y = cell.Top; y < Math.Min(cell.Bottom, sheet.Height); y++)
                        for (int x = cell.Left; x < Math.Min(cell.Right, sheet.Width); x++)
                            if (sheet.GetPixel(x, y).A > 16)
                            {
                                minX = Math.Min(minX, x);
                                minY = Math.Min(minY, y);
                                maxX = Math.Max(maxX, x);
                                maxY = Math.Max(maxY, y);
                            }
                }
                result[index] = maxX < minX
                    ? new Rectangle(0, 0, DockSpriteCellSize, DockSpriteCellSize)
                    : Rectangle.FromLTRB(minX - cell.Left, minY - cell.Top,
                        maxX - cell.Left + 1, maxY - cell.Top + 1);
            }
            return result;
        }

        private void RenderLayered()
        {
            if (IsDisposed || !IsHandleCreated) return;
            using (Bitmap canvas = new Bitmap(Width, Height, PixelFormat.Format32bppPArgb))
            {
                using (Graphics g = Graphics.FromImage(canvas))
                {
                    g.Clear(Color.Transparent);
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;
                    if (ShouldShowThoughtBubble()) DrawThoughtBubble(g);
                    DrawSprite(g);
                }
                UpdateLayeredBitmap(canvas);
            }
        }

        private void UpdateLayeredBitmap(Bitmap bitmap)
        {
            IntPtr screenDc = GetDC(IntPtr.Zero);
            IntPtr memoryDc = CreateCompatibleDC(screenDc);
            IntPtr bitmapHandle = bitmap.GetHbitmap(Color.FromArgb(0));
            IntPtr oldBitmap = SelectObject(memoryDc, bitmapHandle);
            try
            {
                POINT destination = new POINT { X = Left, Y = Top };
                SIZE size = new SIZE { cx = Width, cy = Height };
                POINT source = new POINT { X = 0, Y = 0 };
                BLENDFUNCTION blend = new BLENDFUNCTION
                {
                    BlendOp = 0,
                    BlendFlags = 0,
                    SourceConstantAlpha = 255,
                    AlphaFormat = 1
                };
                UpdateLayeredWindow(Handle, screenDc, ref destination, ref size, memoryDc,
                    ref source, 0, ref blend, 2);
            }
            finally
            {
                SelectObject(memoryDc, oldBitmap);
                DeleteObject(bitmapHandle);
                DeleteDC(memoryDc);
                ReleaseDC(IntPtr.Zero, screenDc);
            }
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ExStyle |= 0x00080000 | 0x08000000 | 0x00000080; // LAYERED | NOACTIVATE | TOOLWINDOW
                return cp;
            }
        }

        protected override bool ShowWithoutActivation
        {
            get { return true; }
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WmNcHitTest)
            {
                long packed = m.LParam.ToInt64();
                int screenX = unchecked((short)(packed & 0xFFFF));
                int screenY = unchecked((short)((packed >> 16) & 0xFFFF));
                Point clientPoint = PointToClient(new Point(screenX, screenY));
                m.Result = new IntPtr(IsInteractivePoint(clientPoint) ? HtClient : HtTransparent);
                return;
            }
            if (m.Msg == WmMouseActivate)
            {
                m.Result = new IntPtr(3); // MA_NOACTIVATE
                return;
            }
            base.WndProc(ref m);
        }

        [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
        [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter,
            int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
        [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
        [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
        [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
        [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
        [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
        [DllImport("user32.dll", SetLastError = true)] private static extern bool UpdateLayeredWindow(
            IntPtr hwnd, IntPtr hdcDst, ref POINT pptDst, ref SIZE psize, IntPtr hdcSrc,
            ref POINT pptSrc, int crKey, ref BLENDFUNCTION pblend, int dwFlags);

        [StructLayout(LayoutKind.Sequential)] private struct POINT { public int X; public int Y; }
        [StructLayout(LayoutKind.Sequential)] private struct SIZE { public int cx; public int cy; }
        [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct BLENDFUNCTION
        {
            public byte BlendOp; public byte BlendFlags; public byte SourceConstantAlpha; public byte AlphaFormat;
        }

        private void GetSpriteFrame(out int row, out int frame, out int frameCount)
        {
            if (currentState == ReminderState.Busy)
            {
                // The cat follows a calm work rhythm: walk for a while,
                // sit down to think, pause, then stand and resume walking.
                row = BusySpriteRow;
                frameCount = 8;
                frame = GetBusySequenceFrame(animationFrame);
                return;
            }

            int holdTicks;
            switch (currentState)
            {
                case ReminderState.Completed:
                    row = CompletedSpriteRow; frameCount = 4; holdTicks = 3; break;
                case ReminderState.Error:
                    row = ErrorSpriteRow; frameCount = 8; holdTicks = 3; break;
                default:
                    row = IdleSpriteRow; frameCount = 8; holdTicks = 3; break;
            }
            frame = (animationFrame / holdTicks) % frameCount;
        }

        private static int GetBusySequenceFrame(int tick)
        {
            int phase = Math.Abs(tick) % 64;
            if (phase < 40) return (phase / 2) % 4;       // five relaxed walk cycles
            if (phase < 44) return 4;                     // lower into the seat
            if (phase < 60) return 5 + ((phase - 44) / 4) % 2; // think/blink
            return 7;                                    // stand up, then loop
        }

        private bool ShouldMirrorFloatingSprite()
        {
            Point anchor = GetPreferredAnchor();
            Rectangle workArea = Screen.FromPoint(anchor).WorkingArea;
            return ShouldMirrorFloatingSprite(anchor, workArea);
        }

        internal static bool ShouldMirrorFloatingSprite(Point anchor, Rectangle workArea)
        {
            return anchor.X < workArea.Left + workArea.Width / 2;
        }

        private RectangleF GetPetDestination(bool mirror, out Rectangle source,
            out SpriteFrameMetrics metrics)
        {
            int row;
            int frame;
            int frameCount;
            GetSpriteFrame(out row, out frame, out frameCount);
            source = new Rectangle(frame * SpriteCellWidth, row * SpriteCellHeight,
                SpriteCellWidth, SpriteCellHeight);
            metrics = spriteMetrics == null
                ? SpriteFrameMetrics.CreateDefault()
                : spriteMetrics[row, frame];
            if (!metrics.HasPixels) metrics = SpriteFrameMetrics.CreateDefault();

            float visualWidth = petWidth * SpriteScale;
            float visualHeight = petHeight * SpriteScale;
            float scaleX = visualWidth / SpriteCellWidth;
            float scaleY = visualHeight / SpriteCellHeight;
            float targetX = Width / 2F;
            float groundY = Height - Math.Max(2F, (float)Math.Round(3F * uiScale));
            float anchorX = mirror ? SpriteCellWidth - metrics.AnchorX : metrics.AnchorX;
            float x = targetX - anchorX * scaleX;
            float y = groundY - metrics.Bottom * scaleY;
            return new RectangleF(x, y, visualWidth, visualHeight);
        }

        private Rectangle GetPetVisibleBounds()
        {
            if (IsDocked) return GetDockedPoseVisibleBounds();
            bool mirror = ShouldMirrorFloatingSprite();
            Rectangle source;
            SpriteFrameMetrics metrics;
            RectangleF destination = GetPetDestination(mirror, out source, out metrics);
            float scaleX = destination.Width / SpriteCellWidth;
            float scaleY = destination.Height / SpriteCellHeight;
            Rectangle opaque = metrics.OpaqueBounds;
            int opaqueX = mirror ? SpriteCellWidth - opaque.Right : opaque.X;
            RectangleF visible = new RectangleF(
                destination.X + opaqueX * scaleX,
                destination.Y + opaque.Y * scaleY,
                Math.Max(1F, opaque.Width * scaleX),
                Math.Max(1F, opaque.Height * scaleY));
            return Rectangle.Ceiling(visible);
        }

        private int GetDockExpressionIndex()
        {
            return SelectDockExpression(currentState, animationFrame);
        }

        internal static int SelectDockExpression(ReminderState state, int frame)
        {
            int phase = Math.Abs(frame) % 20;
            bool quickBlink = phase == 11 || phase == 14;
            if (state == ReminderState.Completed) return quickBlink ? 1 : 2;
            if (state == ReminderState.Error) return quickBlink ? 1 : 3;
            return quickBlink ? 1 : 0;
        }

        private int GetDockSpriteIndex()
        {
            int directionOffset = dockEdge == DockEdge.Right ? DockExpressionCount : 0;
            return directionOffset + GetDockExpressionIndex();
        }

        private static float SmoothStep(float value)
        {
            value = Math.Max(0F, Math.Min(1F, value));
            return value * value * (3F - 2F * value);
        }

        private void GetDockedPose(out Rectangle source, out Rectangle destination,
            out Rectangle visibleBounds)
        {
            int index = GetDockSpriteIndex();
            source = new Rectangle(index * DockSpriteCellSize, 0,
                DockSpriteCellSize, DockSpriteCellSize);
            int size = Math.Max(84, (int)Math.Round(104F * uiScale));
            float visibility = SmoothStep(dockVisibility);
            int hiddenOffset = (int)Math.Round(size * (1F - visibility));
            int x = dockEdge == DockEdge.Left
                ? -hiddenOffset
                : Width - size + hiddenOffset;
            int y = dockBubbleBelow ? 0 : Height - size;
            destination = new Rectangle(x, y, size, size);

            Rectangle opaque = dockSpriteOpaqueBounds == null
                ? new Rectangle(0, 0, DockSpriteCellSize, DockSpriteCellSize)
                : dockSpriteOpaqueBounds[index];
            float scale = size / (float)DockSpriteCellSize;
            visibleBounds = Rectangle.Ceiling(new RectangleF(
                destination.X + opaque.X * scale,
                destination.Y + opaque.Y * scale,
                Math.Max(1F, opaque.Width * scale),
                Math.Max(1F, opaque.Height * scale)));
        }

        private Rectangle GetDockedPoseVisibleBounds()
        {
            Rectangle source;
            Rectangle destination;
            Rectangle visible;
            GetDockedPose(out source, out destination, out visible);
            return visible;
        }

        private void DrawSprite(Graphics g)
        {
            Rectangle source;
            RectangleF destination;
            Bitmap sheet;
            bool mirror = false;
            if (IsDocked)
            {
                Rectangle dockDestination;
                Rectangle visible;
                GetDockedPose(out source, out dockDestination, out visible);
                destination = dockDestination;
                sheet = dockSpriteSheet;
            }
            else
            {
                SpriteFrameMetrics metrics;
                mirror = ShouldMirrorFloatingSprite();
                destination = GetPetDestination(mirror, out source, out metrics);
                sheet = spriteSheet;
            }
            if (sheet == null) return;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            if (mirror)
            {
                PointF[] destinationPoints =
                {
                    new PointF(destination.Right, destination.Top),
                    new PointF(destination.Left, destination.Top),
                    new PointF(destination.Right, destination.Bottom)
                };
                g.DrawImage(sheet, destinationPoints, source, GraphicsUnit.Pixel);
            }
            else
            {
                g.DrawImage(sheet, destination, source, GraphicsUnit.Pixel);
            }
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
        }

        private bool ShouldShowThoughtBubble()
        {
            return ShouldShowThoughtBubble(IsDocked, currentState, DateTime.UtcNow,
                dockThoughtUntilUtc);
        }

        internal static bool ShouldShowThoughtBubble(bool isDocked, ReminderState state,
            DateTime nowUtc, DateTime dockThoughtUntilUtc)
        {
            // While floating, the cloud is part of the assistant and should remain
            // visible even when idle. Docked mode stays compact and only shows a timed
            // task notification.
            if (!isDocked) return true;
            bool hasTaskState = state == ReminderState.Busy ||
                state == ReminderState.Completed || state == ReminderState.Error;
            return hasTaskState && nowUtc < dockThoughtUntilUtc;
        }

        private Rectangle GetBubbleBounds()
        {
            int y = dockBubbleBelow ? Height - bubbleHeight : 0;
            return new Rectangle((Width - bubbleWidth) / 2, y, bubbleWidth, bubbleHeight);
        }

        private void GetThoughtDotBounds(out Rectangle largeDot, out Rectangle smallDot)
        {
            int largeDotW = Math.Max(13, (int)Math.Round(17F * uiScale));
            int largeDotH = Math.Max(11, (int)Math.Round(15F * uiScale));
            int smallDotW = Math.Max(8, (int)Math.Round(11F * uiScale));
            int smallDotH = Math.Max(7, (int)Math.Round(10F * uiScale));
            Rectangle cloud = GetBubbleBounds();
            Rectangle pet = GetPetVisibleBounds();
            int petCenterX = pet.Left + pet.Width / 2;
            int cloudCenterX = cloud.Left + cloud.Width / 2;
            int smallCenterX = IsDocked ? petCenterX : cloudCenterX - (int)Math.Round(11F * uiScale);
            int largeCenterX = IsDocked
                ? (petCenterX * 2 + cloudCenterX) / 3
                : cloudCenterX - (int)Math.Round(7F * uiScale);
            int smallDotX = smallCenterX - smallDotW / 2;
            int largeDotX = largeCenterX - largeDotW / 2;
            int smallDotY;
            int largeDotY;

            if (dockBubbleBelow)
            {
                smallDotY = pet.Bottom + (int)Math.Round(6F * uiScale);
                largeDotY = Math.Min(cloud.Top - largeDotH - (int)Math.Round(5F * uiScale),
                    smallDotY + smallDotH + (int)Math.Round(5F * uiScale));
            }
            else
            {
                int minimumSmallY = cloud.Bottom + (int)Math.Round(10F * uiScale);
                int maximumSmallY = cloud.Bottom + (int)Math.Round(30F * uiScale);
                int desiredSmallY = pet.Top - smallDotH - (int)Math.Round(6F * uiScale);
                smallDotY = Math.Max(minimumSmallY, Math.Min(maximumSmallY, desiredSmallY));
                largeDotY = Math.Max(cloud.Bottom - (int)Math.Round(6F * uiScale),
                    smallDotY - largeDotH - (int)Math.Round(5F * uiScale));
            }

            largeDot = new Rectangle(largeDotX, largeDotY, largeDotW, largeDotH);
            smallDot = new Rectangle(smallDotX, smallDotY, smallDotW, smallDotH);
        }

        private bool ShouldShowLightBulb()
        {
            return ShouldShowLightBulb(currentState);
        }

        internal static bool ShouldShowLightBulb(ReminderState state)
        {
            return state == ReminderState.Busy ||
                state == ReminderState.Completed || state == ReminderState.Error;
        }

        private RectangleF GetContentViewportBounds()
        {
            return CalculateCloudContentBounds(GetBubbleBounds(), ShouldShowLightBulb(),
                contentViewportWidth, contentViewportHeight);
        }

        internal static int GetCloudNotificationSeconds(ReminderState state, int configuredSeconds)
        {
            int safeConfiguredSeconds = Math.Max(1, configuredSeconds);
            return state == ReminderState.Error ? Math.Max(10, safeConfiguredSeconds) :
                safeConfiguredSeconds;
        }

        internal static RectangleF CalculateCloudContentBounds(Rectangle cloud,
            bool reserveLightBulbSpace, int preferredWidth, int preferredHeight)
        {
            // This rectangle stays within the narrowest part of the cloud silhouette.
            // Text is clipped to it, so long titles can only wrap/scroll inside the cloud.
            float x = cloud.X + cloud.Width * (reserveLightBulbSpace ? 0.30F : 0.22F);
            float y = cloud.Y + cloud.Height * 0.31F;
            float maximumRight = cloud.X + cloud.Width * 0.80F;
            float maximumBottom = cloud.Y + cloud.Height * 0.76F;
            float width = Math.Min(Math.Max(1, preferredWidth), Math.Max(1F, maximumRight - x));
            float height = Math.Min(Math.Max(1, preferredHeight), Math.Max(1F, maximumBottom - y));
            return new RectangleF(x, y, Math.Max(1F, width), Math.Max(1F, height));
        }

        internal static RectangleF CalculateCloudHeaderBounds(Rectangle cloud)
        {
            // Keep the complete status line centered in the cloud's upper safe area.
            return new RectangleF(cloud.X + cloud.Width * 0.20F,
                cloud.Y + cloud.Height * 0.14F, cloud.Width * 0.60F,
                cloud.Height * 0.20F);
        }

        internal static string FormatBusyMetadata(string stepProgress, int sessionIndex,
            int sessionCount)
        {
            List<string> parts = new List<string>();
            if (!String.IsNullOrEmpty(stepProgress))
                parts.Add("(" + stepProgress + ")");
            if (sessionCount > 1)
            {
                int safeIndex = Math.Max(0, Math.Min(sessionIndex, sessionCount - 1));
                parts.Add("会话(" + (safeIndex + 1) + "/" + sessionCount + ")");
            }
            return String.Join("·", parts.ToArray());
        }

        internal static string FormatBusyHeader(string stepProgress, int sessionIndex,
            int sessionCount)
        {
            string metadata = FormatBusyMetadata(stepProgress, sessionIndex, sessionCount);
            if (String.IsNullOrEmpty(metadata)) return "进行中";
            return metadata.StartsWith("(", StringComparison.Ordinal)
                ? "进行中" + metadata
                : "进行中·" + metadata;
        }

        private void DrawThoughtBubble(Graphics g)
        {
            Rectangle cloud = GetBubbleBounds();
            if (cloudBubble != null)
            {
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(cloudBubble, cloud);
            }

            Rectangle largeDot;
            Rectangle smallDot;
            GetThoughtDotBounds(out largeDot, out smallDot);
            DrawPixelThoughtDot(g, largeDot, uiScale);
            DrawPixelThoughtDot(g, smallDot, uiScale);

            float headerFontSize = (currentState == ReminderState.Busy ? 10.5F : 11.5F) * uiScale;
            float contentFontSize = GetContentFontSize();
            using (Font headerFont = new Font("Microsoft YaHei UI", headerFontSize, FontStyle.Bold))
            using (Font contentFont = new Font("Microsoft YaHei UI", contentFontSize, FontStyle.Bold))
            using (SolidBrush headerBrush = new SolidBrush(HeaderTextColor))
            using (SolidBrush contentBrush = new SolidBrush(ContentTextColor))
            using (StringFormat headerFormat = (StringFormat)StringFormat.GenericTypographic.Clone())
            {
                headerFormat.FormatFlags = StringFormatFlags.NoWrap;
                headerFormat.Trimming = StringTrimming.EllipsisCharacter;
                string header;
                if (currentState == ReminderState.Busy)
                {
                    string progress = taskIndex >= 0 && taskIndex < taskProgressLabels.Count
                        ? taskProgressLabels[taskIndex] : null;
                    header = FormatBusyHeader(progress, taskIndex, taskTitles.Count);
                }
                else if (currentState == ReminderState.Completed) header = "已完成";
                else if (currentState == ReminderState.Error) header = "异常";
                else header = statusText;

                bool reserveLightBulbSpace = ShouldShowLightBulb();
                headerFormat.Alignment = StringAlignment.Center;
                g.DrawString(header, headerFont, headerBrush,
                    CalculateCloudHeaderBounds(cloud), headerFormat);

                if (reserveLightBulbSpace)
                {
                    bool errorBulb = currentState == ReminderState.Error;
                    DrawPixelLightBulb(g, cloud.X + bubbleWidth * 0.125F,
                        cloud.Y + bubbleHeight * 0.285F, uiScale,
                        errorBulb ? Color.FromArgb(226, 62, 55) : BulbGlowColor,
                        errorBulb ? Color.FromArgb(255, 174, 154) : BulbHighlightColor);
                }

                string sourceText = NormalizeDisplayText(GetDisplayedText());
                RectangleF viewport = GetContentViewportBounds();
                string displayed = GetWrappedDisplayText(g, sourceText, contentFont, viewport.Width);
                int textHeight = MeasureWrappedTextHeight(g, displayed, contentFont);
                CacheTextMeasurement(sourceText, viewport.Width, textHeight);

                GraphicsState state = g.Save();
                g.SetClip(viewport);
                DrawSmoothScrollingContent(g, displayed, contentFont, viewport);
                g.Restore(state);
            }
        }

        private void DrawPixelThoughtDot(Graphics g, Rectangle bounds, float scale)
        {
            SmoothingMode previous = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.None;
            int notch = Math.Max(2, (int)Math.Round(3F * scale));
            notch = Math.Min(notch, Math.Max(1, Math.Min(bounds.Width, bounds.Height) / 3));
            Point[] outer = CreatePixelOctagon(bounds, notch);
            Rectangle innerBounds = Rectangle.Inflate(bounds, -notch, -notch);
            int innerNotch = Math.Max(1, notch / 2);
            Point[] inner = innerBounds.Width > 0 && innerBounds.Height > 0
                ? CreatePixelOctagon(innerBounds, Math.Min(innerNotch,
                    Math.Max(1, Math.Min(innerBounds.Width, innerBounds.Height) / 3)))
                : null;
            using (SolidBrush outline = new SolidBrush(DotOutlineColor))
            using (SolidBrush fill = new SolidBrush(DotFillColor))
            {
                g.FillPolygon(outline, outer);
                if (inner != null) g.FillPolygon(fill, inner);
            }
            g.SmoothingMode = previous;
        }

        private static Point[] CreatePixelOctagon(Rectangle bounds, int notch)
        {
            return new[]
            {
                new Point(bounds.Left + notch, bounds.Top),
                new Point(bounds.Right - notch, bounds.Top),
                new Point(bounds.Right, bounds.Top + notch),
                new Point(bounds.Right, bounds.Bottom - notch),
                new Point(bounds.Right - notch, bounds.Bottom),
                new Point(bounds.Left + notch, bounds.Bottom),
                new Point(bounds.Left, bounds.Bottom - notch),
                new Point(bounds.Left, bounds.Top + notch)
            };
        }

        private static void DrawPixelLightBulb(Graphics g, float x, float y, float scale,
            Color glowColor, Color highlightColor)
        {
            int px = Math.Max(2, (int)Math.Round(2.8F * scale));
            int ox = (int)Math.Round(x);
            int oy = (int)Math.Round(y);
            SmoothingMode previous = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.None;
            using (SolidBrush outline = new SolidBrush(Color.FromArgb(68, 43, 25)))
            using (SolidBrush glow = new SolidBrush(glowColor))
            using (SolidBrush highlight = new SolidBrush(highlightColor))
            using (SolidBrush baseBrush = new SolidBrush(Color.FromArgb(91, 78, 70)))
            {
                Action<Brush, int, int, int, int> cells = delegate(Brush brush, int cx, int cy, int cw, int ch)
                {
                    g.FillRectangle(brush, ox + cx * px, oy + cy * px, cw * px, ch * px);
                };

                // Pixel rays.
                cells(outline, 6, 0, 1, 2);
                cells(outline, 2, 2, 1, 1);
                cells(outline, 10, 2, 1, 1);
                cells(outline, 0, 6, 2, 1);
                cells(outline, 11, 6, 2, 1);

                // Dark-brown silhouette.
                int[,] silhouette = new int[,]
                {
                    {3, 4, 5}, {4, 3, 7}, {5, 2, 9}, {6, 2, 9},
                    {7, 2, 9}, {8, 3, 7}, {9, 4, 5}, {10, 5, 3},
                    {11, 4, 5}, {12, 4, 5}, {13, 5, 3}
                };
                for (int i = 0; i < silhouette.GetLength(0); i++)
                    cells(outline, silhouette[i, 1], silhouette[i, 0], silhouette[i, 2], 1);

                // Warm bulb interior and crisp highlight.
                cells(glow, 4, 4, 5, 1);
                cells(glow, 3, 5, 7, 3);
                cells(glow, 4, 8, 5, 1);
                cells(glow, 5, 9, 3, 1);
                cells(highlight, 4, 4, 2, 1);
                cells(highlight, 3, 5, 2, 2);
                cells(outline, 5, 7, 1, 2);
                cells(outline, 7, 7, 1, 2);
                cells(outline, 6, 9, 1, 1);

                // Lamp base.
                cells(baseBrush, 5, 11, 3, 2);
                cells(outline, 5, 13, 3, 1);
            }
            g.SmoothingMode = previous;
        }

        private static string NormalizeDisplayText(string text)
        {
            if (String.IsNullOrEmpty(text)) return String.Empty;
            string canonical = text.Replace("\r\n", "\n").Replace('\r', '\n');
            string[] rawLines = canonical.Split(new[] { '\n' });
            StringBuilder result = new StringBuilder(canonical.Length);
            for (int i = 0; i < rawLines.Length; i++)
            {
                string line = Regex.Replace(rawLines[i], "\\s+", " ").Trim();
                if (line.Length == 0) continue;
                if (result.Length > 0) result.Append('\n');
                result.Append(line);
            }
            return result.ToString();
        }

        private static float GetContentFontSize()
        {
            // A slightly smaller body font leaves room for three readable lines
            // while keeping the header visually prominent like the reference UI.
            return 9.5F;
        }

        private static int MeasureWrappedTextHeight(Graphics g, string text, Font font)
        {
            int lineCount = 1;
            if (!String.IsNullOrEmpty(text))
                for (int i = 0; i < text.Length; i++) if (text[i] == '\n') lineCount++;
            return Math.Max(1, (int)Math.Ceiling(lineCount * font.GetHeight(g)));
        }

        private string GetWrappedDisplayText(Graphics g, string source, Font font, float width)
        {
            string normalized = NormalizeDisplayText(source);
            int integerWidth = Math.Max(1, (int)Math.Ceiling(width));
            if (String.Equals(wrappedSource, normalized, StringComparison.Ordinal) &&
                wrappedWidth == integerWidth && Math.Abs(wrappedScale - uiScale) < 0.01F)
                return wrappedText;

            if (String.IsNullOrEmpty(normalized))
            {
                wrappedSource = normalized;
                wrappedText = String.Empty;
                wrappedWidth = integerWidth;
                wrappedScale = uiScale;
                return wrappedText;
            }

            StringBuilder result = new StringBuilder(normalized.Length + 16);
            string[] paragraphs = normalized.Split(new[] { '\n' });
            for (int paragraphIndex = 0; paragraphIndex < paragraphs.Length; paragraphIndex++)
            {
                string paragraph = paragraphs[paragraphIndex].Trim();
                if (paragraph.Length == 0) continue;
                StringBuilder line = new StringBuilder(paragraph.Length);
                for (int i = 0; i < paragraph.Length; i++)
                {
                    char character = paragraph[i];
                    string candidate = line.ToString() + character;
                    if (line.Length > 0 && MeasureSingleLineWidth(g, candidate, font) > width)
                    {
                        AppendWrappedLine(result, line.ToString());
                        line.Length = 0;
                    }
                    line.Append(character);
                }
                AppendWrappedLine(result, line.ToString());
            }

            wrappedSource = normalized;
            wrappedText = result.ToString();
            wrappedWidth = integerWidth;
            wrappedScale = uiScale;
            return wrappedText;
        }

        private void DrawSmoothScrollingContent(Graphics g, string text, Font baseFont,
            RectangleF viewport)
        {
            if (String.IsNullOrEmpty(text)) return;
            EnsureSmoothTextBitmap(g, text, baseFont, viewport.Width);
            if (smoothTextBitmap == null || smoothTextLines.Length == 0) return;

            InterpolationMode oldInterpolation = g.InterpolationMode;
            PixelOffsetMode oldPixelOffset = g.PixelOffsetMode;
            CompositingQuality oldCompositing = g.CompositingQuality;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.CompositingQuality = CompositingQuality.HighQuality;
            try
            {
                // Move one pre-rendered text layer as a whole, just like a TV
                // subtitle track.  Every line keeps exactly the same font size;
                // only its vertical position changes continuously.
                float contentHeight = smoothTextLines.Length * smoothTextLineHeight;
                g.DrawImage(smoothTextBitmap,
                    new RectangleF(viewport.X, viewport.Y - scrollOffset,
                        viewport.Width, contentHeight),
                    new RectangleF(0F, 0F, smoothTextBitmap.Width, smoothTextBitmap.Height),
                    GraphicsUnit.Pixel);
            }
            finally
            {
                g.InterpolationMode = oldInterpolation;
                g.PixelOffsetMode = oldPixelOffset;
                g.CompositingQuality = oldCompositing;
            }
        }
        private void EnsureSmoothTextBitmap(Graphics target, string text, Font baseFont,
            float viewportWidth)
        {
            int width = Math.Max(1, (int)Math.Ceiling(viewportWidth));
            if (smoothTextBitmap != null &&
                String.Equals(smoothTextSource, text, StringComparison.Ordinal) &&
                smoothTextWidth == width && Math.Abs(smoothTextScale - uiScale) < 0.01F)
                return;

            DisposeSmoothTextBitmap();
            smoothTextSource = text;
            smoothTextWidth = width;
            smoothTextScale = uiScale;
            smoothTextLines = text.Split(new[] { '\n' });
            smoothTextLineHeight = Math.Max(1F, baseFont.GetHeight(target));
            smoothTextSourceLineHeight = Math.Max(1,
                (int)Math.Ceiling(smoothTextLineHeight * TextSupersample));

            int bitmapWidth = Math.Max(1, width * TextSupersample);
            int bitmapHeight = Math.Max(1, smoothTextLines.Length * smoothTextSourceLineHeight);
            smoothTextBitmap = new Bitmap(bitmapWidth, bitmapHeight, PixelFormat.Format32bppPArgb);
            using (Graphics textGraphics = Graphics.FromImage(smoothTextBitmap))
            using (Font renderFont = new Font(baseFont.FontFamily,
                baseFont.Size * TextSupersample, baseFont.Style, GraphicsUnit.Point))
            using (SolidBrush textBrush = new SolidBrush(ContentTextColor))
            using (StringFormat lineFormat = (StringFormat)StringFormat.GenericTypographic.Clone())
            {
                textGraphics.Clear(Color.Transparent);
                textGraphics.SmoothingMode = SmoothingMode.AntiAlias;
                textGraphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
                lineFormat.FormatFlags = StringFormatFlags.NoWrap;
                lineFormat.Alignment = StringAlignment.Near;
                for (int i = 0; i < smoothTextLines.Length; i++)
                    textGraphics.DrawString(smoothTextLines[i], renderFont, textBrush,
                        new PointF(0F, i * smoothTextSourceLineHeight), lineFormat);
            }
        }

        private void DisposeSmoothTextBitmap()
        {
            if (smoothTextBitmap != null)
            {
                smoothTextBitmap.Dispose();
                smoothTextBitmap = null;
            }
            smoothTextSource = String.Empty;
            smoothTextWidth = -1;
            smoothTextLines = new string[0];
        }
        private static void AppendWrappedLine(StringBuilder result, string line)
        {
            if (String.IsNullOrEmpty(line)) return;
            string trimmed = line.Trim();
            if (trimmed.Length == 0) return;
            if (result.Length > 0) result.Append('\n');
            result.Append(trimmed);
        }

        private static float MeasureSingleLineWidth(Graphics g, string text, Font font)
        {
            if (String.IsNullOrEmpty(text)) return 0F;
            using (StringFormat format = (StringFormat)StringFormat.GenericTypographic.Clone())
            {
                format.FormatFlags = StringFormatFlags.NoWrap | StringFormatFlags.NoClip;
                return g.MeasureString(text, font, new SizeF(100000F, 100000F), format).Width;
            }
        }

        private void CacheTextMeasurement(string source, float width, int height)
        {
            measuredSource = source ?? String.Empty;
            measuredWidth = Math.Max(1, (int)Math.Ceiling(width));
            measuredHeight = Math.Max(1, height);
            measuredScale = uiScale;
        }

        private void InvalidateTextMeasurement()
        {
            DisposeSmoothTextBitmap();
            wrappedSource = null;
            wrappedText = String.Empty;
            wrappedWidth = -1;
            wrappedScale = 0F;
            measuredSource = String.Empty;
            measuredWidth = -1;
            measuredHeight = 0;
            measuredScale = 0F;
        }

        private int GetMeasuredTextHeight(string text)
        {
            string source = NormalizeDisplayText(text);
            int width = Math.Max(1, (int)Math.Ceiling(GetContentViewportBounds().Width));
            if (String.Equals(measuredSource, source, StringComparison.Ordinal) &&
                measuredWidth == width && Math.Abs(measuredScale - uiScale) < 0.01F &&
                measuredHeight > 0)
                return measuredHeight;

            using (Bitmap bitmap = new Bitmap(1, 1, PixelFormat.Format32bppArgb))
            using (Graphics graphics = Graphics.FromImage(bitmap))
            using (Font font = new Font("Microsoft YaHei UI", GetContentFontSize() * uiScale,
                FontStyle.Bold))
            {
                string wrapped = GetWrappedDisplayText(graphics, source, font, width);
                int height = MeasureWrappedTextHeight(graphics, wrapped, font);
                CacheTextMeasurement(source, width, height);
                return height;
            }
        }

        private string GetSelectedTaskTitle()
        {
            if (taskTitles.Count == 0) return null;
            return taskTitles[Math.Max(0, Math.Min(taskIndex, taskTitles.Count - 1))] ?? String.Empty;
        }

        private string GetDisplayedText()
        {
            string title = GetSelectedTaskTitle();
            return title ?? thoughtText ?? String.Empty;
        }

        private int FindTaskIndex(string selectedTitle)
        {
            if (taskTitles.Count == 0 || String.IsNullOrEmpty(selectedTitle)) return -1;
            for (int i = 0; i < taskTitles.Count; i++)
                if (String.Equals(taskTitles[i], selectedTitle, StringComparison.Ordinal)) return i;
            return -1;
        }

        private void ResetScroll()
        {
            scrollOffset = 0;
            scrollHoldSeconds = ScrollStartHoldSeconds;
            scrollCycleSeconds = 0F;
            scrollAtEnd = false;
            lastDisplayedText = NormalizeDisplayText(GetDisplayedText());
        }

        private bool AdvanceScroll(float elapsedSeconds)
        {
            string text = NormalizeDisplayText(GetDisplayedText());
            if (!String.Equals(text, NormalizeDisplayText(lastDisplayedText), StringComparison.Ordinal))
            {
                ResetScroll();
                return true;
            }

            int viewportHeight = Math.Max(1,
                (int)Math.Ceiling(GetContentViewportBounds().Height));
            int maxOffset = Math.Max(0, GetMeasuredTextHeight(text) - viewportHeight);
            if (maxOffset <= 0)
            {
                // Short tasks do not scroll, but still participate in the same
                // forward-only task rotation cycle.
                bool offsetChanged = scrollOffset != 0F;
                scrollOffset = 0;
                scrollCycleSeconds += elapsedSeconds;
                if (scrollCycleSeconds >= ShortTaskDisplaySeconds)
                {
                    MoveToNextTask();
                    return true;
                }
                return offsetChanged;
            }

            if (scrollHoldSeconds > 0F)
            {
                scrollHoldSeconds = Math.Max(0F, scrollHoldSeconds - elapsedSeconds);
                return false;
            }
            if (!scrollAtEnd)
            {
                float previousOffset = scrollOffset;
                scrollOffset = Math.Min(maxOffset, scrollOffset +
                    ScrollSpeedPixelsPerSecond * elapsedSeconds);
                if (scrollOffset >= maxOffset)
                {
                    scrollOffset = maxOffset;
                    scrollAtEnd = true;
                    scrollHoldSeconds = ScrollEndHoldSeconds;
                }
                return Math.Abs(scrollOffset - previousOffset) > 0.01F;
            }

            if (scrollHoldSeconds > 0F)
            {
                scrollHoldSeconds = Math.Max(0F, scrollHoldSeconds - elapsedSeconds);
                return false;
            }
            // Never reverse back to the top. Move forward to the next task, or
            // restart the only task from offset zero when there is no next task.
            MoveToNextTask();
            return true;
        }

        private void MoveToNextTask()
        {
            if (taskTitles.Count > 1)
                taskIndex = (taskIndex + 1) % taskTitles.Count;
            else
                taskIndex = 0;
            ResetScroll();
        }

        private bool IsTaskSwitchPoint(Point clientPoint)
        {
            return IsTaskSwitchPoint(IsDocked, ShouldShowThoughtBubble(), currentState,
                taskTitles.Count, GetBubbleBounds(), GetContentViewportBounds(), clientPoint);
        }

        internal static bool IsTaskSwitchPoint(bool isDocked, bool bubbleVisible,
            ReminderState state, int taskCount, Rectangle bubbleBounds,
            RectangleF contentBounds, Point clientPoint)
        {
            if (!bubbleVisible || taskCount <= 1 || state == ReminderState.Idle) return false;
            // A docked notification is brief and compact, so the complete cloud is the
            // task selector. Floating mode keeps the existing text-only selector so the
            // rest of the cloud can still be used as a drag surface.
            if (isDocked) return bubbleBounds.Contains(clientPoint);
            return contentBounds.Contains(clientPoint.X, clientPoint.Y);
        }

        private bool IsInteractivePoint(Point clientPoint)
        {
            if (GetPetVisibleBounds().Contains(clientPoint)) return true;
            if (!ShouldShowThoughtBubble()) return false;
            if (GetBubbleBounds().Contains(clientPoint)) return true;
            Rectangle largeDot;
            Rectangle smallDot;
            GetThoughtDotBounds(out largeDot, out smallDot);
            return largeDot.Contains(clientPoint) || smallDot.Contains(clientPoint);
        }

        internal bool SwitchFromContentClick()
        {
            if (taskTitles.Count <= 1) return false;
            MoveToNextTask();
            if (IsDocked)
            {
                DateTime now = DateTime.UtcNow;
                dockVisibility = 1F;
                dockLastContentChangeUtc = now;
                dockThoughtUntilUtc = now.AddSeconds(GetCloudNotificationSeconds(
                    currentState, appSettings.DockNotificationSeconds));
                dockHoverRevealUntilUtc = now.AddSeconds(appSettings.DockRevealSeconds);
            }
            RenderLayered();
            return true;
        }

        internal int SelectedTaskIndex
        {
            get { return taskIndex; }
        }

        internal bool IsDragActive
        {
            get { return dragPending || dragging; }
        }

        private void StartDrag(object sender, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            if (IsTaskSwitchPoint(e.Location))
            {
                // In docked mode the whole visible cloud is a selector; while floating,
                // only the task text is reserved so the remaining cloud stays draggable.
                SwitchFromContentClick();
                return;
            }
            if (!IsInteractivePoint(e.Location)) return;

            dragStartedDocked = IsDocked;
            if (dragStartedDocked)
            {
                dockVisibility = 1F;
                RenderLayered();
            }
            dragPending = true;
            dragging = false;
            dragStartCursor = Cursor.Position;
            dragStartLocation = Location;
            Capture = true;
        }

        private void Drag(object sender, MouseEventArgs e)
        {
            if (!IsDragActive) return;

            bool wasMirrored = !IsDocked && ShouldMirrorFloatingSprite();
            Point cursor = Cursor.Position;
            int dx = cursor.X - dragStartCursor.X;
            int dy = cursor.Y - dragStartCursor.Y;
            int threshold = Math.Max(3, SystemInformation.DragSize.Width / 3);
            if (!dragging && Math.Abs(dx) + Math.Abs(dy) < threshold) return;

            if (!dragging && IsDocked)
            {
                UndockForDrag(cursor);
                dx = 0;
                dy = 0;
            }
            dragging = true;
            SetBounds(dragStartLocation.X + dx, dragStartLocation.Y + dy,
                Width, Height, BoundsSpecified.Location);
            RememberCurrentAnchor();
            if (!IsDocked && wasMirrored != ShouldMirrorFloatingSprite()) RenderLayered();
        }

        private void EndDrag(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left) FinishDrag();
        }

        private void AssistantMouseCaptureChanged(object sender, EventArgs e)
        {
            if (IsDragActive && !Capture) FinishDrag();
        }

        private void FinishDrag()
        {
            if (!IsDragActive) return;
            bool moved = dragging;
            bool startedDocked = dragStartedDocked;
            dragging = false;
            dragPending = false;
            dragStartedDocked = false;
            Capture = false;
            if (!moved)
            {
                if (startedDocked && IsDocked)
                {
                    dockLastContentChangeUtc = DateTime.UtcNow;
                    dockVisibility = 1F;
                    RenderLayered();
                }
                return;
            }

            RememberCurrentAnchor();
            bool changed = RecalculateAdaptiveLayout();
            if (!TrySnapToEdge(Cursor.Position))
            {
                ClampToWorkingArea();
                RememberCurrentAnchor();
                if (changed) RenderLayered();
            }
            SaveCurrentPosition();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                SaveCurrentPosition();
                if (spriteSheet != null) spriteSheet.Dispose();
                if (dockSpriteSheet != null) dockSpriteSheet.Dispose();
                if (cloudBubble != null) cloudBubble.Dispose();
                DisposeSmoothTextBitmap();
            }
            base.Dispose(disposing);
        }
    }

    internal enum DockEdge { None, Left, Right }

    internal enum ReminderState { Idle, Busy, Completed, Error }

    internal static class StatusIconFactory
    {
        public const int BusyFrameCount = 24;
        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern bool DestroyIcon(IntPtr handle);

        public static Icon CreateIcon(int size, ReminderState state, int frame)
        {
            using (Bitmap bitmap = CreateBitmap(size, state, frame))
            {
                IntPtr handle = bitmap.GetHicon();
                try
                {
                    using (Icon temporary = Icon.FromHandle(handle)) return (Icon)temporary.Clone();
                }
                finally { DestroyIcon(handle); }
            }
        }

        public static Bitmap CreateBitmap(int size, ReminderState state, int frame)
        {
            Bitmap bitmap = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(bitmap))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.CompositingQuality = CompositingQuality.HighQuality;
                g.Clear(Color.Transparent);
                g.ScaleTransform(size / 64f, size / 64f);

                switch (state)
                {
                    case ReminderState.Busy:
                        DrawBreathingYellowDot(g, frame);
                        break;
                    case ReminderState.Completed:
                        DrawCompletedDot(g);
                        break;
                    case ReminderState.Error:
                        DrawErrorDot(g);
                        break;
                    default:
                        DrawIdleDot(g);
                        break;
                }
            }
            return bitmap;
        }

        private static void DrawIdleDot(Graphics g)
        {
            DrawStatusDot(g, Color.FromArgb(255, 34, 197, 94), 23f, 1f);
        }

        private static void DrawBreathingYellowDot(Graphics g, int frame)
        {
            double t = (frame % StatusIconFactory.BusyFrameCount) * Math.PI * 2.0 / StatusIconFactory.BusyFrameCount;
            float pulse = (float)(0.5 + 0.5 * Math.Sin(t));
            float radius = 20f + 5f * pulse;
            float alpha = 0.75f + 0.25f * pulse;
            DrawStatusDot(g, Color.FromArgb(255, 250, 204, 21), radius, alpha);
        }

        private static void DrawCompletedDot(Graphics g)
        {
            DrawStatusDot(g, Color.FromArgb(255, 34, 197, 94), 23f, 1f);
            using (Pen check = new Pen(Color.White, 3.2f))
            {
                check.StartCap = LineCap.Round;
                check.EndCap = LineCap.Round;
                check.LineJoin = LineJoin.Round;
                g.DrawLines(check, new PointF[]
                {
                    new PointF(25f, 33f),
                    new PointF(30f, 38f),
                    new PointF(40f, 27f)
                });
            }
        }

        private static void DrawErrorDot(Graphics g)
        {
            DrawStatusDot(g, Color.FromArgb(255, 229, 57, 53), 23f, 1f);
        }

        private static void DrawStatusDot(Graphics g, Color color, float radius, float alpha)
        {
            float cx = 32f, cy = 32f;
            float glowRadius = radius + 6f;
            int glowAlpha = (int)(80 * alpha);
            using (Brush glow = new SolidBrush(Color.FromArgb(glowAlpha, color.R, color.G, color.B)))
                g.FillEllipse(glow, cx - glowRadius, cy - glowRadius, glowRadius * 2f, glowRadius * 2f);

            int fillAlpha = (int)(255 * alpha);
            Color solid = Color.FromArgb(fillAlpha, color.R, color.G, color.B);
            using (GraphicsPath path = new GraphicsPath())
            {
                path.AddEllipse(cx - radius, cy - radius, radius * 2f, radius * 2f);
                using (PathGradientBrush brush = new PathGradientBrush(path))
                {
                    brush.CenterColor = Lighten(solid, 0.45f);
                    brush.SurroundColors = new Color[] { solid };
                    brush.CenterPoint = new PointF(cx - radius * 0.35f, cy - radius * 0.35f);
                    g.FillPath(brush, path);
                }
            }

            using (Pen border = new Pen(Color.FromArgb(245, 255, 255, 255), 2.6f))
                g.DrawEllipse(border, cx - radius, cy - radius, radius * 2f, radius * 2f);

            using (Brush highlight = new SolidBrush(Color.FromArgb(150, 255, 255, 255)))
                g.FillEllipse(highlight, cx - radius * 0.58f, cy - radius * 0.66f,
                    radius * 0.55f, radius * 0.38f);
        }

        private static Color Lighten(Color color, float amount)
        {
            int r = color.R + (int)((255 - color.R) * amount);
            int g = color.G + (int)((255 - color.G) * amount);
            int b = color.B + (int)((255 - color.B) * amount);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            return Color.FromArgb(color.A, r, g, b);
        }

    }

    internal sealed class CodexSessionMonitor : IDisposable
    {
        private const int ReadBufferSize = 65536;
        private const int MaximumBufferedLineCharacters = 262144;
        private const int MaximumTrackedFiles = 40;
        private const int FullDiscoveryIntervalSeconds = 120;
        private const int StaleTurnGraceSeconds = 600;
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
        private static readonly JavaScriptSerializer Json = new JavaScriptSerializer();
        private string sessionsRoot;
        private readonly Dictionary<string, TailState> files = new Dictionary<string, TailState>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, ActiveTurn> activeTurns = new Dictionary<string, ActiveTurn>(StringComparer.Ordinal);
        private readonly Dictionary<string, TaskPlanProgress> plansByFile =
            new Dictionary<string, TaskPlanProgress>(StringComparer.OrdinalIgnoreCase);
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
        public CodexSessionMonitor() : this(GetDefaultSessionsRoot()) { }
        internal CodexSessionMonitor(string sessionsRootPath)
        {
            sessionsRoot = NormalizeSessionsRoot(sessionsRootPath);
            DiscoverFiles(true);
        }
        public int ActiveCount { get { return activeTurns.Count; } }
        public string LastCompletedTitle { get { return lastCompletedTitle; } }
        public string LastAbortedTitle { get { return lastAbortedTitle; } }
        public string LastEventType { get { return lastEventType; } }
        public string LastEventFile { get { return lastEventFile; } }

        public int GetActiveTitleIndex(string sourcePath)
        {
            if (String.IsNullOrEmpty(sourcePath)) return -1;
            int index = 0;
            foreach (ActiveTurn turn in OrderedActiveTurns())
            {
                if (String.Equals(turn.SourcePath, sourcePath, StringComparison.OrdinalIgnoreCase))
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
            get { return activeTurns.Values.Sum(delegate(ActiveTurn turn) { return turn.Plan == null ? 0 : turn.Plan.TotalSteps; }); }
        }

        public int CompletedPlanStepCount
        {
            get { return activeTurns.Values.Sum(delegate(ActiveTurn turn) { return turn.Plan == null ? 0 : turn.Plan.CompletedSteps; }); }
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
            return activeTurns.Values.OrderBy(delegate(ActiveTurn item) { return item.StartSequence; });
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

        public static string GetDefaultSessionsRoot()
        {
            string codexHome = Environment.GetEnvironmentVariable("CODEX_HOME");
            if (String.IsNullOrWhiteSpace(codexHome))
                codexHome = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".codex");
            return Path.Combine(codexHome, "sessions");
        }

        private static string NormalizeSessionsRoot(string path)
        {
            if (String.IsNullOrWhiteSpace(path)) path = GetDefaultSessionsRoot();
            try
            {
                return Path.GetFullPath(Environment.ExpandEnvironmentVariables(path.Trim()));
            }
            catch { return GetDefaultSessionsRoot(); }
        }

        public void SetSessionsRoot(string path)
        {
            if (disposed) return;
            string nextRoot = NormalizeSessionsRoot(path);
            if (String.Equals(nextRoot, sessionsRoot, StringComparison.OrdinalIgnoreCase)) return;
            sessionsRoot = nextRoot;
            files.Clear();
            activeTurns.Clear();
            plansByFile.Clear();
            lastCompletedTitle = null;
            lastAbortedTitle = null;
            nextDiscoveryUtc = DateTime.MinValue;
            nextFullDiscoveryUtc = DateTime.MinValue;
            nextStaleTurnCheckUtc = DateTime.MinValue;
            DiscoverFiles(true);
            EventHandler changed = StateChanged;
            if (changed != null) changed(this, EventArgs.Empty);
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

        internal void ReportUnexpectedError(string operation, Exception exception)
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
            foreach (TailState state in files.Values.ToList()) ReadNewBytes(state, false);
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
            List<string> candidates = new List<string>();
            DateTime today = DateTime.Today;
            for (int daysAgo = 0; daysAgo <= 2; daysAgo++)
            {
                DateTime day = today.AddDays(-daysAgo);
                string dayFolder = Path.Combine(sessionsRoot, day.ToString("yyyy"),
                    day.ToString("MM"), day.ToString("dd"));
                if (!Directory.Exists(dayFolder)) continue;
                try { candidates.AddRange(Directory.GetFiles(dayFolder, "*.jsonl", SearchOption.TopDirectoryOnly)); }
                catch (Exception ex) { ReportUnexpectedError("扫描日期目录", ex); }
            }

            bool runFullDiscovery = initial || now >= nextFullDiscoveryUtc;
            if (runFullDiscovery)
            {
                nextFullDiscoveryUtc = now.AddSeconds(FullDiscoveryIntervalSeconds);
                try
                {
                    candidates.AddRange(Directory.GetFiles(sessionsRoot, "*.jsonl",
                        SearchOption.AllDirectories));
                }
                catch (Exception ex) { ReportUnexpectedError("完整扫描会话目录", ex); }
            }

            List<string> newest = candidates.Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderByDescending(delegate(string path) { return SafeLastWriteTimeUtc(path); })
                .Take(MaximumTrackedFiles).ToList();
            foreach (string path in newest)
            {
                if (files.ContainsKey(path)) continue;
                TailState state = new TailState(path);
                files[path] = state;
                ReadNewBytes(state, initial);
            }
            PruneTrackedFiles();
        }

        private void PruneTrackedFiles()
        {
            if (files.Count <= MaximumTrackedFiles) return;
            HashSet<string> activePaths = new HashSet<string>(activeTurns.Values
                .Select(delegate(ActiveTurn turn) { return turn.SourcePath; }),
                StringComparer.OrdinalIgnoreCase);
            List<TailState> removable = files.Values
                .Where(delegate(TailState state) { return !activePaths.Contains(state.Path); })
                .OrderBy(delegate(TailState state) { return state.LastActivityUtc; }).ToList();
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
            if (lineStart.IndexOf("event_msg", StringComparison.Ordinal) < 0) return false;

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
            if (String.IsNullOrEmpty(turnId)) return true;

            int before = activeTurns.Count;
            if (eventType == "task_started")
            {
                bool added = false;
                ActiveTurn turn;
                if (!activeTurns.TryGetValue(turnId, out turn))
                {
                    turn = new ActiveTurn(turnId, sourcePath, nextStartedSequence++);
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
                    turn = activeTurns.Values.Where(delegate(ActiveTurn item)
                    {
                        return String.Equals(item.SourcePath, sourcePath, StringComparison.OrdinalIgnoreCase);
                    }).OrderByDescending(delegate(ActiveTurn item) { return item.StartSequence; }).FirstOrDefault();
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
                .Where(delegate(ActiveTurn turn) { return String.Equals(turn.SourcePath, path, StringComparison.OrdinalIgnoreCase); })
                .Select(delegate(ActiveTurn turn) { return turn.TurnId; }).ToList();
            if (ids.Count == 0) return;
            foreach (string id in ids) activeTurns.Remove(id);
            EventHandler changed = StateChanged;
            if (changed != null) changed(this, EventArgs.Empty);
        }

        private void SetTitleForFile(string path, string title, DateTime activityUtc)
        {
            ActiveTurn turn = activeTurns.Values
                .Where(delegate(ActiveTurn item)
                {
                    return String.Equals(item.SourcePath, path, StringComparison.OrdinalIgnoreCase);
                })
                .OrderByDescending(delegate(ActiveTurn item) { return item.StartSequence; })
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
                if (String.Equals(turn.SourcePath, path, StringComparison.OrdinalIgnoreCase))
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
            public string Title;
            public TaskPlanProgress Plan;
            public DateTime LastActivityUtc;

            public ActiveTurn(string turnId, string sourcePath, long startSequence)
            {
                TurnId = turnId;
                SourcePath = sourcePath;
                StartSequence = startSequence;
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

    internal static class CompletionVoice
    {
        private const string StartVoiceResource = "voice-start.mp3";
        private const string CompleteVoiceResource = "voice-complete.mp3";
        private const string ErrorVoiceResource = "voice-error.mp3";
        private static readonly Queue<string> voiceQueue = new Queue<string>();
        private static int workerRunning;

        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        private static extern int mciSendString(string command, StringBuilder result,
            int resultLength, IntPtr callbackWindow);

        public static void QueueStart() { Queue(StartVoiceResource); }
        public static void QueueComplete() { Queue(CompleteVoiceResource); }
        public static void QueueError() { Queue(ErrorVoiceResource); }

        private static void Queue(string resourceName)
        {
            lock (voiceQueue) voiceQueue.Enqueue(resourceName);
            if (Interlocked.CompareExchange(ref workerRunning, 1, 0) == 0)
                ThreadPool.QueueUserWorkItem(delegate { Worker(); });
        }

        private static void Worker()
        {
            try
            {
                while (true)
                {
                    string resourceName;
                    lock (voiceQueue)
                    {
                        if (voiceQueue.Count == 0) break;
                        resourceName = voiceQueue.Dequeue();
                    }
                    PlayNow(resourceName);
                }
            }
            finally
            {
                Interlocked.Exchange(ref workerRunning, 0);
                lock (voiceQueue)
                {
                    if (voiceQueue.Count > 0 &&
                        Interlocked.CompareExchange(ref workerRunning, 1, 0) == 0)
                        ThreadPool.QueueUserWorkItem(delegate { Worker(); });
                }
            }
        }


        private static void PlayNow(string resourceName)
        {
            try
            {
                PlayEmbeddedNeuralVoice(resourceName);
            }
            catch
            {
                try { SystemSounds.Exclamation.Play(); }
                catch { }
            }
        }

        private static void PlayEmbeddedNeuralVoice(string resourceName)
        {
            string alias = "codexpets" + Guid.NewGuid().ToString("N");
            string tempFile = Path.Combine(Path.GetTempPath(), alias + ".mp3");
            try
            {
                using (Stream source = Assembly.GetExecutingAssembly()
                    .GetManifestResourceStream(resourceName))
                {
                    if (source == null) throw new InvalidOperationException("内嵌自然女声不存在。");
                    using (FileStream output = new FileStream(tempFile, FileMode.Create,
                        FileAccess.Write, FileShare.Read))
                        source.CopyTo(output);
                }

                // Use the Windows Media Foundation player first. It handles MP3 more
                // reliably than the legacy MCI MPEG device on newer Windows systems.
                try { PlayWithWindowsMediaPlayer(tempFile); }
                catch { PlayWithMci(tempFile, alias); }
            }
            finally
            {
                try { if (File.Exists(tempFile)) File.Delete(tempFile); }
                catch { }
            }
        }

        private static void PlayWithWindowsMediaPlayer(string fileName)
        {
            Exception failure = null;
            using (ManualResetEvent completed = new ManualResetEvent(false))
            {
                Thread playerThread = new Thread(new ThreadStart(delegate
                {
                    MediaPlayer player = null;
                    try
                    {
                        player = new MediaPlayer();
                        player.MediaOpened += delegate
                        {
                            player.Play();
                        };
                        player.MediaEnded += delegate
                        {
                            try { player.Close(); } catch { }
                            completed.Set();
                            Dispatcher.CurrentDispatcher.BeginInvokeShutdown(DispatcherPriority.Normal);
                        };
                        player.MediaFailed += delegate(object sender, MediaExceptionEventArgs args)
                        {
                            failure = args.ErrorException ??
                                new InvalidOperationException("Windows Media 无法播放内嵌语音。");
                            try { player.Close(); } catch { }
                            completed.Set();
                            Dispatcher.CurrentDispatcher.BeginInvokeShutdown(DispatcherPriority.Normal);
                        };
                        player.Open(new Uri(fileName, UriKind.Absolute));
                        Dispatcher.Run();
                    }
                    catch (Exception ex)
                    {
                        failure = ex;
                        completed.Set();
                    }
                    finally
                    {
                        if (player != null) try { player.Close(); } catch { }
                    }
                }));
                playerThread.IsBackground = true;
                playerThread.SetApartmentState(ApartmentState.STA);
                playerThread.Start();
                if (!completed.WaitOne(TimeSpan.FromSeconds(15)))
                    throw new TimeoutException("等待 Windows Media 播放完成超时。");
                playerThread.Join(1000);
            }
            if (failure != null) throw failure;
        }

        private static void PlayWithMci(string fileName, string alias)
        {
            mciSendString("close " + alias, null, 0, IntPtr.Zero);
            int result = mciSendString("open \"" + fileName +
                "\" type mpegvideo alias " + alias, null, 0, IntPtr.Zero);
            if (result != 0) throw new InvalidOperationException("无法打开内嵌自然女声。错误码：" + result);
            try
            {
                result = mciSendString("play " + alias + " wait", null, 0, IntPtr.Zero);
                if (result != 0) throw new InvalidOperationException("无法播放内嵌自然女声。错误码：" + result);
            }
            finally
            {
                mciSendString("close " + alias, null, 0, IntPtr.Zero);
            }
        }

        internal static bool HasEmbeddedVoice()
        {
            return HasResource(StartVoiceResource) &&
                HasResource(CompleteVoiceResource) &&
                HasResource(ErrorVoiceResource);
        }

        private static bool HasResource(string name)
        {
            using (Stream source = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
                return source != null && source.Length > 1000;
        }
    }

    internal static class StartupManager
    {
        private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
        private const string ValueName = "CodeXPetsPortable";
        private static readonly string[] LegacyValueNames =
            { "CodexTaskReminderPortable", "CodexDogPortable" };

        public static void MigrateLegacyEntry()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKeyPath))
                {
                    if (key == null) return;
                    bool foundLegacyEntry = false;
                    foreach (string legacyValueName in LegacyValueNames)
                    {
                        if (key.GetValue(legacyValueName) == null) continue;
                        foundLegacyEntry = true;
                        key.DeleteValue(legacyValueName, false);
                    }
                    if (foundLegacyEntry)
                        key.SetValue(ValueName, "\"" + Application.ExecutablePath + "\"");
                }
            }
            catch { }
        }

        public static bool IsEnabled()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false))
                {
                    if (key == null) return false;
                    string value = key.GetValue(ValueName) as string;
                    string expected = "\"" + Application.ExecutablePath + "\"";
                    return String.Equals(value, expected, StringComparison.OrdinalIgnoreCase);
                }
            }
            catch { return false; }
        }

        public static void SetEnabled(bool enabled)
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKeyPath))
            {
                if (key == null) throw new InvalidOperationException("无法打开当前用户的启动项注册表。");
                if (enabled) key.SetValue(ValueName, "\"" + Application.ExecutablePath + "\"");
                else key.DeleteValue(ValueName, false);
                foreach (string legacyValueName in LegacyValueNames)
                    key.DeleteValue(legacyValueName, false);
            }
        }
    }
}
