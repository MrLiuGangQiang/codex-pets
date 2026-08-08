#include "app_logic.h"
#include "json.h"
#include "monitor_policy.h"
#include "monitor_update_queue.h"
#include "paths.h"
#include "platform_text.h"
#include "presentation.h"
#include "render_layout.h"
#include "session_monitor.h"
#include "settings.h"
#include "telegram_notifier.h"
#include "xiaomi_speaker.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
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

std::string utc_timestamp(SystemClock::time_point value) {
    const auto raw = SystemClock::to_time_t(value);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char timestamp[40]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return timestamp;
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

    const std::vector<std::string> active_titles{"任务 A", "任务 B"};
    const std::vector<std::string> project_names{"项目 A", "项目 B"};
    CHECK_EQ(std::string_view("项目 B"),
             app_logic::select_notification_label(project_names, 1));
    CHECK_EQ(std::string_view("项目 A"),
             app_logic::select_notification_label(project_names, -1));
    CHECK(app_logic::select_notification_label({}, 0).empty());

    CHECK_EQ(std::string("进行中(1/3) • 2/4"),
             app_logic::format_busy_header(std::string_view("1/3"), 1, 4));
    CHECK_EQ(std::string("进行中 • 2/4"), app_logic::format_busy_header(std::nullopt, 1, 4));
    CHECK_EQ(std::string("进行中(1/3)"),
             app_logic::format_busy_header(std::string_view("1/3"), 0, 1));

    CHECK_EQ(std::vector<std::string>({"项目 A：进行中（1/3）", "项目 B：进行中"}),
             app_logic::format_active_task_status_lines(
                 project_names, active_titles,
                 {std::optional<std::string>("1/3"), std::nullopt}));
    CHECK_EQ(std::vector<std::string>({"任务 B：进行中"}),
             app_logic::format_active_task_status_lines({""}, {"任务 B"}, {}));

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
    const auto floating_interaction = pet_interaction_bounds(floating);
    CHECK_NEAR(floating_pet.x, floating_interaction.x);
    CHECK_NEAR(floating_pet.y, floating_interaction.y);
    CHECK_NEAR(floating_pet.width, floating_interaction.width);
    CHECK_NEAR(floating_pet.height, floating_interaction.height);

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
    const auto dock_interaction = pet_interaction_bounds(left_above);
    CHECK_NEAR(left_pet.x, dock_interaction.x);
    CHECK_NEAR(left_pet.y, dock_interaction.y);
    CHECK_NEAR(left_pet.width, dock_interaction.width);
    CHECK_NEAR(left_pet.height, dock_interaction.height);
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
    snapshot.active_project_names = {"Windows", "macOS"};
    snapshot.active_plan_progress_labels = {std::optional<std::string>("1/3"), std::nullopt};
    const auto busy = make_visual_content(ReminderState::Busy, snapshot);
    CHECK_EQ(std::string("Windows：进行中（1/3）\nmacOS：进行中"), busy.status_text);
    CHECK_EQ(std::vector<std::string>({"Windows：进行中（1/3）", "macOS：进行中"}),
             busy.status_lines);
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
    settings.xiaoai.enabled = true;
    settings.xiaoai.auth_cookies = "serviceToken=test-token";
    settings.xiaoai.device_id = "speaker-device";
    settings.xiaoai.device_ids = {"speaker-device", "speaker-kitchen", "speaker-device"};
    settings.xiaoai.max_parallel_requests = 5;
    settings.xiaoai.notify_interrupted = false;
    settings.telegram.enabled = true;
    settings.telegram.bot_token = "secret-bot-token";
    settings.telegram.chat_id = " 123456789 ";
    settings.telegram.notify_error = false;
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
    CHECK(loaded.xiaoai.enabled);
    CHECK(loaded.xiaoai.auth_cookies.empty());
    CHECK_EQ(settings.xiaoai.device_id, loaded.xiaoai.device_id);
    CHECK_EQ(2, static_cast<int>(loaded.xiaoai.device_ids.size()));
    CHECK_EQ(std::string("speaker-device"), loaded.xiaoai.device_ids[0]);
    CHECK_EQ(std::string("speaker-kitchen"), loaded.xiaoai.device_ids[1]);
    CHECK_EQ(5, loaded.xiaoai.max_parallel_requests);
    CHECK(!loaded.xiaoai.notify_interrupted);
    CHECK(loaded.telegram.enabled);
    CHECK(loaded.telegram.bot_token.empty());
    CHECK_EQ(std::string("123456789"), loaded.telegram.chat_id);
    CHECK(!loaded.telegram.notify_error);
    {
        std::ifstream saved(root.path / "settings.json", std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(saved)),
                                   std::istreambuf_iterator<char>());
        CHECK(contents.find("secret-bot-token") == std::string::npos);
    }
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

void test_xiaoai_protocol() {
    std::vector<XiaoAiHttpRequest> requests;
    XiaoAiHttpTransport transport = [&](const XiaoAiHttpRequest& request) {
        requests.push_back(request);
        XiaoAiHttpResponse response;
        response.status = 200;
        if (requests.size() == 1) {
            response.body = R"({"code":0,"data":[{"deviceID":"speaker-device","hardware":"l09a","serialNumber":"speaker-serial","mac":"aa:bb:cc:dd:ee:ff","alias":"Art"}]})";
        } else {
            response.body = R"({"code":0})";
        }
        return response;
    };
    XiaoAiSettings settings;
    settings.enabled = true;
    settings.auth_cookies = "userId=user; serviceToken=token";
    settings.device_id = "speaker-device";
    XiaoAiNotifier notifier(std::move(transport));
    std::string error;
    CHECK(notifier.validate(settings, &error));
    CHECK(error.empty());
    CHECK_EQ(1, static_cast<int>(requests.size()));
    requests.clear();
    CHECK(notifier.test(settings, &error));
    CHECK(error.empty());
    CHECK_EQ(2, static_cast<int>(requests.size()));
    CHECK(requests[0].url.find("/admin/v2/device_list?") != std::string::npos);
    CHECK(requests[0].url.find("master=0") != std::string::npos);
    CHECK(requests[0].headers[0].second.find("MICO/AndroidApp/") != std::string::npos);
    const auto cookie = std::find_if(requests[0].headers.begin(), requests[0].headers.end(),
        [](const auto& header) { return header.first == "Cookie"; });
    CHECK(cookie != requests[0].headers.end());
    CHECK(cookie->second.find("serviceToken=token") != std::string::npos);
    CHECK(requests[1].body.find("text_to_speech") != std::string::npos);
    CHECK(requests[1].body.find("Codex") == std::string::npos);
    const auto ubus_cookie = std::find_if(requests[1].headers.begin(), requests[1].headers.end(),
        [](const auto& header) { return header.first == "Cookie"; });
    CHECK(ubus_cookie != requests[1].headers.end());
    CHECK(ubus_cookie->second.find("sn=speaker-serial") != std::string::npos);
    CHECK(ubus_cookie->second.find("hardware=l09a") != std::string::npos);
    CHECK(ubus_cookie->second.find("deviceId=speaker-device") != std::string::npos);

    XiaoAiHttpTransport async_transport = [](const XiaoAiHttpRequest& request) {
        if (request.url.find("/admin/v2/device_list?") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"data":[{"deviceID":"speaker-device","hardware":"l09a","alias":"Desk","miotDID":"12345"}]})", {}};
        }
        return XiaoAiHttpResponse{200, R"({"code":0})", {}};
    };
    XiaoAiNotifier async_notifier(std::move(async_transport));
    const auto caller_thread = std::this_thread::get_id();
    std::atomic_bool validate_callback_off_caller_thread{};
    std::promise<std::pair<XiaoAiSettings, std::string>> validate_promise;
    auto validate_future = validate_promise.get_future();
    async_notifier.validate_async(settings, [&validate_promise, &validate_callback_off_caller_thread, caller_thread](
                                          XiaoAiSettings validated, std::string async_error) {
        validate_callback_off_caller_thread.store(std::this_thread::get_id() != caller_thread,
                                                  std::memory_order_release);
        validate_promise.set_value({std::move(validated), std::move(async_error)});
    });
    CHECK(validate_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto validated = validate_future.get();
    CHECK(validated.second.empty());
    CHECK(validated.first.auth_cookies.find("serviceToken=token") != std::string::npos);

    std::promise<std::pair<std::vector<XiaoAiDeviceInfo>, std::string>> discover_promise;
    auto discover_future = discover_promise.get_future();
    async_notifier.discover_devices_async(settings, [&discover_promise](std::vector<XiaoAiDeviceInfo> devices,
                                                                          std::string async_error) {
        discover_promise.set_value({std::move(devices), std::move(async_error)});
    });
    CHECK(discover_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto discovered = discover_future.get();
    CHECK(discovered.second.empty());
    CHECK_EQ(1, static_cast<int>(discovered.first.size()));
    CHECK_EQ(std::string("speaker-device"), discovered.first.front().id);
    CHECK_EQ(std::string("12345"), discovered.first.front().miot_did);

    std::promise<std::string> test_promise;
    auto test_future = test_promise.get_future();
    async_notifier.test_async(settings, [&test_promise](std::string async_error) {
        test_promise.set_value(std::move(async_error));
    });
    CHECK(test_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(test_future.get().empty());
    CHECK(validate_callback_off_caller_thread.load(std::memory_order_acquire));

    std::vector<XiaoAiHttpRequest> multi_requests;
    std::mutex multi_mutex;
    std::atomic_int active_requests{};
    std::atomic_int peak_requests{};
    XiaoAiHttpTransport multi_transport = [&](const XiaoAiHttpRequest& request) {
        {
            std::lock_guard lock(multi_mutex);
            multi_requests.push_back(request);
        }
        if (request.url.find("/admin/v2/device_list?") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"data":[{"deviceID":"speaker-1","hardware":"l09a"},{"deviceID":"speaker-2","hardware":"l09a"},{"deviceID":"speaker-3","hardware":"l09a"},{"deviceID":"speaker-4","hardware":"l09a"},{"deviceID":"speaker-5","hardware":"l09a"}]})", {}};
        }
        const auto active = active_requests.fetch_add(1) + 1;
        auto observed = peak_requests.load();
        while (active > observed && !peak_requests.compare_exchange_weak(observed, active)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        active_requests.fetch_sub(1);
        return XiaoAiHttpResponse{200, R"({"code":0})", {}};
    };
    XiaoAiSettings multi_settings = settings;
    multi_settings.device_ids = {"speaker-1", "speaker-2", "speaker-3", "speaker-4", "speaker-5"};
    multi_settings.device_id = multi_settings.device_ids.front();
    multi_settings.max_parallel_requests = 3;
    XiaoAiNotifier multi_notifier(std::move(multi_transport));
    CHECK(multi_notifier.test(multi_settings, &error));
    CHECK(error.empty());
    CHECK_EQ(6, static_cast<int>(multi_requests.size()));
    CHECK_EQ(3, peak_requests.load());
    std::vector<std::string> bodies;
    {
        std::lock_guard lock(multi_mutex);
        for (const auto& request : multi_requests) {
            if (!request.body.empty()) bodies.push_back(request.body);
        }
    }
    CHECK_EQ(5, static_cast<int>(bodies.size()));
    for (int index = 1; index <= 5; ++index) {
        CHECK(std::any_of(bodies.begin(), bodies.end(), [index](const std::string& body) {
            return body.find("deviceId=speaker-" + std::to_string(index)) != std::string::npos;
        }));
    }

    std::vector<XiaoAiHttpRequest> lx04_requests;
    XiaoAiHttpTransport lx04_transport = [&](const XiaoAiHttpRequest& request) {
        lx04_requests.push_back(request);
        if (request.url.find("serviceLogin?sid=micoapi") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"ssecurity":"MDEyMzQ1Njc4OWFiY2RlZg==","nonce":"123","location":"https://account.xiaomi.com/sts/mico?sid=micoapi"})", {}};
        }
        if (request.url.find("/sts/mico?") != std::string::npos) {
            return XiaoAiHttpResponse{302, {}, {{"Set-Cookie", "serviceToken=mina-token; Path=/; HttpOnly"}}};
        }
        if (request.url.find("/admin/v2/device_list?") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"data":[{"deviceID":"speaker-lx04","hardware":"LX04","alias":"Touch"}]})", {}};
        }
        if (request.url.find("serviceLogin?sid=xiaomiio") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"ssecurity":"MDEyMzQ1Njc4OWFiY2RlZg==","nonce":"456","location":"https://account.xiaomi.com/sts/miot?sid=xiaomiio"})", {}};
        }
        if (request.url.find("/sts/miot?") != std::string::npos) {
            return XiaoAiHttpResponse{302, {}, {{"Set-Cookie", "serviceToken=miot-token; Path=/; HttpOnly"}}};
        }
        if (request.url == "https://api.io.mi.com/app/home/device_list") {
            return XiaoAiHttpResponse{200,
                R"({"code":0,"result":{"list":[{"did":"123456789","name":"Touch","model":"xiaomi.wifispeaker.lx04"}]}})", {}};
        }
        if (request.url == "https://api.io.mi.com/app/miotspec/action") {
            return XiaoAiHttpResponse{200, R"({"code":0,"result":{"code":0}})", {}};
        }
        throw std::runtime_error("Unexpected LX04 request: " + request.url);
    };
    XiaoAiSettings lx04_settings;
    lx04_settings.enabled = true;
    lx04_settings.auth_cookies =
        "userId=user; passToken=passport-token; deviceId=browser-device";
    lx04_settings.device_id = "speaker-lx04";
    XiaoAiNotifier lx04_notifier(std::move(lx04_transport));
    if (!lx04_notifier.test(lx04_settings, &error)) {
        throw std::runtime_error("LX04 test failed: " + error);
    }
    CHECK(error.empty());
    CHECK_EQ(7, static_cast<int>(lx04_requests.size()));
    CHECK(lx04_requests[4].url.find("/sts/miot?") != std::string::npos);
    CHECK_EQ(std::string("https://api.io.mi.com/app/home/device_list"), lx04_requests[5].url);
    CHECK_EQ(std::string("https://api.io.mi.com/app/miotspec/action"), lx04_requests[6].url);
    CHECK(lx04_requests[6].body.find("data=") != std::string::npos);
    CHECK(lx04_requests[6].body.find("siid%22%3A5") != std::string::npos);
    CHECK(lx04_requests[6].body.find("aiid%22%3A1") != std::string::npos);
    CHECK(lx04_requests[6].body.find("signature=") != std::string::npos);
    const auto lx04_cookie = std::find_if(lx04_requests[6].headers.begin(), lx04_requests[6].headers.end(),
        [](const auto& header) { return header.first == "Cookie"; });
    CHECK(lx04_cookie != lx04_requests[6].headers.end());
    CHECK(lx04_cookie->second.find("serviceToken=miot-token") != std::string::npos);
    CHECK(lx04_cookie->second.find("PassportDeviceId=browser-device") != std::string::npos);
    CHECK(std::none_of(lx04_requests.begin(), lx04_requests.end(), [](const XiaoAiHttpRequest& request) {
        return request.body.find("text_to_speech") != std::string::npos;
    }));

    lx04_requests.clear();
    CHECK(lx04_notifier.validate(lx04_settings, &error));
    CHECK(error.empty());
    const auto persisted_lx04_authorization = compact_xiaoai_authorization(lx04_settings.auth_cookies);
    CHECK(persisted_lx04_authorization.find("passToken=") == std::string::npos);
    CHECK(persisted_lx04_authorization.find("codexpetsMiotSsecurity=") != std::string::npos);
    CHECK(persisted_lx04_authorization.find("codexpetsMiotServiceToken=") != std::string::npos);
    lx04_settings.auth_cookies = persisted_lx04_authorization;
    lx04_requests.clear();
    CHECK(lx04_notifier.test(lx04_settings, &error));
    CHECK(error.empty());
    CHECK_EQ(3, static_cast<int>(lx04_requests.size()));
    CHECK(lx04_requests[0].url.find("/admin/v2/device_list?") != std::string::npos);
    CHECK_EQ(std::string("https://api.io.mi.com/app/home/device_list"), lx04_requests[1].url);
    CHECK_EQ(std::string("https://api.io.mi.com/app/miotspec/action"), lx04_requests[2].url);

    std::vector<XiaoAiHttpRequest> passport_requests;
    XiaoAiHttpTransport passport_transport = [&](const XiaoAiHttpRequest& request) {
        passport_requests.push_back(request);
        if (request.url.find("account.xiaomi.com/pass/serviceLogin") != std::string::npos) {
            return XiaoAiHttpResponse{200,
                R"(&&&START&&&{"code":0,"ssecurity":"abc","nonce":"123","location":"https://api2.mina.mi.com/sts?sid=micoapi"})",
                {{"Set-Cookie", "serviceToken=account-token; Path=/; HttpOnly"}}};
        }
        if (request.url.find("/sts?") != std::string::npos) {
            return XiaoAiHttpResponse{302, {}, {{"Set-Cookie", "serviceToken=mina-token; Path=/; HttpOnly"}}};
        }
        return XiaoAiHttpResponse{200,
            R"({"code":0,"data":[{"deviceID":"speaker-device","hardware":"l09a"}]})", {}};
    };
    XiaoAiSettings passport_settings;
    passport_settings.enabled = true;
    passport_settings.auth_cookies = "userId=user; passToken=passport-token; deviceId=browser-device";
    passport_settings.device_id = "speaker-device";
    XiaoAiNotifier passport_notifier(std::move(passport_transport));
    CHECK(passport_notifier.validate(passport_settings, &error));
    CHECK(error.empty());
    CHECK_EQ(3, static_cast<int>(passport_requests.size()));
    CHECK(passport_requests[0].url.find("sid=micoapi") != std::string::npos);
    CHECK(passport_requests[1].url.find("_userIdNeedEncrypt=true") != std::string::npos);
    CHECK(passport_requests[1].url.find("clientSign=ehDduSM16SqOGc9aZjJ7lp3PGUk%3D") != std::string::npos);
    CHECK(!passport_requests[1].follow_redirects);
    CHECK(passport_requests[2].url.find("/admin/v2/device_list?") != std::string::npos);
    const auto mina_cookie = std::find_if(passport_requests[2].headers.begin(), passport_requests[2].headers.end(),
        [](const auto& header) { return header.first == "Cookie"; });
    CHECK(mina_cookie != passport_requests[2].headers.end());
    CHECK(mina_cookie->second.find("serviceToken=mina-token") != std::string::npos);
    CHECK(mina_cookie->second.find("serviceToken=account-token") == std::string::npos);
    CHECK(passport_settings.auth_cookies.find("serviceToken=mina-token") != std::string::npos);

    XiaoAiHttpTransport rejected_transport = [](const XiaoAiHttpRequest&) {
        return XiaoAiHttpResponse{401, {}, {}};
    };
    XiaoAiNotifier rejected_notifier(std::move(rejected_transport));
    CHECK(!rejected_notifier.test(settings, &error));
    CHECK_EQ(std::string("小米设备列表请求失败（HTTP 401）"), error);

    XiaoAiSettings malformed = settings;
    malformed.auth_cookies = "notserviceToken=token";
    CHECK(!rejected_notifier.test(malformed, &error));
    CHECK_EQ(std::string("请先点击“浏览器登录”完成小米授权"), error);

    XiaoAiHttpTransport failing_transport = [](const XiaoAiHttpRequest&) -> XiaoAiHttpResponse {
        throw std::runtime_error("network unavailable");
    };
    XiaoAiNotifier failing_notifier(std::move(failing_transport));
    CHECK(!failing_notifier.test(settings, &error));
    CHECK_EQ(std::string("network unavailable"), error);
}
void test_monitor_update_queue_and_policy() {
    MonitorUpdateQueue queue;
    MonitorSnapshot first_snapshot;
    first_snapshot.active_count = 1;
    first_snapshot.event_contexts = {"first"};
    TaskNotification started_notification;
    started_notification.state = TaskNotificationState::Started;
    started_notification.project_name = "first";
    first_snapshot.event_notifications = {started_notification};
    CHECK(queue.push(PendingMonitorUpdate{1, {MonitorEventKind::TaskStarted}, first_snapshot}));
    MonitorSnapshot second_snapshot;
    second_snapshot.active_count = 2;
    second_snapshot.event_contexts = {"", "second"};
    TaskNotification error_notification;
    error_notification.state = TaskNotificationState::Error;
    error_notification.project_name = "second";
    error_notification.summary = "compiler exited with code 1";
    second_snapshot.event_notifications = {std::nullopt, error_notification};
    CHECK(queue.push(PendingMonitorUpdate{1,
        {MonitorEventKind::PlanUpdated, MonitorEventKind::TaskAborted}, second_snapshot}));
    CHECK(!queue.push(PendingMonitorUpdate{0, {MonitorEventKind::TaskAborted}, {}}));
    auto updates = queue.take();
    CHECK_EQ(std::size_t(1), updates.size());
    CHECK_EQ(2, updates.front().snapshot.active_count);
    CHECK_EQ(std::size_t(3), updates.front().events.size());
    CHECK_EQ(MonitorEventKind::TaskAborted, updates.front().events.back());
    CHECK_EQ(std::size_t(3), updates.front().snapshot.event_contexts.size());
    CHECK_EQ(std::string("first"), updates.front().snapshot.event_contexts[0]);
    CHECK_EQ(std::string("second"), updates.front().snapshot.event_contexts[2]);
    CHECK_EQ(std::size_t(3), updates.front().snapshot.event_notifications.size());
    CHECK(updates.front().snapshot.event_notifications[0].has_value());
    CHECK(!updates.front().snapshot.event_notifications[1].has_value());
    CHECK(updates.front().snapshot.event_notifications[2].has_value());
    CHECK_EQ(std::string("compiler exited with code 1"),
             updates.front().snapshot.event_notifications[2]->summary);

    CHECK(queue.push(PendingMonitorUpdate{2, {MonitorEventKind::TaskStarted}, {}}));
    updates = queue.take();
    CHECK_EQ(std::uint64_t{2}, updates.front().generation);

    AppSettings settings;
    settings.dock_notification_seconds = 5;
    MonitorSnapshot snapshot;
    snapshot.active_count = 2;
    snapshot.active_project_names = {"alpha", "beta"};
    snapshot.latest_event_active_title_index = 1;
    snapshot.last_completed_project_name = "alpha";
    VisualStateCoordinator coordinator;
    const auto effects = apply_monitor_event_policy(
        coordinator, snapshot,
        {MonitorEventKind::TaskStarted, MonitorEventKind::TaskCompleted}, settings,
        Clock::now());
    CHECK_EQ(std::size_t(2), effects.size());
    CHECK(effects[0].reveal_pet);
    CHECK(effects[0].sound.has_value());
    CHECK_EQ(SoundCue::Started, *effects[0].sound);
    CHECK_EQ(XiaoAiEvent::Started, *effects[0].xiaoai_event);
    CHECK_EQ(std::string("beta"), effects[0].xiaoai_context);
    CHECK_EQ(SoundCue::Completed, *effects[1].sound);
    CHECK_EQ(std::string("alpha"), effects[1].xiaoai_context);
    CHECK_EQ(ReminderState::Completed, coordinator.select(snapshot.active_count, Clock::now()));

    snapshot.last_interrupted_project_name = "gamma";
    const auto interrupted_effects = apply_monitor_event_policy(
        coordinator, snapshot, {MonitorEventKind::TaskInterrupted}, settings, Clock::now());
    CHECK_EQ(std::size_t(1), interrupted_effects.size());
    CHECK(interrupted_effects[0].reveal_pet);
    CHECK(interrupted_effects[0].sound.has_value());
    CHECK_EQ(SoundCue::Interrupted, *interrupted_effects[0].sound);
    CHECK_EQ(XiaoAiEvent::Interrupted, *interrupted_effects[0].xiaoai_event);
    CHECK_EQ(std::string("gamma"), interrupted_effects[0].xiaoai_context);

    snapshot.event_contexts = {"alpha", "beta"};
    const auto started_effects = apply_monitor_event_policy(
        coordinator, snapshot, {MonitorEventKind::TaskStarted, MonitorEventKind::TaskStarted}, settings,
        Clock::now());
    CHECK_EQ(std::size_t(2), started_effects.size());
    CHECK_EQ(std::string("alpha"), started_effects[0].xiaoai_context);
    CHECK_EQ(std::string("beta"), started_effects[1].xiaoai_context);
}

void test_xiaoai_authorization_compaction() {
    CHECK_EQ(std::string("userId=u; serviceToken=s; deviceId=d"),
             compact_xiaoai_authorization("foo=x; userId=u; serviceToken=s; deviceId=d; bar=y"));
    CHECK_EQ(std::string("userId=u; serviceToken=s; deviceId=d"),
             compact_xiaoai_authorization("userId=u; serviceToken=s; deviceId=d; passToken=p"));
    CHECK_EQ(std::string("cUserId=c; serviceToken=s"),
             compact_xiaoai_authorization("cUserId=c; serviceToken=s"));
    const auto parts = split_xiaoai_authorization(
        "userId=u; serviceToken=s; deviceId=d; codexpetsMiotSsecurity=sec; "
        "codexpetsMiotServiceToken=miot");
    CHECK_EQ(std::string("userId=u; serviceToken=s; deviceId=d"), parts.mina);
    CHECK_EQ(std::string("sec"), parts.miot_ssecurity);
    CHECK_EQ(std::string("miot"), parts.miot_service_token);
    CHECK_EQ(std::string("userId=u; serviceToken=s; deviceId=d; "
                         "codexpetsMiotSsecurity=sec; codexpetsMiotServiceToken=miot"),
             combine_xiaoai_authorization(parts));
    CHECK(compact_xiaoai_authorization("userId=u").empty());
}

void test_xiaoai_start_context_tracks_each_task() {
    TempDirectory root("CodeXPetsXiaoAiContexts_");
    const auto alpha = create_session(root.path, "alpha.jsonl", "");
    const auto beta = create_session(root.path, "beta.jsonl", "");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();

    append(alpha,
        "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"session_meta\",\"payload\":{\"cwd\":\"D:\\\\Projects\\\\alpha\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n");
    append(beta,
        "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"session_meta\",\"payload\":{\"cwd\":\"D:\\\\Projects\\\\beta\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n");
    monitor.poll();
    const auto events = monitor.take_events();
    const auto snapshot = monitor.snapshot(false);
    CHECK_EQ(events.size(), snapshot.event_contexts.size());

    AppSettings settings;
    VisualStateCoordinator coordinator;
    const auto effects = apply_monitor_event_policy(coordinator, snapshot, events, settings, Clock::now());
    std::vector<std::string> labels;
    for (const auto& effect : effects) {
        if (effect.xiaoai_event && *effect.xiaoai_event == XiaoAiEvent::Started) {
            labels.push_back(effect.xiaoai_context);
        }
    }
    std::sort(labels.begin(), labels.end());
    CHECK_EQ(std::size_t(2), labels.size());
    CHECK_EQ(std::string("alpha"), labels[0]);
    CHECK_EQ(std::string("beta"), labels[1]);
}

void test_session_monitor_lifecycle() {
    TempDirectory root("CodeXPetsMonitor_");
    const auto file = create_session(root.path, "rollout-main.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"session_meta\",\"payload\":{\"cwd\":\"D:\\\\Projects\\alpha\",\"git\":{\"repository_url\":\"https://github.com/example/alpha.git\"}}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"A\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Hello title test\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();
    CHECK_EQ(1, monitor.active_count());
    CHECK_EQ(std::string("Hello title test"), monitor.primary_active_title());
    const auto lean_snapshot = monitor.snapshot(false);
    CHECK(lean_snapshot.diagnostics_text.empty());
    CHECK_EQ(1, lean_snapshot.active_count);
    CHECK_EQ(std::size_t(1), lean_snapshot.active_project_names.size());
    CHECK_EQ(std::string("alpha"), lean_snapshot.active_project_names.front());

    const std::string large_response(180 * 1024, 'x');
    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:02.5Z\",\"type\":\"response_item\","
        "\"payload\":{\"type\":\"message\",\"content\":\"" + large_response +
        " \\\"type\\\":\\\"event_msg\\\"\"}}\n");
    monitor.poll();
    CHECK_EQ(1, monitor.active_count());

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
    const auto plan_snapshot = monitor.snapshot(false);
    CHECK_EQ(std::size_t(1), plan_snapshot.active_plan_steps.size());
    CHECK_EQ(std::size_t(3), plan_snapshot.active_plan_steps[0].size());
    CHECK_EQ(TaskStepState::Completed, plan_snapshot.active_plan_steps[0][0].state);
    CHECK_EQ(TaskStepState::InProgress, plan_snapshot.active_plan_steps[0][1].state);
    CHECK_EQ(std::string("Build feature"), plan_snapshot.active_plan_steps[0][1].text);
    const auto labels = monitor.active_plan_progress_labels();
    CHECK_EQ(std::size_t(1), labels.size());
    CHECK(labels[0].has_value() && *labels[0] == "1/3");

    (void)monitor.take_events();
    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"A\",\"last_agent_message\":\"ok\"}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Hello title test"), monitor.last_completed_title());
    CHECK_EQ(std::string("alpha"), monitor.snapshot(false).last_completed_project_name);
    auto events = monitor.take_events();
    const auto completed_snapshot = monitor.snapshot(false);
    const auto completed_it = std::find(events.begin(), events.end(), MonitorEventKind::TaskCompleted);
    CHECK(completed_it != events.end());
    const auto completed_index = static_cast<std::size_t>(std::distance(events.begin(), completed_it));
    CHECK(completed_index < completed_snapshot.event_notifications.size());
    CHECK(completed_snapshot.event_notifications[completed_index].has_value());
    const auto& completed_notification = *completed_snapshot.event_notifications[completed_index];
    CHECK_EQ(TaskNotificationState::Completed, completed_notification.state);
    CHECK_EQ(std::string("alpha"), completed_notification.project_name);
    CHECK_EQ(std::string("Hello title test"), completed_notification.task_title);
    CHECK_EQ(std::string("ok"), completed_notification.summary);
    CHECK_EQ(std::size_t(3), completed_notification.steps.size());

    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:05Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"B\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:06Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Abort title test\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:07Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"B\",\"reason\":\"interrupted\"}}\n");
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Abort title test"), monitor.last_interrupted_title());
    events = monitor.take_events();
    const auto interrupted_snapshot = monitor.snapshot(false);
    const auto interrupted_it = std::find(events.begin(), events.end(), MonitorEventKind::TaskInterrupted);
    CHECK(interrupted_it != events.end());
    const auto interrupted_index = static_cast<std::size_t>(std::distance(events.begin(), interrupted_it));
    CHECK(interrupted_snapshot.event_notifications[interrupted_index].has_value());
    CHECK_EQ(TaskNotificationState::Interrupted,
             interrupted_snapshot.event_notifications[interrupted_index]->state);
    CHECK_EQ(std::string("interrupted"),
             interrupted_snapshot.event_notifications[interrupted_index]->summary);
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
    const auto error_snapshot = monitor.snapshot(false);
    const auto error_it = std::find(events.begin(), events.end(), MonitorEventKind::TaskAborted);
    CHECK(error_it != events.end());
    const auto error_index = static_cast<std::size_t>(std::distance(events.begin(), error_it));
    CHECK(error_snapshot.event_notifications[error_index].has_value());
    CHECK(error_snapshot.event_notifications[error_index]->summary.find(
        "tool_calls") != std::string::npos);

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

void test_pending_tool_call_liveness_and_new_turn_supersedes_old() {
    TempDirectory root("CodeXPetsPendingTool_");
    const auto pending_timestamp = utc_timestamp(SystemClock::now() - std::chrono::minutes(20));
    const auto file = create_session(root.path, "rollout-pending-tool.jsonl",
        std::string("{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"OLD\"}}\n") +
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Long running task\"}}\n" +
        "{\"timestamp\":\"" + pending_timestamp +
        "\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"name\":\"wait_agent\",\"arguments\":\"{}\",\"call_id\":\"call-wait\"}}\n");
    std::filesystem::last_write_time(file,
        std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30));

    CodexSessionMonitor monitor(root.path);
    CHECK_EQ(1, monitor.active_count());
    // A stale pending call without a live writer is an interrupted session and
    // must not remain visible as an active task.
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());

    append(file,
        "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call_output\",\"call_id\":\"call-wait\",\"output\":\"done\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:04Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"NEW\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:05Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Replacement task\"}}\n");
    monitor.poll();
    CHECK_EQ(1, monitor.active_count());
    CHECK_EQ(std::string("Replacement task"), monitor.primary_active_title());

    TempDirectory live_pending_root("CodeXPetsLivePendingTool_");
    const auto live_pending_file = create_session(live_pending_root.path, "rollout-live-pending.jsonl",
        std::string("{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"LIVE\"}}\n") +
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Live pending task\"}}\n" +
        "{\"timestamp\":\"" + utc_timestamp(SystemClock::now() - std::chrono::minutes(20)) +
        "\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"name\":\"wait_agent\",\"arguments\":\"{}\",\"call_id\":\"call-live\"}}\n");
    std::filesystem::last_write_time(live_pending_file,
        std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30));
    CodexSessionMonitor live_pending_monitor(live_pending_root.path);
    std::ofstream live_writer(live_pending_file, std::ios::binary | std::ios::app);
    CHECK(live_writer.good());
    live_pending_monitor.poll();
    CHECK_EQ(1, live_pending_monitor.active_count());
    live_writer.close();

    TempDirectory stale_root("CodeXPetsActuallyStale_");
    const auto stale_file = create_session(stale_root.path, "rollout-stale.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"STALE\"}}\n");
    std::filesystem::last_write_time(stale_file,
        std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30));
    CodexSessionMonitor stale_monitor(stale_root.path);
    CHECK_EQ(1, stale_monitor.active_count());
    stale_monitor.poll();
    CHECK_EQ(0, stale_monitor.active_count());

    TempDirectory expired_pending_root("CodeXPetsExpiredPending_");
    const auto expired_timestamp = utc_timestamp(SystemClock::now() - std::chrono::hours(7));
    const auto expired_file = create_session(expired_pending_root.path, "rollout-expired-pending.jsonl",
        std::string("{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"EXPIRED\"}}\n") +
        "{\"timestamp\":\"" + expired_timestamp +
        "\",\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\",\"name\":\"shell_command\",\"arguments\":\"{}\",\"call_id\":\"call-expired\"}}\n");
    std::filesystem::last_write_time(expired_file,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(8));
    CodexSessionMonitor expired_pending_monitor(expired_pending_root.path);
    CHECK_EQ(1, expired_pending_monitor.active_count());
    expired_pending_monitor.poll();
    CHECK_EQ(0, expired_pending_monitor.active_count());
}

void test_long_silent_reasoning_kept_while_session_writer_is_alive() {
    TempDirectory root("CodeXPetsLongReasoning_");
    const auto silent_timestamp = utc_timestamp(SystemClock::now() - std::chrono::minutes(27));
    const auto file = create_session(root.path, "rollout-long-reasoning.jsonl",
        std::string("{\"timestamp\":\"") + silent_timestamp +
        "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"LONG\"}}\n" +
        "{\"timestamp\":\"" + silent_timestamp +
        "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Long silent reasoning\"}}\n" +
        "{\"timestamp\":\"" + silent_timestamp +
        "\",\"type\":\"response_item\",\"payload\":{\"type\":\"reasoning\",\"summary\":[]}}\n");
    const auto old_write = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30);
    std::filesystem::last_write_time(file, old_write);

    CodexSessionMonitor monitor(root.path);
    CHECK_EQ(1, monitor.active_count());
    std::ofstream writer(file, std::ios::binary | std::ios::app);
    CHECK(writer.good());
    std::filesystem::last_write_time(file, old_write);
    monitor.poll();
    CHECK_EQ(1, monitor.active_count());
    CHECK_EQ(std::string("Long silent reasoning"), monitor.primary_active_title());

    writer.close();
    std::filesystem::last_write_time(file, old_write);
    CodexSessionMonitor ended_monitor(root.path);
    CHECK_EQ(1, ended_monitor.active_count());
    ended_monitor.poll();
    CHECK_EQ(0, ended_monitor.active_count());
}

void test_oversized_json_line_does_not_poison_tail() {
    TempDirectory root("CodeXPetsOversizedLine_");
    const auto file = create_session(root.path, "rollout-oversized-line.jsonl",
        "{\"timestamp\":\"2026-08-01T00:00:00Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"BIG\"}}\n"
        "{\"timestamp\":\"2026-08-01T00:00:01Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"user_message\",\"message\":\"Large output\"}}\n");
    CodexSessionMonitor monitor(root.path);
    (void)monitor.take_events();

    std::string oversized = "{\"timestamp\":\"2026-08-01T00:00:02Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"reasoning\",\"summary\":\"";
    oversized.append(300 * 1024, 'x');
    oversized += "\"}}\n";
    oversized += "{\"timestamp\":\"2026-08-01T00:00:03Z\",\"type\":\"event_msg\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"BIG\"}}\n";
    append(file, oversized);
    monitor.poll();
    CHECK_EQ(0, monitor.active_count());
    CHECK_EQ(std::string("Large output"), monitor.last_completed_title());
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
    CHECK(!CodexSessionMonitor::is_turn_stale(now - std::chrono::minutes(20),
                                              now - std::chrono::minutes(20), now, 600, true));
}

void test_telegram_card_and_transport() {
    TaskNotification notification;
    notification.state = TaskNotificationState::Error;
    notification.project_name = "codex-pets";
    notification.task_title = "编译 <Windows> & 发布";
    notification.steps = {
        {"检查代码", TaskStepState::Completed},
        {"编译 Windows x64", TaskStepState::Error},
        {"生成安装包", TaskStepState::Pending},
    };
    notification.summary = "编译器返回 <error> & 任务已停止。";
    const auto card = format_telegram_task_card(notification, SystemClock::from_time_t(0));
    CHECK(card.find("❌ <b>codex-pets · 出现异常</b>") != std::string::npos);
    CHECK(card.find("<b>步骤 · 1 / 3</b>") != std::string::npos);
    CHECK(card.find("✅ 检查代码") != std::string::npos);
    CHECK(card.find("❌ 编译 Windows x64") != std::string::npos);
    CHECK(card.find("&lt;Windows&gt; &amp; 发布") != std::string::npos);
    CHECK(card.find("&lt;error&gt; &amp; 任务已停止") != std::string::npos);

    std::vector<TelegramHttpRequest> requests;
    TelegramNotifier notifier([&](const TelegramHttpRequest& request) {
        requests.push_back(request);
        return TelegramHttpResponse{200, R"({"ok":true,"result":{"message_id":1}})"};
    });
    TelegramSettings settings;
    settings.bot_token = "123456:test-token";
    settings.chat_id = "123456789";
    std::string error;
    CHECK(notifier.test(settings, &error));
    CHECK(error.empty());
    CHECK_EQ(std::size_t(1), requests.size());
    CHECK(requests[0].url.find("/bot123456:test-token/sendMessage") != std::string::npos);
    CHECK(requests[0].body.find("\"parse_mode\":\"HTML\"") != std::string::npos);
    CHECK(requests[0].body.find("123456789") != std::string::npos);
    notifier.stop();

    TelegramNotifier rejected([](const TelegramHttpRequest&) {
        return TelegramHttpResponse{400, R"({"ok":false,"description":"Bad Request: chat not found"})"};
    });
    CHECK(!rejected.test(settings, &error));
    CHECK_EQ(std::string("Bad Request: chat not found"), error);
    rejected.stop();
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
        {"xiaoai_protocol", test_xiaoai_protocol},
        {"xiaoai_authorization_compaction", test_xiaoai_authorization_compaction},
        {"monitor_update_queue_and_policy", test_monitor_update_queue_and_policy},
        {"xiaoai_start_context_tracks_each_task", test_xiaoai_start_context_tracks_each_task},
        {"session_monitor_lifecycle", test_session_monitor_lifecycle},
        {"session_monitor_success_and_order", test_session_monitor_success_and_order},
        {"plan_update_focus_survives_later_event", test_plan_update_focus_survives_later_event},
        {"session_monitor_error_object", test_session_monitor_error_object},
        {"session_monitor_v2_aliases_and_non_fatal_error", test_session_monitor_v2_aliases_and_non_fatal_error},
        {"pending_tool_call_liveness_and_superseded_turn", test_pending_tool_call_liveness_and_new_turn_supersedes_old},
        {"long_silent_reasoning_writer_liveness", test_long_silent_reasoning_kept_while_session_writer_is_alive},
        {"oversized_json_line_tail_recovery", test_oversized_json_line_does_not_poison_tail},
        {"open_file_activity_and_stale_rule", test_open_file_activity_and_stale_rule},
        {"telegram_card_and_transport", test_telegram_card_and_transport},
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
