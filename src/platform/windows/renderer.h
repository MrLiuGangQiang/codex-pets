#pragma once

#include "render_layout.h"
#include "types.h"

#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace codexpets::windows {

enum class NotificationSound : std::uint8_t { Started, Completed, Error, Interrupted };

struct RenderState {
    ReminderState state{ReminderState::Idle};
    DockEdge dock_edge{DockEdge::None};
    bool docked{};
    bool bubble_visible{true};
    bool bubble_below{};
    bool mirror{};
    double dock_visibility{1.0};
    int animation_tick{};
    int selected_task_index{};
    double scroll_offset{};
    std::string status_text{"空闲"};
    std::string thought_text{"主人，现在没有在进行中的任务!别让我歇着!"};
    std::vector<std::string> task_titles;
    std::vector<std::optional<std::string>> progress_labels;
};

class Renderer {
public:
    static constexpr int LogicalWidth = static_cast<int>(render_layout::logical_width);
    static constexpr int LogicalHeight = static_cast<int>(render_layout::logical_height);

    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(HINSTANCE instance, std::string* error = nullptr);
    void shutdown() noexcept;
    bool render(const RenderState& state, double scale, std::string* error = nullptr);
    bool save_preview(const std::filesystem::path& output, const RenderState& state,
                      double scale = 1.0, std::string* error = nullptr);
    bool validate(std::string* error = nullptr);

    [[nodiscard]] HDC dc() const noexcept { return memory_dc_; }
    [[nodiscard]] HBITMAP bitmap() const noexcept { return memory_bitmap_; }
    [[nodiscard]] int pixel_width() const noexcept { return pixel_width_; }
    [[nodiscard]] int pixel_height() const noexcept { return pixel_height_; }
    [[nodiscard]] bool hit_test_alpha(int x, int y, std::uint8_t threshold = 16) const noexcept;

    HICON tray_icon(ReminderState state, int frame);
    bool extract_audio(NotificationSound sound, const std::filesystem::path& destination,
                       std::string* error = nullptr);
    HICON application_icon() const noexcept { return application_icon_; }

private:
    struct BitmapDeleter { void operator()(Gdiplus::Bitmap* value) const noexcept { delete value; } };
    using BitmapPtr = std::unique_ptr<Gdiplus::Bitmap, BitmapDeleter>;

    BitmapPtr load_bitmap(int resource_id);
    HICON load_icon(int resource_id);
    void clear_floating_cache(int row);
    BitmapPtr& floating_bitmap(int row, int frame);
    BitmapPtr& dock_bitmap(int frame);
    bool ensure_surface(int width, int height, std::string* error);
    bool save_png(const std::filesystem::path& path, std::string* error);
    void draw_scene(Gdiplus::Graphics& graphics, const RenderState& state, double scale);
    void draw_cloud(Gdiplus::Graphics& graphics, const RenderState& state, double scale);
    void draw_pet(Gdiplus::Graphics& graphics, const RenderState& state, double scale);
    void draw_text(Gdiplus::Graphics& graphics, const RenderState& state, double scale);
    static int png_encoder_clsid(CLSID* clsid);

    HINSTANCE instance_{};
    ULONG_PTR gdiplus_token_{};
    HDC memory_dc_{};
    HBITMAP memory_bitmap_{};
    HBITMAP previous_bitmap_{};
    void* dib_bits_{};
    int pixel_width_{};
    int pixel_height_{};
    double scale_{1.0};
    HICON application_icon_{};
    std::unique_ptr<Gdiplus::FontFamily> cloud_font_family_{};
    std::unique_ptr<Gdiplus::FontFamily> body_font_family_{};
    BitmapPtr cloud_bitmap_{};
    std::array<BitmapPtr, 20> dock_cache_{};
    int dock_cache_side_{-1};
    std::array<BitmapPtr, 8> floating_cache_{};
    int floating_cache_row_{-1};
    std::array<HICON, 11> tray_icons_{};
    std::array<bool, 11> tray_icon_loaded_{};
};

} // namespace codexpets::windows
