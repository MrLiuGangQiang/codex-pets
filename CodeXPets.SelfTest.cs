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
                Check("plan retained across turns", carryMonitor.ActiveCount == 1 &&
                    carryMonitor.ActivePlanProgressLabels.Count == 1 &&
                    String.Equals(carryMonitor.ActivePlanProgressLabels[0], "1/2", StringComparison.Ordinal),
                    ref failures);
            }
            try { Directory.Delete(carryRoot, true); } catch { }

            using (Bitmap idle = StatusIconFactory.CreateBitmap(64, ReminderState.Idle, 0))
            using (Bitmap busy = StatusIconFactory.CreateBitmap(64, ReminderState.Busy, 3))
            using (Bitmap done = StatusIconFactory.CreateBitmap(64, ReminderState.Completed, 0))
            using (Bitmap error = StatusIconFactory.CreateBitmap(64, ReminderState.Error, 0))
                Check("all status icons render", idle.Width == 64 && busy.Width == 64 && done.Width == 64 && error.Width == 64,
                    ref failures);

            Check("all event voices embedded", CompletionVoice.HasEmbeddedVoice(), ref failures);

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

            DateTime expiredDockTime = visibilityStart.AddSeconds(20);
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
            Check("hidden left dock can be found along the full edge",
                leftHover.Contains(101, 60) && leftHover.Contains(101, 840) &&
                !leftHover.Contains(200, 400), ref failures);
            Check("hidden right dock can be found along the full edge",
                rightHover.Contains(1299, 60) && rightHover.Contains(1299, 840) &&
                !rightHover.Contains(1200, 400), ref failures);
            Check("visible dock hover zone stays near the pet",
                localHover.Contains(101, 400) && !localHover.Contains(101, 100), ref failures);

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
