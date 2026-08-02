using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;

namespace CodeXPets
{
    internal static class MonitorSelfTest
    {
        [STAThread]
        public static int Main()
        {
            int failures = 0;
            string root = Path.Combine(Path.GetTempPath(),
                "CodeXPetsSelfTest_" + Guid.NewGuid().ToString("N"));
            string day = Path.Combine(root, DateTime.Today.ToString("yyyy"),
                DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
            Directory.CreateDirectory(day);
            string f = Path.Combine(day, "rollout-test.jsonl");
            File.WriteAllText(f,
                "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"response_item\",\"payload\":{\"text\":\"type task_started fake\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Hello title test\",\"images\":[],\"local_images\":[],\"audio\":[],\"local_audio\":[],\"text_elements\":[]}}\n", Encoding.UTF8);

            CodexSessionMonitor monitor = new CodexSessionMonitor(root);
            int started = 0;
            int completed = 0;
            int aborted = 0;
            monitor.TaskStarted += delegate { started++; };
            monitor.TaskCompleted += delegate { completed++; };
            monitor.TaskAborted += delegate { aborted++; };
            Check("initial active", monitor.ActiveCount == 1, ref failures);
            Check("no startup voice", completed == 0, ref failures);
            Check("title from user_message", String.Equals(monitor.PrimaryActiveTitle, "Hello title test", StringComparison.Ordinal), ref failures);

            File.AppendAllText(f,
                "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"response_item\",\"payload\":{" +
                "\"type\":\"function_call\",\"name\":\"update_plan\"," +
                "\"arguments\":\"{\\\"plan\\\":[{\\\"step\\\":\\\"Inspect\\\",\\\"status\\\":\\\"completed\\\"}," +
                "{\\\"step\\\":\\\"Build feature\\\",\\\"status\\\":\\\"in_progress\\\"}," +
                "{\\\"step\\\":\\\"Test\\\",\\\"status\\\":\\\"pending\\\"}]}\"," +
                "\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"A\"}}}\n", Encoding.UTF8);
            monitor.Poll();
            Check("plan total steps", monitor.TotalPlanStepCount == 3, ref failures);
            Check("plan completed steps", monitor.CompletedPlanStepCount == 1, ref failures);
            Check("plan current step", String.Equals(monitor.PrimaryCurrentPlanStep,
                "Build feature", StringComparison.Ordinal), ref failures);
            Check("plan aligned with its task", monitor.ActivePlanProgressLabels.Count == 1 &&
                String.Equals(monitor.ActivePlanProgressLabels[0], "1/3", StringComparison.Ordinal) &&
                monitor.ActiveTitles.Count == 1 &&
                String.Equals(monitor.ActiveTitles[0], "Hello title test", StringComparison.Ordinal), ref failures);

            File.AppendAllText(f, "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\",\"last_agent_message\":\"ok\"}}\n", Encoding.UTF8);
            monitor.Poll();
            Check("complete clears busy", monitor.ActiveCount == 0, ref failures);
            Check("complete notifies once", completed == 1, ref failures);
            Check("completed title retained", String.Equals(monitor.LastCompletedTitle,
                "Hello title test", StringComparison.Ordinal), ref failures);

            File.AppendAllText(f,
                "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Abort title test\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"B\"}}\n", Encoding.UTF8);
            monitor.Poll();
            Check("abort returns idle", monitor.ActiveCount == 0, ref failures);
            Check("abort stays silent", completed == 1, ref failures);
            Check("abort event once", aborted == 1, ref failures);
            Check("aborted title retained", String.Equals(monitor.LastAbortedTitle,
                "Abort title test", StringComparison.Ordinal), ref failures);
            Check("started event once", started == 1, ref failures);

            File.AppendAllText(f,
                "{\"timestamp\":\"2026-08-01T00:00:05Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call_output\",\"output\":\"\\\"type\\\":\\\"task_started\\\",\\\"turn_id\\\":\\\"FAKE\\\"\"}}\n", Encoding.UTF8);
            monitor.Poll();
            Check("quoted tool output ignored", monitor.ActiveCount == 0, ref failures);

            File.AppendAllText(f,
                "{\"timestamp\":\"2026-08-01T00:00:06Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"C\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:06Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Server failure title\"}}\n" +
                "{\"timestamp\":\"2026-08-01T00:00:07Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"C\",\"last_agent_message\":\"Traceback (most recent call last):\\nRuntimeError: upstream exploded\"}}\n",
                Encoding.UTF8);
            monitor.Poll();
            Check("failed completion clears busy", monitor.ActiveCount == 0, ref failures);
            Check("failed completion is not success", completed == 1, ref failures);
            Check("failed completion raises abnormal event", aborted == 2, ref failures);
            Check("failed completion title retained", String.Equals(monitor.LastAbortedTitle,
                "Server failure title", StringComparison.Ordinal), ref failures);
            Check("failure message classifier catches server errors",
                CodexSessionMonitor.LooksLikeFailureMessage("Internal Server Error"), ref failures);
            Check("failure classifier does not flag successful fix summary",
                !CodexSessionMonitor.LooksLikeFailureMessage("已修复服务端错误并完成全部测试。"), ref failures);

            string carryRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsCarryTest_" + Guid.NewGuid().ToString("N"));
            string carryDay = Path.Combine(carryRoot, DateTime.Today.ToString("yyyy"),
                DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
            Directory.CreateDirectory(carryDay);
            string carryFile = Path.Combine(carryDay, "rollout-carry.jsonl");
            File.WriteAllText(carryFile,
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"OLD\"}}\n" +
                "{\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"name\":\"update_plan\"," +
                "\"arguments\":\"{\\\"plan\\\":[{\\\"step\\\":\\\"One\\\",\\\"status\\\":\\\"completed\\\"}," +
                "{\\\"step\\\":\\\"Two\\\",\\\"status\\\":\\\"in_progress\\\"}]}\"," +
                "\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"OLD\"}}}\n" +
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"OLD\"}}\n" +
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"NEW\"}}\n" +
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Follow-up\"}}\n", Encoding.UTF8);
            using (CodexSessionMonitor carryMonitor = new CodexSessionMonitor(carryRoot))
            {
                bool planRetained = carryMonitor.ActiveCount == 1 &&
                    carryMonitor.ActivePlanProgressLabels.Count == 1 &&
                    String.Equals(carryMonitor.ActivePlanProgressLabels[0], "1/2", StringComparison.Ordinal);
                if (!planRetained) Console.WriteLine(carryMonitor.GetDiagnosticsText());
                Check("plan retained across turns", planRetained, ref failures);
            }
            try { Directory.Delete(carryRoot, true); } catch { }

            string oldRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsOldSessionTest_" + Guid.NewGuid().ToString("N"));
            DateTime oldDayValue = DateTime.Today.AddDays(-10);
            string oldDay = Path.Combine(oldRoot, oldDayValue.ToString("yyyy"),
                oldDayValue.ToString("MM"), oldDayValue.ToString("dd"));
            Directory.CreateDirectory(oldDay);
            string oldFile = Path.Combine(oldDay, "rollout-old-active.jsonl");
            File.WriteAllText(oldFile,
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"OLD-DAY\"}}\n",
                Encoding.UTF8);
            File.SetLastWriteTimeUtc(oldFile, DateTime.UtcNow);
            using (CodexSessionMonitor oldMonitor = new CodexSessionMonitor(oldRoot))
                Check("recently modified long-running session is discovered outside three-day folders",
                    oldMonitor.ActiveCount == 1, ref failures);
            string switchRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsSwitchRootTest_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(switchRoot);
            using (CodexSessionMonitor switchMonitor = new CodexSessionMonitor(switchRoot))
            {
                switchMonitor.SetSessionsRoot(oldRoot);
                Check("session root can be changed at runtime",
                    switchMonitor.ActiveCount == 1 &&
                    switchMonitor.GetDiagnosticsText().IndexOf(oldRoot,
                        StringComparison.OrdinalIgnoreCase) >= 0, ref failures);
            }
            try { Directory.Delete(switchRoot, true); } catch { }
            try { Directory.Delete(oldRoot, true); } catch { }

            string utfRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsUtf8BoundaryTest_" + Guid.NewGuid().ToString("N"));
            string utfDay = Path.Combine(utfRoot, DateTime.Today.ToString("yyyy"),
                DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
            Directory.CreateDirectory(utfDay);
            string utfFile = Path.Combine(utfDay, "rollout-utf8.jsonl");
            string utfStart =
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"UTF8\"}}\n";
            string utfMessagePrefix =
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"";
            int bytesBeforeBoundary = Encoding.UTF8.GetByteCount(utfStart + utfMessagePrefix);
            int asciiPadding = 65535 - (bytesBeforeBoundary % 65536);
            string utfContent = utfStart + utfMessagePrefix + new string('a', asciiPadding) +
                "猫跨块\"}}\n";
            File.WriteAllBytes(utfFile, new UTF8Encoding(false).GetBytes(utfContent));
            using (CodexSessionMonitor utfMonitor = new CodexSessionMonitor(utfRoot))
            {
                Check("UTF-8 character split across read buffers is preserved",
                    utfMonitor.ActiveCount == 1 &&
                    !String.IsNullOrEmpty(utfMonitor.PrimaryActiveTitle) &&
                    utfMonitor.PrimaryActiveTitle.EndsWith("猫跨块", StringComparison.Ordinal),
                    ref failures);
                Check("partial JSON while tailing is not reported as a parse error",
                    utfMonitor.GetDiagnosticsText().IndexOf("JSON 解析错误：0",
                        StringComparison.Ordinal) >= 0, ref failures);
            }
            try { Directory.Delete(utfRoot, true); } catch { }

            string startupRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsStartupActiveTest_" + Guid.NewGuid().ToString("N"));
            string startupDay = Path.Combine(startupRoot, DateTime.Today.ToString("yyyy"),
                DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
            Directory.CreateDirectory(startupDay);
            string startupFile = Path.Combine(startupDay, "rollout-startup-active.jsonl");
            string recentTimestamp = DateTime.UtcNow.AddSeconds(-2).ToString("o");
            File.WriteAllText(startupFile,
                "{\"timestamp\":\"" + recentTimestamp + "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"STARTUP\"}}\n" +
                "{\"timestamp\":\"" + recentTimestamp + "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Startup title\"}}\n",
                Encoding.UTF8);
            File.SetLastWriteTimeUtc(startupFile, DateTime.UtcNow.AddHours(-1));
            using (CodexSessionMonitor startupMonitor = new CodexSessionMonitor(startupRoot))
            {
                startupMonitor.Poll();
                Check("startup scan uses recent JSON event time when file time is frozen",
                    startupMonitor.ActiveCount == 1 &&
                    String.Equals(startupMonitor.PrimaryActiveTitle, "Startup title",
                        StringComparison.Ordinal), ref failures);
            }
            try { Directory.Delete(startupRoot, true); } catch { }

            string liveRoot = Path.Combine(Path.GetTempPath(),
                "CodeXPetsLiveAppendTest_" + Guid.NewGuid().ToString("N"));
            string liveDay = Path.Combine(liveRoot, DateTime.Today.ToString("yyyy"),
                DateTime.Today.ToString("MM"), DateTime.Today.ToString("dd"));
            Directory.CreateDirectory(liveDay);
            string liveFile = Path.Combine(liveDay, "rollout-live-append.jsonl");
            File.WriteAllText(liveFile, "{}\n", Encoding.UTF8);
            DateTime frozenWriteUtc = DateTime.UtcNow.AddHours(-1);
            File.SetLastWriteTimeUtc(liveFile, frozenWriteUtc);
            using (CodexSessionMonitor liveMonitor = new CodexSessionMonitor(liveRoot))
            {
                File.AppendAllText(liveFile,
                    "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"LIVE\"}}\n" +
                    "{\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Live title\"}}\n",
                    Encoding.UTF8);
                // Reproduce Codex holding an open JSONL whose LastWriteTime does not
                // advance even though its length and contents do.
                File.SetLastWriteTimeUtc(liveFile, frozenWriteUtc);
                liveMonitor.Poll();
                Check("live append remains active when LastWriteTime is frozen",
                    liveMonitor.ActiveCount == 1 &&
                    String.Equals(liveMonitor.PrimaryActiveTitle, "Live title",
                        StringComparison.Ordinal), ref failures);
            }
            try { Directory.Delete(liveRoot, true); } catch { }

            DateTime staleNow = new DateTime(2026, 8, 2, 12, 0, 0, DateTimeKind.Utc);
            Check("inactive turn is stale only when both turn and file are old",
                CodexSessionMonitor.IsTurnStale(staleNow.AddMinutes(-11),
                    staleNow.AddMinutes(-11), staleNow, 600) &&
                !CodexSessionMonitor.IsTurnStale(staleNow.AddMinutes(-11),
                    staleNow.AddMinutes(-1), staleNow, 600), ref failures);

            CodeXPetsSettings defaultSettings = CodeXPetsSettings.CreateDefault();
            Check("default edge trigger height is 240 pixels",
                defaultSettings.DockHoverHeight == 240, ref failures);
            CodeXPetsSettings normalizedSettings = CodeXPetsSettings.CreateDefault();
            normalizedSettings.DockHoverHeight = 5;
            normalizedSettings.DockIdleHideSeconds = -1;
            normalizedSettings.DockRevealSeconds = 0;
            normalizedSettings.Normalize();
            Check("settings values are normalized safely",
                normalizedSettings.DockHoverHeight == 40 &&
                normalizedSettings.DockIdleHideSeconds == 0 &&
                normalizedSettings.DockRevealSeconds == 1, ref failures);

            using (Bitmap idle = StatusIconFactory.CreateBitmap(64, ReminderState.Idle, 0))
            using (Bitmap busy = StatusIconFactory.CreateBitmap(64, ReminderState.Busy, 3))
            using (Bitmap done = StatusIconFactory.CreateBitmap(64, ReminderState.Completed, 0))
            using (Bitmap error = StatusIconFactory.CreateBitmap(64, ReminderState.Error, 0))
                Check("all status icons render", idle.Width == 64 && busy.Width == 64 && done.Width == 64 && error.Width == 64,
                    ref failures);

            Check("all event voices embedded", CompletionVoice.HasEmbeddedVoice(), ref failures);
            Check("release version metadata available", String.Equals(AppInfo.Version, "2.2.1", StringComparison.Ordinal), ref failures);

            PetPositionState savedDockPosition = new PetPositionState(DockEdge.Left,
                @"\\.\DISPLAY2", 0.5D, 0.375D);
            string serializedPosition = PetPositionSettings.Serialize(savedDockPosition);
            PetPositionState restoredDockPosition = PetPositionSettings.Deserialize(serializedPosition);
            Check("docked position serializes and restores", restoredDockPosition != null &&
                restoredDockPosition.DockEdge == DockEdge.Left &&
                String.Equals(restoredDockPosition.ScreenDeviceName, @"\\.\DISPLAY2",
                    StringComparison.Ordinal) &&
                Math.Abs(restoredDockPosition.RelativeY - 0.375D) < 0.000001D,
                ref failures);
            PetPositionState restoredFloatingPosition = PetPositionSettings.Deserialize(
                PetPositionSettings.Serialize(new PetPositionState(DockEdge.None,
                    @"\\.\DISPLAY1", 0.72D, 0.84D)));
            Check("floating position serializes and restores", restoredFloatingPosition != null &&
                restoredFloatingPosition.DockEdge == DockEdge.None &&
                Math.Abs(restoredFloatingPosition.RelativeX - 0.72D) < 0.000001D &&
                Math.Abs(restoredFloatingPosition.RelativeY - 0.84D) < 0.000001D,
                ref failures);
            Check("saved position clamps invalid ratios",
                Math.Abs(PetPositionSettings.Clamp01(-2D)) < 0.000001D &&
                Math.Abs(PetPositionSettings.Clamp01(3D) - 1D) < 0.000001D &&
                Math.Abs(PetPositionSettings.Clamp01(Double.NaN) - 0.5D) < 0.000001D,
                ref failures);
            Check("invalid saved position is ignored",
                PetPositionSettings.Deserialize("broken-position") == null, ref failures);

            using (Stream catStream = typeof(MonitorSelfTest).Assembly.GetManifestResourceStream(
                "white-cat-spritesheet.png"))
            using (Stream dockStream = typeof(MonitorSelfTest).Assembly.GetManifestResourceStream(
                "cat-dock-spritesheet.png"))
            using (Stream removedOtterStream = typeof(MonitorSelfTest).Assembly.GetManifestResourceStream(
                "boba-spritesheet.png"))
            using (Bitmap catSheet = catStream == null ? null : new Bitmap(catStream))
            using (Bitmap dockSheet = dockStream == null ? null : new Bitmap(dockStream))
            {
                Check("compact white cat sprite embedded", catSheet != null &&
                    catSheet.Width == 1536 && catSheet.Height == 832, ref failures);
                Check("dock expression sprite embedded", dockSheet != null && dockSheet.Width == 2048 &&
                    dockSheet.Height == 256, ref failures);
                Check("otter sprite removed", removedOtterStream == null, ref failures);
                if (dockSheet != null)
                {
                    bool leftExpressionsReady = true;
                    bool rightExpressionsReady = true;
                    Rectangle leftReferenceBounds = Rectangle.Empty;
                    Rectangle rightReferenceBounds = Rectangle.Empty;
                    bool expressionPoseStable = true;
                    for (int expression = 0; expression < 4; expression++)
                    {
                        Rectangle leftDock = FindOpaqueBounds(dockSheet,
                            new Rectangle(expression * 256, 0, 256, 256));
                        Rectangle rightDock = FindOpaqueBounds(dockSheet,
                            new Rectangle((4 + expression) * 256, 0, 256, 256));
                        leftExpressionsReady = leftExpressionsReady && !leftDock.IsEmpty &&
                            leftDock.Left == expression * 256 && leftDock.Height > leftDock.Width;
                        rightExpressionsReady = rightExpressionsReady && !rightDock.IsEmpty &&
                            rightDock.Right == (5 + expression) * 256 && rightDock.Height > rightDock.Width;
                        if (expression == 0)
                        {
                            leftReferenceBounds = leftDock;
                            rightReferenceBounds = rightDock;
                        }
                        else
                        {
                            expressionPoseStable = expressionPoseStable &&
                                leftDock.Equals(new Rectangle(expression * 256,
                                    leftReferenceBounds.Y, leftReferenceBounds.Width,
                                    leftReferenceBounds.Height)) &&
                                rightDock.Equals(new Rectangle((4 + expression) * 256 +
                                    (rightReferenceBounds.X - 4 * 256), rightReferenceBounds.Y,
                                    rightReferenceBounds.Width, rightReferenceBounds.Height));
                        }
                    }
                    bool pawsStayFixed = true;
                    for (int expression = 1; expression < 4; expression++)
                    {
                        pawsStayFixed = pawsStayFixed && RegionsEqual(dockSheet,
                            new Rectangle(0, 147, 40, 109),
                            new Rectangle(expression * 256, 147, 40, 109)) &&
                            RegionsEqual(dockSheet,
                            new Rectangle(4 * 256 + 216, 147, 40, 109),
                            new Rectangle((4 + expression) * 256 + 216, 147, 40, 109));
                    }
                    Check("dock expressions keep one fixed pose", expressionPoseStable,
                        ref failures);
                    Check("dock expressions never modify gripping paws", pawsStayFixed,
                        ref failures);
                    Check("all left dock expressions grip screen edge", leftExpressionsReady,
                        ref failures);
                    Check("all right dock expressions grip screen edge", rightExpressionsReady,
                        ref failures);
                }
                Rectangle seatedBounds = catSheet == null ? Rectangle.Empty :
                    FindOpaqueBounds(catSheet, new Rectangle(0, 0, 192, 208));
                Rectangle catWalkingBounds = catSheet == null ? Rectangle.Empty :
                    FindOpaqueBounds(catSheet, new Rectangle(0, 2 * 208, 192, 208));
                Rectangle catThinkingBounds = catSheet == null ? Rectangle.Empty :
                    FindOpaqueBounds(catSheet, new Rectangle(5 * 192, 2 * 208, 192, 208));
                Check("cat idle uses seated silhouette", seatedBounds.Height > seatedBounds.Width * 1.05F,
                    ref failures);
                Check("cat busy uses walking silhouette", catWalkingBounds.Width >
                    catWalkingBounds.Height * 1.15F, ref failures);
                Check("cat busy includes thinking pose", catThinkingBounds.Height >
                    catThinkingBounds.Width * 1.05F, ref failures);

                bool allBusyFramesPresent = catSheet != null;
                for (int frame = 0; frame < 8 && allBusyFramesPresent; frame++)
                {
                    Rectangle cell = new Rectangle(frame * 192, 2 * 208, 192, 208);
                    allBusyFramesPresent = !FindOpaqueBounds(catSheet, cell).IsEmpty;
                }
                Check("all cat work-cycle frames embedded", allBusyFramesPresent, ref failures);

                bool catWalkAmplitudeBalanced = catSheet != null;
                double minFrontX = Double.MaxValue;
                double maxFrontX = Double.MinValue;
                double minHindX = Double.MaxValue;
                double maxHindX = Double.MinValue;
                for (int frame = 0; frame < 4 && catWalkAmplitudeBalanced; frame++)
                {
                    int cellX = frame * 192;
                    double frontX = FindAlphaCentroidX(catSheet, new Rectangle(
                        cellX + 55, 2 * 208 + 108, 55, 24));
                    double hindX = FindAlphaCentroidX(catSheet, new Rectangle(
                        cellX + 110, 2 * 208 + 108, 60, 24));
                    catWalkAmplitudeBalanced = !Double.IsNaN(frontX) && !Double.IsNaN(hindX);
                    minFrontX = Math.Min(minFrontX, frontX);
                    maxFrontX = Math.Max(maxFrontX, frontX);
                    minHindX = Math.Min(minHindX, hindX);
                    maxHindX = Math.Max(maxHindX, hindX);
                }
                double frontTravel = maxFrontX - minFrontX;
                double hindTravel = maxHindX - minHindX;
                catWalkAmplitudeBalanced = catWalkAmplitudeBalanced && frontTravel >= 1.5D &&
                    hindTravel >= 1.5D && frontTravel <= hindTravel * 1.5D &&
                    hindTravel <= frontTravel * 1.5D;
                Check("cat walking front and hind travel stay balanced",
                    catWalkAmplitudeBalanced, ref failures);
            }

            Check("latest task change controls the displayed state",
                ReminderApplicationContext.SelectVisualState(1, true, false, ReminderState.Error) == ReminderState.Error &&
                ReminderApplicationContext.SelectVisualState(1, false, true, ReminderState.Completed) == ReminderState.Completed &&
                ReminderApplicationContext.SelectVisualState(1, false, false, ReminderState.Busy) == ReminderState.Busy &&
                ReminderApplicationContext.SelectVisualState(1, false, false, ReminderState.Idle) == ReminderState.Busy &&
                ReminderApplicationContext.SelectVisualState(0, false, false, ReminderState.Idle) == ReminderState.Idle,
                ref failures);
            Check("abnormal task text includes the failed task title",
                String.Equals(ReminderApplicationContext.FormatAbnormalTaskText("构建安装包"),
                    "任务失败：构建安装包", StringComparison.Ordinal) &&
                String.Equals(ReminderApplicationContext.FormatAbnormalTaskText(null),
                    "任务失败：未知任务", StringComparison.Ordinal), ref failures);
            Check("animation redraw waits for an actual visual change",
                DesktopAssistantForm.ShouldRenderAnimation(true, false, false, false) == false &&
                DesktopAssistantForm.ShouldRenderAnimation(false, true, false, false) &&
                DesktopAssistantForm.ShouldRenderAnimation(true, false, true, false) &&
                DesktopAssistantForm.ShouldRenderAnimation(false, false, false, true), ref failures);
            Check("expired latest notification falls back to remaining active work",
                ReminderApplicationContext.SelectVisualState(1, false, false, ReminderState.Error) == ReminderState.Busy &&
                ReminderApplicationContext.SelectVisualState(1, false, false, ReminderState.Completed) == ReminderState.Busy,
                ref failures);

            Check("busy state keeps its light bulb",
                DesktopAssistantForm.ShouldShowLightBulb(ReminderState.Busy) &&
                DesktopAssistantForm.ShouldShowLightBulb(ReminderState.Completed) &&
                DesktopAssistantForm.ShouldShowLightBulb(ReminderState.Error) &&
                !DesktopAssistantForm.ShouldShowLightBulb(ReminderState.Idle), ref failures);
            Check("error notifications stay visible for at least ten seconds",
                DesktopAssistantForm.GetCloudNotificationSeconds(ReminderState.Error, 1) == 10 &&
                DesktopAssistantForm.GetCloudNotificationSeconds(ReminderState.Busy, 5) == 5 &&
                DesktopAssistantForm.GetCloudNotificationSeconds(ReminderState.Error, 15) == 15,
                ref failures);

            Rectangle cloudSafetyTest = new Rectangle(10, 20, 300, 150);
            RectangleF bulbContentBounds = DesktopAssistantForm.CalculateCloudContentBounds(
                cloudSafetyTest, true, 190, 80);
            RectangleF plainContentBounds = DesktopAssistantForm.CalculateCloudContentBounds(
                cloudSafetyTest, false, 220, 80);
            Check("title viewport stays inside cloud safe area",
                bulbContentBounds.Left >= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.30F &&
                bulbContentBounds.Right <= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.80F + 0.01F &&
                bulbContentBounds.Top >= cloudSafetyTest.Top + cloudSafetyTest.Height * 0.31F &&
                bulbContentBounds.Bottom <= cloudSafetyTest.Top + cloudSafetyTest.Height * 0.76F + 0.01F &&
                plainContentBounds.Left >= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.22F &&
                plainContentBounds.Right <= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.80F + 0.01F,
                ref failures);
            RectangleF centeredHeaderBounds = DesktopAssistantForm.CalculateCloudHeaderBounds(cloudSafetyTest);
            Check("status header uses one centered safe region",
                Math.Abs((centeredHeaderBounds.Left + centeredHeaderBounds.Width * 0.5F) -
                    (cloudSafetyTest.Left + cloudSafetyTest.Width * 0.5F)) < 0.01F &&
                centeredHeaderBounds.Left >= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.20F &&
                centeredHeaderBounds.Right <= cloudSafetyTest.Left + cloudSafetyTest.Width * 0.80F + 0.01F,
                ref failures);

            Check("busy header combines status and counters without a step label",
                String.Equals(DesktopAssistantForm.FormatBusyHeader("1/2", 0, 2),
                    "进行中(1/2)·会话(1/2)", StringComparison.Ordinal), ref failures);
            Check("busy metadata distinguishes step progress from session position",
                String.Equals(DesktopAssistantForm.FormatBusyMetadata("1/2", 0, 2),
                    "(1/2)·会话(1/2)", StringComparison.Ordinal), ref failures);
            Check("busy metadata omits unnecessary single-session counter",
                String.Equals(DesktopAssistantForm.FormatBusyMetadata("1/3", 0, 1),
                    "(1/3)", StringComparison.Ordinal), ref failures);
            Check("busy metadata labels session counter even without a plan",
                String.Equals(DesktopAssistantForm.FormatBusyMetadata(null, 1, 2),
                    "会话(2/2)", StringComparison.Ordinal), ref failures);

            Rectangle snapArea = new Rectangle(100, 50, 1200, 800);
            Check("floating cat faces inward from the left side",
                DesktopAssistantForm.ShouldMirrorFloatingSprite(
                    new Point(300, 400), snapArea), ref failures);
            Check("floating cat faces inward from the right side",
                !DesktopAssistantForm.ShouldMirrorFloatingSprite(
                    new Point(1100, 400), snapArea), ref failures);
            Check("floating cat direction changes at screen midpoint",
                !DesktopAssistantForm.ShouldMirrorFloatingSprite(
                    new Point(700, 400), snapArea), ref failures);

            Check("snap detects left edge", DesktopAssistantForm.SelectSnapEdge(
                new Point(112, 400), snapArea, 24) == DockEdge.Left, ref failures);
            Check("snap detects right edge", DesktopAssistantForm.SelectSnapEdge(
                new Point(1285, 400), snapArea, 24) == DockEdge.Right, ref failures);
            Check("top edge no longer snaps", DesktopAssistantForm.SelectSnapEdge(
                new Point(700, 60), snapArea, 24) == DockEdge.None, ref failures);
            Check("snap ignores distant point", DesktopAssistantForm.SelectSnapEdge(
                new Point(700, 400), snapArea, 24) == DockEdge.None, ref failures);

            DateTime visibilityStart = new DateTime(2026, 8, 2, 0, 0, 0, DateTimeKind.Utc);
            Check("dock remains visible before ten seconds", DesktopAssistantForm.ShouldKeepDockVisible(
                visibilityStart, visibilityStart.AddSeconds(9.9)), ref failures);
            Check("dock hides after ten seconds", !DesktopAssistantForm.ShouldKeepDockVisible(
                visibilityStart, visibilityStart.AddSeconds(10.1)), ref failures);
            Check("zero auto-hide timeout keeps dock visible",
                DesktopAssistantForm.ShouldKeepDockVisible(visibilityStart,
                    visibilityStart.AddHours(1), 0), ref failures);

            DateTime expiredDockTime = visibilityStart.AddSeconds(20);
            Check("floating cloud remains visible while idle",
                DesktopAssistantForm.ShouldShowThoughtBubble(false, ReminderState.Idle,
                    expiredDockTime, DateTime.MinValue), ref failures);
            Check("docked idle cloud remains hidden",
                !DesktopAssistantForm.ShouldShowThoughtBubble(true, ReminderState.Idle,
                    expiredDockTime, expiredDockTime.AddSeconds(5)), ref failures);
            Check("docked task cloud respects its notification timeout",
                DesktopAssistantForm.ShouldShowThoughtBubble(true, ReminderState.Busy,
                    expiredDockTime, expiredDockTime.AddSeconds(5)) &&
                !DesktopAssistantForm.ShouldShowThoughtBubble(true, ReminderState.Busy,
                    expiredDockTime.AddSeconds(6), expiredDockTime.AddSeconds(5)), ref failures);

            Check("dragging keeps expired dock visible", DesktopAssistantForm.ShouldShowDock(
                visibilityStart, expiredDockTime, true, false, DateTime.MinValue), ref failures);
            Check("hover keeps expired dock visible", DesktopAssistantForm.ShouldShowDock(
                visibilityStart, expiredDockTime, false, true, DateTime.MinValue), ref failures);
            Check("expired dock hides without interaction", !DesktopAssistantForm.ShouldShowDock(
                visibilityStart, expiredDockTime, false, false, DateTime.MinValue), ref failures);
            Check("recent hover remains visible after cursor leaves", DesktopAssistantForm.ShouldShowDock(
                visibilityStart, expiredDockTime, false, false,
                expiredDockTime.AddSeconds(2)), ref failures);

            Rectangle leftHover = DesktopAssistantForm.GetDockHoverBounds(
                DockEdge.Left, snapArea, 400, 1F, true);
            Rectangle rightHover = DesktopAssistantForm.GetDockHoverBounds(
                DockEdge.Right, snapArea, 400, 1F, true);
            Rectangle localHover = DesktopAssistantForm.GetDockHoverBounds(
                DockEdge.Left, snapArea, 400, 1F, false);
            Check("hidden left dock uses a fixed-height local edge hotspot",
                leftHover.Contains(101, 400) && !leftHover.Contains(101, 250) &&
                !leftHover.Contains(200, 400), ref failures);
            Check("hidden right dock uses a fixed-height local edge hotspot",
                rightHover.Contains(1299, 400) && !rightHover.Contains(1299, 550) &&
                !rightHover.Contains(1200, 400), ref failures);
            Check("visible dock hover zone stays near the pet",
                localHover.Contains(101, 400) && !localHover.Contains(101, 250), ref failures);
            Rectangle customHover = DesktopAssistantForm.GetDockHoverBounds(
                DockEdge.Left, snapArea, 400, 1F, true, 240);
            Check("configured dock hover height is respected",
                customHover.Height == 240 && customHover.Contains(101, 510) &&
                !customHover.Contains(101, 530), ref failures);

            Check("dock expression performs quick double blink",
                DesktopAssistantForm.SelectDockExpression(ReminderState.Busy, 11) == 1 &&
                DesktopAssistantForm.SelectDockExpression(ReminderState.Busy, 12) == 0 &&
                DesktopAssistantForm.SelectDockExpression(ReminderState.Busy, 14) == 1,
                ref failures);
            Check("dock completion and error expressions selected",
                DesktopAssistantForm.SelectDockExpression(ReminderState.Completed, 0) == 2 &&
                DesktopAssistantForm.SelectDockExpression(ReminderState.Error, 0) == 3,
                ref failures);

            bool assistantLoads = true;
            try
            {
                using (System.Windows.Forms.ContextMenuStrip testMenu =
                    new System.Windows.Forms.ContextMenuStrip())
                using (DesktopAssistantForm testForm = new DesktopAssistantForm(testMenu, false))
                    assistantLoads = !testForm.IsDocked;
            }
            catch (Exception ex)
            {
                Console.WriteLine("Assistant integration error: " + ex.Message);
                assistantLoads = false;
            }
            Check("fixed cat assistant loads", assistantLoads, ref failures);

            bool utilityDialogsLoad = true;
            try
            {
                CodeXPetsSettings testSettings = CodeXPetsSettings.CreateDefault();
                using (CodeXPetsSettingsForm settingsForm = new CodeXPetsSettingsForm(testSettings))
                    utilityDialogsLoad = settingsForm.Result != null;
                using (CodeXPetsDiagnosticsForm diagnosticsForm =
                    new CodeXPetsDiagnosticsForm(monitor, testSettings))
                    utilityDialogsLoad = utilityDialogsLoad &&
                        diagnosticsForm.Text.IndexOf("2.2.1", StringComparison.Ordinal) >= 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine("Utility dialog integration error: " + ex.Message);
                utilityDialogsLoad = false;
            }
            Check("settings and diagnostics dialogs load", utilityDialogsLoad, ref failures);

            monitor.Dispose();

            try { Directory.Delete(root, true); } catch { }

            Stopwatch sw = Stopwatch.StartNew();
            using (CodexSessionMonitor real = new CodexSessionMonitor())
            {
                sw.Stop();
                Console.WriteLine("Real sessions: active={0}, startup={1} ms", real.ActiveCount,
                    sw.ElapsedMilliseconds);
            }
            Console.WriteLine(failures == 0 ? "ALL TESTS PASSED" :
                (failures + " TEST(S) FAILED"));
            return failures == 0 ? 0 : 1;
        }


        private static bool RegionsEqual(Bitmap bitmap, Rectangle first, Rectangle second)
        {
            if (bitmap == null || first.Size != second.Size) return false;
            for (int y = 0; y < first.Height; y++)
                for (int x = 0; x < first.Width; x++)
                    if (bitmap.GetPixel(first.X + x, first.Y + y).ToArgb() !=
                        bitmap.GetPixel(second.X + x, second.Y + y).ToArgb()) return false;
            return true;
        }

        private static Rectangle FindOpaqueBounds(Bitmap bitmap, Rectangle area)
        {
            int minX = area.Right;
            int minY = area.Bottom;
            int maxX = area.Left - 1;
            int maxY = area.Top - 1;
            for (int y = area.Top; y < area.Bottom; y++)
                for (int x = area.Left; x < area.Right; x++)
                    if (bitmap.GetPixel(x, y).A > 16)
                    {
                        minX = Math.Min(minX, x);
                        minY = Math.Min(minY, y);
                        maxX = Math.Max(maxX, x);
                        maxY = Math.Max(maxY, y);
                    }
            return maxX < minX ? Rectangle.Empty :
                Rectangle.FromLTRB(minX, minY, maxX + 1, maxY + 1);
        }

        private static double FindAlphaCentroidX(Bitmap bitmap, Rectangle area)
        {
            double weightedX = 0D;
            double totalAlpha = 0D;
            for (int y = area.Top; y < area.Bottom; y++)
                for (int x = area.Left; x < area.Right; x++)
                {
                    int alpha = bitmap.GetPixel(x, y).A;
                    if (alpha <= 16) continue;
                    weightedX += x * alpha;
                    totalAlpha += alpha;
                }
            return totalAlpha <= 0D ? Double.NaN : weightedX / totalAlpha;
        }

        private static void Check(string name, bool ok, ref int failures)
        {
            Console.WriteLine("{0}: {1}", ok ? "PASS" : "FAIL", name);
            if (!ok) failures++;
        }
    }
}
