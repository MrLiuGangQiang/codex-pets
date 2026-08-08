#pragma once

#include "renderer.h"

#include "monitor_update_queue.h"
#include "session_monitor.h"
#include "settings.h"
#include "visual_state.h"
#include "xiaomi_speaker.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace codexpets::windows {

class NativeApp {
public:
    NativeApp(HINSTANCE instance, std::vector<std::wstring> arguments);
    ~NativeApp();
    NativeApp(const NativeApp&) = delete;
    NativeApp& operator=(const NativeApp&) = delete;

    int run();
    static int run_utility(HINSTANCE instance, const std::vector<std::wstring>& arguments);

private:
    struct ScreenInfo {
        HMONITOR monitor{};
        RECT work{};
        std::wstring identifier;
        UINT dpi{96};
    };

    bool initialize(std::string* error);
    void shutdown() noexcept;
    bool create_pet_window(std::string* error);
    bool create_message_window(std::string* error);
    void create_tray_icon();
    void remove_tray_icon() noexcept;
    void start_monitor();
    void process_monitor_updates();
    void show_expression_demo_state(int index, Clock::time_point now);
    void handle_monitor_update(const PendingMonitorUpdate& update);
    void refresh_visual(bool force_text);
    void on_timer();
    void render_and_present();
    void update_tray_icon();
    void update_window_position();
    void place_default();
    void restore_saved_position();
    void save_position();
    void clamp_to_work_area();
    void try_snap_or_clamp(POINT cursor);
    void reveal_dock_for_interaction();
    bool dock_hovering(POINT cursor) const;
    [[nodiscard]] std::optional<RectD> dock_hover_rect() const;
    bool dock_hover_path_crosses(POINT from, POINT to) const;
    bool update_dock_visibility(double elapsed_seconds, bool hovering);
    void begin_drag(POINT cursor);
    void move_drag(POINT cursor);
    void finish_drag(POINT cursor);
    void show_pet(bool visible);
    void toggle_sound();
    void toggle_xiaoai();
    void toggle_startup();
    void open_sessions_folder();
    void open_latest_release();
    void show_settings();
    void exit_application();
    void save_settings();
    void play_sound(NotificationSound sound);
    void notify_xiaoai(XiaoAiEvent event, std::string_view title = {});
    void open_xiaomi_login();
    void set_xiaoai_controls_enabled(bool enabled);
    void scan_xiaoai_devices(bool suppress_errors);
    void populate_xiaoai_device_selector(HWND hwnd);
    void update_xiaoai_device_summary(HWND hwnd);
    [[nodiscard]] std::vector<std::string> selected_xiaoai_device_ids(HWND hwnd) const;
    void test_xiaoai();
    bool is_autostart_enabled() const;
    void set_autostart_enabled(bool enabled);
    std::wstring audio_path(NotificationSound sound);
    void refresh_audio_cache(const std::filesystem::path& directory);
    ScreenInfo screen_from_point(POINT point) const;
    ScreenInfo screen_for_identifier(std::wstring_view identifier) const;
    [[nodiscard]] ScreenInfo dock_screen() const;
    std::wstring screen_identifier(HMONITOR monitor, const MONITORINFOEXW& info) const;
    POINT cursor_position() const;
    void update_menu_checks(HMENU menu);
    HMENU build_menu(bool context_menu);
    void show_error(std::wstring title, std::wstring text);
    static LRESULT CALLBACK pet_window_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK message_window_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK settings_window_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT pet_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT message_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT settings_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static std::wstring to_wide(std::string_view value);
    static std::string to_utf8(std::wstring_view value);
    static void write_stdout(std::string_view value);

    HINSTANCE instance_{};
    std::vector<std::wstring> arguments_;
    HWND pet_window_{};
    HWND message_window_{};
    HWND settings_window_{};
    HANDLE instance_mutex_{};
    UINT timer_id_{1};
    NOTIFYICONDATAW tray_{};
    bool tray_added_{};
    ReminderState tray_state_{ReminderState::Idle};
    int tray_frame_{-1};
    std::wstring tray_tip_;
    bool initialized_{};
    bool duplicate_instance_{};
    bool expression_demo_{};
    int expression_demo_index_{-1};
    Clock::time_point expression_demo_next_{Clock::time_point::min()};
    bool shutting_down_{};
    bool shutdown_complete_{};
    bool dragging_{};
    bool drag_pending_{};
    bool drag_started_docked_{};
    POINT drag_start_cursor_{};
    POINT drag_start_position_{};
    POINT last_cursor_{};
    POINT last_hover_cursor_{};
    bool has_last_hover_cursor_{};
    DockEdge dock_edge_{DockEdge::None};
    std::wstring dock_screen_identifier_;
    mutable std::wstring dock_screen_cache_key_;
    mutable ScreenInfo dock_screen_cache_{};
    int dock_coordinate_{};
    double dock_visibility_{1.0};
    Clock::time_point dock_last_content_change_{Clock::now()};
    Clock::time_point dock_thought_until_{Clock::time_point::min()};
    Clock::time_point dock_hover_reveal_until_{Clock::time_point::min()};
    Clock::time_point last_tick_{Clock::now()};
    int animation_tick_{};
    double animation_accumulator_{};
    double scroll_offset_{};
    double scroll_hold_seconds_{1.9};
    bool scroll_at_end_{};
    double session_rotation_seconds_{};
    int selected_task_index_{};
    std::vector<std::string> displayed_task_titles_;
    std::vector<std::optional<std::string>> displayed_progress_labels_;
    std::string displayed_thought_text_;
    std::string last_status_signature_;
    std::string last_status_text_;
    std::vector<std::string> last_status_lines_;
    ReminderState last_visual_state_{ReminderState::Idle};
    AppSettings settings_;
    JsonSettingsStore settings_store_;
    Renderer renderer_;
    VisualStateCoordinator visual_coordinator_;
    MonitorSnapshot snapshot_;
    bool has_snapshot_{};
    std::unique_ptr<MonitorWorker> monitor_worker_;
    std::unique_ptr<XiaoAiNotifier> xiaoai_notifier_;
    std::vector<XiaoAiDeviceInfo> xiaoai_devices_;
    bool xiaoai_operation_in_flight_{};
    MonitorUpdateQueue pending_updates_;
    std::uint64_t monitor_generation_{};
    std::atomic_bool monitor_message_posted_{};
};

} // namespace codexpets::windows
