#include "app_logic.h"
#include "json.h"
#include "paths.h"
#include "platform_text.h"
#include "presentation.h"
#include "render_layout.h"
#include "session_monitor.h"
#include "settings.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace codexpets;

namespace {

struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };

#define CHECK(value) do { if (!(value)) throw TestFailure(std::string("CHECK failed: ") + #value + " at line " + std::to_string(__LINE__)); } while (false)
#define CHECK_EQ(expected, actual) do { const auto _e = (expected); const auto _a = (actual); if (!(_e == _a)) throw TestFailure(std::string("CHECK_EQ failed at line ") + std::to_string(__LINE__)); } while (false)
#define CHECK_NEAR(expected, actual) do { const double _e = static_cast<double>(expected); const double _a = static_cast<double>(actual); if (std::abs(_e - _a) > 1e-6) throw TestFailure(std::string("CHECK_NEAR failed at line ") + std::to_string(__LINE__)); } while (false)

static_assert(monitor_pending_event_limit == std::size_t{64});

std::filesystem::path unique_temp(std::string_view prefix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    return std::filesystem::temp_directory_path() /
        (std::string(prefix) + std::to_string(pid) + "_" + std::to_string(stamp));
}

struct TempDirectory {
    std::filesystem::path path;
    explicit TempDirectory(std::string_view prefix) : path(unique_temp(prefix)) {
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::filesystem::path today_folder(const std::filesystem::path& root) {
    const auto raw = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    char year[5]{}, month[3]{}, day[3]{};
    std::strftime(year, sizeof(year), "%Y", &local);
    std::strftime(month, sizeof(month), "%m", &local);
    std::strftime(day, sizeof(day), "%d", &local);
    return root / year / month / day;
}

std::filesystem::path create_session(const std::filesystem::path& root,
                                     std::string_view name, std::string_view content) {
    const auto folder = today_folder(root);
    std::filesystem::create_directories(folder);
    const auto file = folder / path_from_utf8(name);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file;
}

void append(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
}

void set_environment(std::string_view name, const std::optional<std::string>& value) {
#ifdef _WIN32
    const auto key = utf8_to_wide(name);
    if (value) {
        const auto text = utf8_to_wide(*value);
        SetEnvironmentVariableW(key.c_str(), text.c_str());
    } else SetEnvironmentVariableW(key.c_str(), nullptr);
#else
    const std::string key(name);
    if (value) setenv(key.c_str(), value->c_str(), 1);
    else unsetenv(key.c_str());
#endif
}

void test_app_logic() {
    CHECK_EQ(ReminderState::Error,
             app_logic::select_visual_state(1, true, false, ReminderState::Error));
    CHECK_EQ(ReminderState::Completed,
             app_logic::select_visual_state(1, false, true, ReminderState::Completed));
    CHECK_EQ(ReminderState::Busy,
             app_logic::select_visual_state(1, false, false, ReminderState::Completed));

    const RectD work{0, 0, 1920, 1080};
    CHECK_EQ(DockEdge::Left, app_logic::select_snap_edge({8, 400}, work, 36));
    CHECK_EQ(DockEdge::Right, app_logic::select_snap_edge({1900, 400}, work, 36));
    CHECK_EQ(DockEdge::None, app_logic::select_snap_edge({900, 10}, work, 36));
    CHECK(app_logic::should_mirror_floating_sprite({300, 500}, work));
    CHECK(!app_logic::should_mirror_floating_sprite({1500, 500}, work));
    CHECK_EQ(10, app_logic::cloud_notification_seconds(ReminderState::Error, 5));
    CHECK_EQ(5, app_logic::cloud_notification_seconds(ReminderState::Completed, 5));

    CHECK_EQ(std::string("进行中(1/3) • 2/4"),
             app_logic::format_busy_header(std::string_view("1/3"), 1, 4));
    CHECK_EQ(std::string("进行中 • 2/4"), app_logic::format_busy_header(std::nullopt, 1, 4));
    CHECK_EQ(std::string("进行中(1/3)"),
             app_logic::format_busy_header(std::string_view("1/3"), 0, 1));

    const std::vector<std::string> titles{"任务 A", "任务 B", "任务 C"};
    CHECK_EQ(1, app_logic::reconcile_task_selection(ReminderState::Busy, titles, 1, "任务 B", false, -1));
    const std::vector<std::string> reordered{"任务 B", "任务 C", "任务 D"};
    CHECK_EQ(0, app_logic::reconcile_task_selection(ReminderState::Busy, reordered, 1, "任务 B", false, -1));
    CHECK_EQ(2, app_logic::reconcile_task_selection(ReminderState::Busy, titles, 0, "任务 A", true, -1));
    CHECK_EQ(1, app_logic::reconcile_task_selection(ReminderState::Busy, titles, 0, "任务 A", true, 1));
    CHECK_EQ(0, app_logic::reconcile_task_selection(ReminderState::Completed, titles, 2, "任务 C", false, -1));

    CHECK_EQ(0, app_logic::select_floating_sprite_row(ReminderState::Idle));
    CHECK_EQ(1, app_logic::select_floating_sprite_row(ReminderState::Completed));
    CHECK_EQ(2, app_logic::select_floating_sprite_row(ReminderState::Busy));
    CHECK_EQ(3, app_logic::select_floating_sprite_row(ReminderState::Error));
    CHECK_EQ(4, app_logic::select_floating_sprite_row(ReminderState::Interrupted));
    CHECK_EQ(0, app_logic::select_floating_frame(ReminderState::Interrupted, 0));
    CHECK_EQ(1, app_logic::select_floating_frame(ReminderState::Interrupted, 11));
    CHECK_EQ(0, app_logic::select_floating_frame(ReminderState::Interrupted, 12));
    CHECK_EQ(1, app_logic::select_floating_frame(ReminderState::Interrupted, 14));
    CHECK_EQ(0, app_logic::select_floating_frame(ReminderState::Interrupted, 15));
    CHECK_EQ(0, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Idle, 0));
    CHECK_EQ(1, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Idle, 11));
    CHECK_EQ(0, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Idle, 12));
    CHECK_EQ(2, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Busy, 0));
    CHECK_EQ(3, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Busy, 11));
    CHECK_EQ(4, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Completed, 0));
    CHECK_EQ(6, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Error, 0));
    CHECK_EQ(7, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Error, 11));
    CHECK_EQ(8, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Interrupted, 0));
    CHECK_EQ(9, app_logic::select_dock_sprite_index(DockEdge::Left, ReminderState::Interrupted, 11));
    CHECK_EQ(12, app_logic::select_dock_sprite_index(DockEdge::Right, ReminderState::Busy, 0));
    CHECK_EQ(18, app_logic::select_dock_sprite_index(DockEdge::Right, ReminderState::Interrupted, 0));
    CHECK_EQ(std::string("任务失败：构建安装包"), app_logic::format_abnormal_task_text("构建安装包"));
    CHECK_EQ(std::string("任务失败：未知任务"), app_logic::format_abnormal_task_text(""));
    CHECK_EQ(std::string("任务已中断：构建安装包"), app_logic::format_interrupted_task_text("构建安装包"));
    CHECK_EQ(std::string("任务已中断：未知任务"), app_logic::format_interrupted_task_text(""));

    const auto start = Clock::now();
    CHECK(app_logic::should_show_dock(start, start + std::chrono::milliseconds(9900), false, false,
                                      Clock::time_point::min(), 10));
    CHECK(!app_logic::should_show_dock(start, start + std::chrono::milliseconds(10100), false, false,
                                       Clock::time_point::min(), 10));
    CHECK(app_logic::should_show_dock(start, start + std::chrono::hours(1), false, false,
                                      Clock::time_point::min(), 0));
    const auto expired = start + std::chrono::seconds(20);
    CHECK(app_logic::should_show_thought_bubble(false, ReminderState::Idle, expired, Clock::time_point::min()));
    CHECK(!app_logic::should_show_thought_bubble(true, ReminderState::Idle, expired, expired + std::chrono::seconds(5)));
    CHECK(app_logic::should_show_thought_bubble(true, ReminderState::Busy, expired, expired + std::chrono::seconds(5)));
    CHECK(app_logic::should_show_thought_bubble(true, ReminderState::Interrupted, expired, expired + std::chrono::seconds(5)));

    const RectD desktop{100, 50, 1200, 800};
    const auto left = app_logic::dock_hover_bounds(DockEdge::Left, desktop, 400, 1, true, 240);
    const auto right = app_logic::dock_hover_bounds(DockEdge::Right, desktop, 400, 1, true, 240);
    CHECK(left.contains({101, 510}));
    CHECK(!left.contains({101, 530}));
    CHECK(right.contains({1299, 400}));
    CHECK(!right.contains({1200, 400}));
    CHECK(app_logic::segment_intersects_rect({-10, 5}, {20, 5}, {0, 0, 10, 10}));
    CHECK(!app_logic::segment_intersects_rect({-10, -1}, {20, -1}, {0, 0, 10, 10}));
    CHECK(app_logic::segment_intersects_rect({5, 5}, {20, 20}, {0, 0, 10, 10}));
    const RectD bubble{10, 20, 300, 150};
    const RectD content{100, 70, 150, 60};
    CHECK(app_logic::is_task_switch_point(true, true, ReminderState::Busy, 2, bubble, content, {15, 25}));
    CHECK(!app_logic::is_task_switch_point(false, true, ReminderState::Busy, 2, bubble, content, {15, 25}));
}


void test_render_layout_contract() {
    using namespace render_layout;

    const State floating{ReminderState::Idle, DockEdge::None, false, false, false, 1.0, 0};
    const auto floating_bubble = bubble_bounds(floating);
    CHECK_NEAR(75.0, floating_bubble.x);
    CHECK_NEAR(45.0, floating_bubble.y);
    CHECK_NEAR(270.0, floating_bubble.width);
    CHECK_NEAR(110.0, floating_bubble.height);

    const auto visible_cloud = visible_cloud_bounds(floating);
    CHECK_NEAR(96.9375, visible_cloud.x);
    CHECK_NEAR(46.990950226244344, visible_cloud.y);
    CHECK_NEAR(226.125, visible_cloud.width);
    CHECK_NEAR(96.0633484162896, visible_cloud.height);

    const auto floating_pet = floating_pet_bounds(floating);
    CHECK_NEAR(150.23464838541668, floating_pet.x);
    CHECK_NEAR(151.3269230769231, floating_pet.y);
    CHECK_NEAR(floating_pet_width, floating_pet.width);
    CHECK_NEAR(floating_pet_height, floating_pet.height);

    const auto header = header_bounds(floating);
    const auto body = body_bounds(floating);
    CHECK_NEAR(145.2, header.x);
    CHECK_NEAR(56.0, header.y);
    CHECK_NEAR(140.4, header.width);
    CHECK_NEAR(22.0, header.height);
    CHECK_NEAR(156.0, body.x);
    CHECK_NEAR(82.4, body.y);
    CHECK_NEAR(156.0, body.width);
    CHECK_NEAR(45.0, body.height);
    const auto bulb = bulb_origin(floating);
    CHECK_NEAR(120.0, bulb.x);
    CHECK_NEAR(78.0, bulb.y);

    const State left_above{ReminderState::Busy, DockEdge::Left, true, false, false, 1.0, 18};
    const State right_above{ReminderState::Busy, DockEdge::Right, true, false, false, 1.0, 18};
    const State left_below{ReminderState::Busy, DockEdge::Left, true, true, false, 1.0, 18};
    const State right_below{ReminderState::Busy, DockEdge::Right, true, true, false, 1.0, 18};
    const State mirrored{ReminderState::Idle, DockEdge::None, false, false, true, 1.0, 0};
    const auto left_bubble = bubble_bounds(left_above);
    const auto right_bubble = bubble_bounds(right_above);
    CHECK_NEAR(27.0, left_bubble.x);
    CHECK_NEAR(123.0, right_bubble.x);
    CHECK_NEAR(51.0, left_bubble.y);
    CHECK_NEAR(103.0, bubble_bounds(left_below).y);
    CHECK_NEAR(103.0, bubble_bounds(right_below).y);
    CHECK_NEAR(123.0, bubble_bounds(right_below).x);
    CHECK(floating_pet_bounds(mirrored).x < floating_pet.x);

    const auto left_pet = dock_pet_bounds(left_above);
    const auto right_pet = dock_pet_bounds(right_above);
    const auto below_pet = dock_pet_bounds(left_below);
    CHECK_NEAR(0.0, left_pet.x);
    CHECK_NEAR(316.0, right_pet.x);
    CHECK_NEAR(149.0, left_pet.y);
    CHECK_NEAR(4.0, below_pet.y);
    CHECK_NEAR(dock_pet_size, left_pet.width);
    CHECK_NEAR(dock_pet_size, left_pet.height);
    CHECK_NEAR(201.0, dock_pet_center_y(left_above));
    CHECK_NEAR(56.0, dock_pet_center_y(left_below));

    const auto left_visible_pet = visible_pet_bounds(left_above);
    const auto right_visible_pet = visible_pet_bounds(right_above);
    const auto right_below_visible_pet = visible_pet_bounds(right_below);
    CHECK_NEAR(0.0, left_visible_pet.x);
    CHECK_NEAR(153.875, left_visible_pet.y);
    CHECK_NEAR(67.84375, left_visible_pet.width);
    CHECK_NEAR(352.15625, right_visible_pet.x);
    CHECK_NEAR(8.875, right_below_visible_pet.y);
    CHECK_NEAR(150.0, dock_bubble_switch_margin);

    const auto floating_dots = thought_dot_bounds(floating);
    const auto dock_dots = thought_dot_bounds(left_above);
    CHECK_NEAR(17.0, floating_dots.large.width);
    CHECK_NEAR(15.0, floating_dots.large.height);
    CHECK_NEAR(11.0, floating_dots.secondary.width);
    CHECK_NEAR(10.0, floating_dots.secondary.height);
    CHECK(dock_dots.large.width > dock_dots.secondary.width);
    CHECK(dock_dots.large.y > left_bubble.y);
}

void test_visual_content_contract() {
    MonitorSnapshot snapshot;
    const auto idle = make_visual_content(ReminderState::Idle, snapshot);
    CHECK_EQ(std::string("空闲"), idle.status_text);
    CHECK_EQ(std::string("主人，现在没有在进行中的任务!别让我歇着!"), idle.thought_text);
    CHECK(idle.task_titles.empty());
    CHECK(idle.progress_labels.empty());

    snapshot.active_count = 2;
    snapshot.latest_event_active_title_index = 1;
    snapshot.total_plan_step_count = 5;
    snapshot.completed_plan_step_count = 2;
    snapshot.active_titles = {"编译 Windows", "校验 macOS"};
    snapshot.active_plan_progress_labels = {std::optional<std::string>("1/3"), std::nullopt};
    const auto busy = make_visual_content(ReminderState::Busy, snapshot);
    CHECK_EQ(std::string("进行中(2/5) • 2/2"), busy.status_text);
    CHECK_EQ(std::string("编译 Windows"), busy.thought_text);
    CHECK_EQ(std::size_t(2), busy.task_titles.size());
    CHECK_EQ(std::string("校验 macOS"), busy.task_titles[1]);
    CHECK_EQ(std::size_t(2), busy.progress_labels.size());
    CHECK_EQ(std::string("1/3"), *busy.progress_labels[0]);
    CHECK(!busy.progress_labels[1].has_value());

    MonitorSnapshot busy_fallback;
    busy_fallback.active_count = 1;
    const auto fallback = make_visual_content(ReminderState::Busy, busy_fallback);
    CHECK_EQ(std::string("正在认真处理你的任务…"), fallback.thought_text);
    CHECK_EQ(std::size_t(1), fallback.task_titles.size());
    CHECK_EQ(std::string("正在处理任务…"), fallback.task_titles[0]);

    snapshot.last_completed_title = "打包完成";
    const auto completed = make_visual_content(ReminderState::Completed, snapshot);
    CHECK_EQ(std::string("已完成"), completed.status_text);
    CHECK_EQ(std::string("任务完成啦！"), completed.thought_text);
    CHECK_EQ(std::size_t(1), completed.task_titles.size());
    CHECK_EQ(std::string("打包完成"), completed.task_titles[0]);
    CHECK(completed.progress_labels.empty());

    snapshot.last_aborted_title = "签名失败";
    const auto error = make_visual_content(ReminderState::Error, snapshot);
    CHECK_EQ(std::string("异常"), error.status_text);
    CHECK_EQ(std::string("任务出现异常了。"), error.thought_text);
    CHECK_EQ(std::size_t(1), error.task_titles.size());
    CHECK_EQ(std::string("任务失败：签名失败"), error.task_titles[0]);
    CHECK(error.progress_labels.empty());

    snapshot.last_interrupted_title = "用户取消";
    const auto interrupted = make_visual_content(ReminderState::Interrupted, snapshot);
    CHECK_EQ(std::string("已中断"), interrupted.status_text);
    CHECK_EQ(std::string("任务已中断了。"), interrupted.thought_text);
    CHECK_EQ(std::size_t(1), interrupted.task_titles.size());
    CHECK_EQ(std::string("任务已中断：用户取消"), interrupted.task_titles[0]);
    CHECK(interrupted.progress_labels.empty());
}

void test_paths_and_settings() {
    const auto before = environment_utf8(paths::kHomeEnvironmentVariable);
    set_environment(paths::kHomeEnvironmentVariable, std::nullopt);
    CHECK_EQ(std::string(".codex"), path_to_utf8(paths::default_home().filename()));
    CHECK_EQ(paths::default_home() / "sessions", paths::default_sessions_root());
    CHECK_EQ(paths::default_home() / "config.toml", paths::default_config_file());

    TempDirectory home("CodeXPetsHome_");
    set_environment(paths::kHomeEnvironmentVariable, path_to_utf8(home.path));
    CHECK_EQ(std::filesystem::absolute(home.path).lexically_normal(), paths::default_home());
    set_environment(paths::kHomeEnvironmentVariable, before);

#ifdef _WIN32
    CHECK_EQ(paths::default_sessions_root(), paths::normalize_sessions_root("~/Library/Application Support/Codex"));
#else
    CHECK_EQ(paths::default_sessions_root(), paths::normalize_sessions_root("C:\\Users\\Someone\\.codex\\sessions"));
#endif

    AppSettings normalized;
    normalized.dock_hover_height = -1;
    normalized.dock_idle_hide_seconds = 9000;
    normalized.dock_reveal_seconds = 0;
    normalized.dock_notification_seconds = 1000;
    normalized.sessions_root.clear();
    normalized.normalize();
    CHECK_EQ(40, normalized.dock_hover_height);
    CHECK_EQ(3600, normalized.dock_idle_hide_seconds);
    CHECK_EQ(1, normalized.dock_reveal_seconds);
    CHECK_EQ(120, normalized.dock_notification_seconds);
    CHECK_EQ(paths::default_sessions_root(), normalized.sessions_root);

    TempDirectory root("CodeXPetsSettings_");
    JsonSettingsStore store(root.path / "settings.json");
    AppSettings settings;
    settings.dock_hover_height = 320;
    settings.dock_idle_hide_seconds = 14;
    settings.dock_reveal_seconds = 4;
    settings.dock_notification_seconds = 8;
    settings.sound_enabled = false;
    settings.pet_visible = false;
    settings.sessions_root = root.path / "sessions";
    settings.pet_position = PetPositionState{DockEdge::Right, "Display A|0,0,1920,1080", 1, 0.42};
    std::string error;
    CHECK(store.save(settings, &error));
    const auto loaded = store.load();
    CHECK_EQ(320, loaded.dock_hover_height);
    CHECK_EQ(14, loaded.dock_idle_hide_seconds);
    CHECK_EQ(4, loaded.dock_reveal_seconds);
    CHECK_EQ(8, loaded.dock_notification_seconds);
    CHECK(!loaded.sound_enabled);
    CHECK(!loaded.pet_visible);
    CHECK_EQ(std::filesystem::absolute(settings.sessions_root).lexically_normal(), loaded.sessions_root);
    CHECK(loaded.pet_position == settings.pet_position);
    CHECK(!std::filesystem::exists(root.path / "settings.json.tmp"));

    { std::ofstream invalid(root.path / "invalid.json"); invalid << "{ invalid json"; }
    const auto fallback = JsonSettingsStore(root.path / "invalid.json").load();
    CHECK(fallback.pet_visible && fallback.sound_enabled);

    const auto legacy = deserialize_legacy_macos_position(
        R"({"dockEdge":"right","screenIdentifier":"Studio Display","relativeX":1,"relativeY":0.25})");
    CHECK(legacy.has_value());
    CHECK_EQ(DockEdge::Right, legacy->dock_edge);
    CHECK_EQ(std::string("Studio Display"), legacy->screen_identifier);
    CHECK(std::abs(legacy->relative_y - 0.75) < 0.0001);
    CHECK(!deserialize_legacy_macos_position("{ invalid json").has_value());
}

void test_session_monitor_lifecycle() {
    TempDirectory root("CodeXPetsMonitor_");
    const auto file = create_session(root.path, "rollout-main.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Hello title test\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();
    CHECK_EQ(1, monitor.active_count());
    CHECK_EQ(std::string("Hello title test"), monitor.primary_active_title());

    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"response_item\",\"payload\":{" 
        "\"type\":\"function_call\",\"name\":\"update_plan\"," 
        "\"arguments\":\"{\\\"plan\\\":[{\\\"step\\\":\\\"Inspect\\\",\\\"status\\\":\\\"completed\\\"},"
        "{\\\"step\\\":\\\"Build feature\\\",\\\"status\\\":\\\"in_progress\\\"},"
        "{\\\"step\\\":\\\"Test\\\",\\\"status\\\":\\\"pending\\\"}] }\"," 
        "\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"A\"}}}\n");
    monitor.poll();
    CHECK_EQ(3, monitor.total_plan_step_count());
    CHECK_EQ(1, monitor.completed_plan_step_count());
    CHECK_EQ(std::string("Build feature"), monitor.primary_current_plan_step());
    const auto labels = monitor.active_plan_progress_labels();
    CHECK_EQ(std::size_t(1), labels.size());
    CHECK(labels[0].has_value() && *labels[0] == "1/3");

    (void)monitor.take_events();
    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\",\"last_agent_message\":\"ok\"}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Hello title test"), monitor.last_completed_title());
    auto events = monitor.take_events();
    CHECK(std::find(events.begin(), events.end(), MonitorEventKind::TaskCompleted) != events.end());

    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:05Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:06Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Abort title test\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:07Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"B\"}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Abort title test"), monitor.last_interrupted_title());
    events = monitor.take_events();
    CHECK(std::find(events.begin(), events.end(), MonitorEventKind::TaskInterrupted) != events.end());
}

void test_plan_update_focus_survives_later_event() {
    TempDirectory root("CodeXPetsPlanFocus_");
    const auto file = create_session(root.path, "rollout-plan-focus.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();

    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"name\":\"update_plan\",\"arguments\":\"{\\\"plan\\\":[{\\\"step\\\":\\\"Inspect\\\",\\\"status\\\":\\\"completed\\\"},{\\\"step\\\":\\\"Build\\\",\\\"status\\\":\\\"in_progress\\\"}] }\",\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"A\"}}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"agent_message\",\"turn_id\":\"B\",\"message\":\"Still working\"}}\n");
    monitor.poll();

    const auto snapshot = monitor.snapshot();
    const auto events = monitor.take_events();
    CHECK(std::find(events.begin(), events.end(), MonitorEventKind::PlanUpdated) != events.end());
    CHECK(snapshot.latest_plan_update_active_title_index >= 0);
    CHECK(snapshot.latest_plan_update_active_title_index < snapshot.active_count);
    CHECK(snapshot.active_plan_progress_labels[
              static_cast<std::size_t>(snapshot.latest_plan_update_active_title_index)]);
    CHECK_EQ(std::string("1/2"), *snapshot.active_plan_progress_labels[
              static_cast<std::size_t>(snapshot.latest_plan_update_active_title_index)]);
    CHECK_EQ(std::string("agent_message"), snapshot.last_event_type);
}

void test_session_monitor_success_and_order() {
    TempDirectory root("CodeXPetsFailure_");
    const auto file = create_session(root.path, "rollout-failure.jsonl",
        "{\"type\":\"response_item\",\"payload\":{\"text\":\"event_msg task_started fake\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"F\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Failure case\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();
    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"F\",\"last_agent_message\":\"Internal server error\"}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    // Without a structured error/status field, a completion is success even if
    // the agent's closing message mentions failure-related words.
    CHECK_EQ(std::string("Failure case"), monitor.last_completed_title());
    const auto events = monitor.take_events();
    CHECK(std::find(events.begin(), events.end(), MonitorEventKind::TaskCompleted) != events.end());

    TempDirectory ordered_root("CodeXPetsOrder_");
    const auto ordered_file = create_session(ordered_root.path, "rollout-order.jsonl", "");
    CodexSessionMonitor ordered(ordered_root.path);
    (void)ordered.take_events();
    append(ordered_file,
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n"
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\"}}\n"
        "{\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n");
    ordered.poll();
    const auto ordered_events = ordered.take_events();
    std::vector<MonitorEventKind> lifecycle;
    for (const auto event : ordered_events) if (event != MonitorEventKind::StateChanged) lifecycle.push_back(event);
    CHECK_EQ(std::size_t(3), lifecycle.size());
    CHECK_EQ(MonitorEventKind::TaskStarted, lifecycle[0]);
    CHECK_EQ(MonitorEventKind::TaskCompleted, lifecycle[1]);
    CHECK_EQ(MonitorEventKind::TaskStarted, lifecycle[2]);
    CHECK_EQ(1, ordered.active_count());
}


void test_session_monitor_error_object() {
    // Real Codex failure shape: task_complete carries an object-valued "error".
    TempDirectory root("CodeXPetsError_");
    const auto file = create_session(root.path, "rollout-error-object.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"E\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Error case\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();
    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"E\",\"last_agent_message\":null,\"error\":{\"message\":\"{\\\"error\\\":{\\\"message\\\":\\\"An assistant message with 'tool_calls' must be followed by tool messages\\\",\\\"type\\\":\\\"invalid_request_error\\\"}}\",\"codex_error_info\":\"other\"}}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Error case"), monitor.last_aborted_title());
    const auto events = monitor.take_events();
    CHECK(std::find(events.begin(), events.end(), MonitorEventKind::TaskAborted) != events.end());

    // Fallback: abnormal completion arrives without a tracked turn (out-of-order / lost turn).
    TempDirectory lost_root("CodeXPetsLostTurn_");
    const auto lost_file = create_session(lost_root.path, "rollout-lost-turn.jsonl", "");
    CodexSessionMonitor lost(lost_root.path);
    (void)lost.take_events();
    append(lost_file,
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"MISSING\",\"error\":{\"message\":\"invalid_request_error\"}}}\n");
    lost.poll();
    CHECK_EQ(0, lost.active_count());
    const auto lost_events = lost.take_events();
    CHECK(std::find(lost_events.begin(), lost_events.end(), MonitorEventKind::TaskAborted) != lost_events.end());

    // Structured failure signal: task_complete carries an explicit failed status.
    TempDirectory status_root("CodeXPetsStatusFailed_");
    const auto status_file = create_session(status_root.path, "rollout-status-failed.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"S\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Status failed case\"}}\n");
    CodexSessionMonitor status_monitor(status_root.path);
    (void)status_monitor.take_events();
    append(status_file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"S\",\"status\":\"failed\"}}\n");
    status_monitor.poll();
    CHECK_EQ(0, status_monitor.active_count());
    CHECK_EQ(std::string("Status failed case"), status_monitor.last_aborted_title());
    const auto status_events = status_monitor.take_events();
    CHECK(std::find(status_events.begin(), status_events.end(), MonitorEventKind::TaskAborted) != status_events.end());
}

void test_session_monitor_v2_aliases_and_non_fatal_error() {
    // v2 interop aliases: turn_started / turn_complete map to task_started / task_complete.
    TempDirectory alias_root("CodeXPetsAlias_");
    const auto alias_file = create_session(alias_root.path, "rollout-alias.jsonl", "");
    CodexSessionMonitor alias_monitor(alias_root.path);
    (void)alias_monitor.take_events();
    append(alias_file,
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_started\",\"turn_id\":\"T\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Alias case\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_complete\",\"turn_id\":\"T\"}}\n");
    alias_monitor.poll();
    CHECK_EQ(0, alias_monitor.active_count());
    CHECK_EQ(std::string("Alias case"), alias_monitor.last_completed_title());

    // codex_error_info values that do not affect turn status are not abnormal.
    TempDirectory nonfatal_root("CodeXPetsNonFatal_");
    const auto nonfatal_file = create_session(nonfatal_root.path, "rollout-nonfatal.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"N\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Non fatal case\"}}\n");
    CodexSessionMonitor nonfatal_monitor(nonfatal_root.path);
    (void)nonfatal_monitor.take_events();
    append(nonfatal_file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"N\",\"error\":{\"message\":\"rollback performed\",\"codex_error_info\":\"thread_rollback_failed\"}}}\n");
    nonfatal_monitor.poll();
    CHECK_EQ(0, nonfatal_monitor.active_count());
    CHECK_EQ(std::string("Non fatal case"), nonfatal_monitor.last_completed_title());
    const auto nonfatal_events = nonfatal_monitor.take_events();
    CHECK(std::find(nonfatal_events.begin(), nonfatal_events.end(), MonitorEventKind::TaskCompleted) != nonfatal_events.end());
}

void test_open_file_activity_and_stale_rule() {
    TempDirectory root("CodeXPetsOpen_");
    const auto recent = std::chrono::system_clock::now() - std::chrono::minutes(1);
    const auto recent_raw = std::chrono::system_clock::to_time_t(recent);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &recent_raw);
#else
    gmtime_r(&recent_raw, &utc);
#endif
    char timestamp[40]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    const auto file = create_session(root.path, "rollout-open-file.jsonl",
        std::string("{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"OPEN\"}}\n") +
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Still running\"}}\n" +
        "{\"timestamp\":\"" + timestamp + "\",\"type\":\"response_item\",\"payload\":{\"type\":\"reasoning\",\"summary\":[]}}\n");
    std::filesystem::last_write_time(file,
        std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30));
    CodexSessionMonitor monitor(root.path);
    CHECK_EQ(1, monitor.active_count());
    monitor.poll();
    CHECK_EQ(1, monitor.active_count());
    CHECK_EQ(std::string("Still running"), monitor.primary_active_title());

    const auto now = SystemClock::now();
    CHECK(CodexSessionMonitor::is_turn_stale(now - std::chrono::minutes(20),
                                             now - std::chrono::minutes(20), now, 600));
    CHECK(!CodexSessionMonitor::is_turn_stale(now - std::chrono::minutes(1),
                                              now - std::chrono::minutes(20), now, 600));
    CHECK(!CodexSessionMonitor::is_turn_stale(now - std::chrono::minutes(20),
                                              now - std::chrono::minutes(1), now, 600));
}

void test_json_parser() {
    const auto value = parse_json(R"({"text":"中文\nline","emoji":"\ud83d\udc31","n":3,"ok":true,"items":[1,2]})");
    CHECK(value.is_object());
    CHECK_EQ(std::string("中文\nline"), value.get("text")->string());
    CHECK_EQ(std::string("🐱").size(), value.get("emoji")->string().size());
    CHECK_EQ(3, value.get("n")->int_or());
    CHECK(value.get("ok")->boolean());
    CHECK_EQ(std::size_t(2), value.get("items")->array().size());
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"app_logic", test_app_logic},
        {"render_layout_contract", test_render_layout_contract},
        {"visual_content_contract", test_visual_content_contract},
        {"paths_and_settings", test_paths_and_settings},
        {"session_monitor_lifecycle", test_session_monitor_lifecycle},
        {"session_monitor_success_and_order", test_session_monitor_success_and_order},
        {"plan_update_focus_survives_later_event", test_plan_update_focus_survives_later_event},
        {"session_monitor_error_object", test_session_monitor_error_object},
        {"session_monitor_v2_aliases_and_non_fatal_error", test_session_monitor_v2_aliases_and_non_fatal_error},
        {"open_file_activity_and_stale_rule", test_open_file_activity_and_stale_rule},
        {"json_parser", test_json_parser},
    };
    int failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }
    std::cout << (tests.size() - static_cast<std::size_t>(failures)) << "/" << tests.size()
              << " test groups passed\n";
    return failures == 0 ? 0 : 1;
}

