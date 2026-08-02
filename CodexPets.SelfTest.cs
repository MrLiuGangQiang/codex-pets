using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;

namespace CodexPets
{
    internal static class MonitorSelfTest
    {
        public static int Main()
        {
            int failures = 0;
            string root = Path.Combine(Path.GetTempPath(),
                "CodexPetsSelfTest_" + Guid.NewGuid().ToString("N"));
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

            Check("embedded official Codex icon", StatusIconFactory.HasEmbeddedLogo(), ref failures);
            using (Bitmap idle = StatusIconFactory.CreateBitmap(64, ReminderState.Idle, 0))
            using (Bitmap busy = StatusIconFactory.CreateBitmap(64, ReminderState.Busy, 3))
            using (Bitmap done = StatusIconFactory.CreateBitmap(64, ReminderState.Completed, 0))
            using (Bitmap error = StatusIconFactory.CreateBitmap(64, ReminderState.Error, 0))
                Check("all status icons render", idle.Width == 64 && busy.Width == 64 && done.Width == 64 && error.Width == 64,
                    ref failures);

            Check("all event voices embedded", CompletionVoice.HasEmbeddedVoice(), ref failures);

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

        private static void Check(string name, bool ok, ref int failures)
        {
            Console.WriteLine("{0}: {1}", ok ? "PASS" : "FAIL", name);
            if (!ok) failures++;
        }
    }
}
