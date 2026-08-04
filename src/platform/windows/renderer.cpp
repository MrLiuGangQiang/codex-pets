#include "renderer.h"

#include "../../../src/core/app_logic.h"
#include "resource_ids.h"

#include <windows.h>
#include <shlwapi.h>
#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>


namespace codexpets::windows {
namespace {

using namespace Gdiplus;

constexpr float kBubbleWidth = 270.0f;
constexpr float kBubbleHeight = 110.0f;
constexpr float kBubbleInset = 45.0f;
constexpr float kDockBubbleOffset = 48.0f;
constexpr float kDockSize = 104.0f;
constexpr float kPetWidth = 130.0f;
constexpr float kPetHeight = 140.0f;

struct SpriteMetric {
    float anchor_x;
    float bottom;
    float x;
    float y;
    float width;
    float height;
};

constexpr std::array<std::array<SpriteMetric, 8>, 5> kFloatingMetrics{{
    {{{88.268827f,157,43,30,113,127},{88.268827f,157,43,30,113,127},{88.479689f,157,43,30,115,127},{88.335350f,153,42,30,113,123},{87.956078f,158,42,30,114,128},{88.335350f,153,42,30,113,123},{88.479689f,157,43,30,115,127},{88.268827f,157,43,30,113,127}}},
    {{{87.675970f,158,42,30,114,128},{88.439236f,158,43,30,115,128},{88.264264f,156,42,30,113,126},{87.714413f,159,43,30,114,129},{96,208,0,0,192,208},{96,208,0,0,192,208},{96,208,0,0,192,208},{96,208,0,0,192,208}}},
    {{{87.853720f,130,41,35,128,95},{87.853720f,130,41,35,128,95},{87.853720f,130,41,35,128,95},{87.853720f,130,41,35,128,95},{87.564935f,127,39,28,134,99},{87.748331f,160,44,28,113,132},{87.691131f,160,44,28,113,132},{88.078308f,129,35,28,128,101}}},
    {{{87.944566f,145,39,31,120,114},{87.944566f,145,39,31,120,114},{87.923949f,144,39,31,118,113},{87.540754f,143,39,30,116,113},{88.265314f,145,40,31,117,114},{87.540754f,143,39,30,116,113},{87.923949f,144,39,31,118,113},{87.944566f,145,39,31,120,114}}},
    {{{88.268827f,157,43,30,113,127},{88.268827f,157,43,30,113,127},{88.479689f,157,43,30,115,127},{88.335350f,153,42,30,113,123},{87.956078f,158,42,30,114,128},{88.335350f,153,42,30,113,123},{88.479689f,157,43,30,115,127},{88.268827f,157,43,30,113,127}}},
}};

struct SimpleRect { float x; float y; float width; float height; };
constexpr std::array<SimpleRect, 20> kDockOpaque{{
    {0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},
    {0,12,167,232},{0,12,167,232},{0,12,167,232},{0,12,167,232},
    {89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232},
    {89,12,167,232},{89,12,167,232},{89,12,167,232},{89,12,167,232}
}};

RectF bubble_bounds(const RenderState& state) {
    float x = (Renderer::LogicalWidth - kBubbleWidth) / 2.0f;
    if (state.docked) x += state.dock_edge == DockEdge::Left ? -kDockBubbleOffset : kDockBubbleOffset;
    const float y = state.bubble_below
        ? Renderer::LogicalHeight - kBubbleHeight - kBubbleInset - (state.docked ? 2.0f : 0.0f)
        : kBubbleInset + (state.docked ? 6.0f : 0.0f);
    return RectF(x, y, kBubbleWidth, kBubbleHeight);
}

RectF visible_cloud_bounds(const RenderState& state) {
    const auto cloud = bubble_bounds(state);
    return RectF(cloud.X + 52.0f / 640.0f * cloud.Width,
                 cloud.Y + 4.0f / 221.0f * cloud.Height,
                 536.0f / 640.0f * cloud.Width,
                 193.0f / 221.0f * cloud.Height);
}

RectF floating_destination(const RenderState& state, int row, int frame) {
    const auto& metric = kFloatingMetrics[static_cast<std::size_t>(std::clamp(row, 0, 4))]
                                          [static_cast<std::size_t>(std::clamp(frame, 0, 7))];
    constexpr float scale_x = kPetWidth / 192.0f;
    constexpr float scale_y = kPetHeight / 208.0f;
    const float anchor = state.mirror ? 192.0f - metric.anchor_x : metric.anchor_x;
    return RectF(Renderer::LogicalWidth / 2.0f - anchor * scale_x,
                 Renderer::LogicalHeight - 3.0f - metric.bottom * scale_y,
                 kPetWidth, kPetHeight);
}

RectF visible_pet_bounds(const RenderState& state) {
    if (state.docked) {
        const int frame = app_logic::select_dock_sprite_index(state.dock_edge, state.state,
                                                               state.animation_tick);
        const auto& opaque = kDockOpaque[static_cast<std::size_t>(std::clamp(frame, 0, 19))];
        const float visibility = std::clamp(static_cast<float>(state.dock_visibility), 0.0f, 1.0f);
        const float shown = visibility * visibility * (3.0f - 2.0f * visibility);
        const float hidden = kDockSize * (1.0f - shown);
        const float x = state.dock_edge == DockEdge::Left ? -hidden
                                                          : Renderer::LogicalWidth - kDockSize + hidden;
        const float y = state.bubble_below ? 4.0f : Renderer::LogicalHeight - kDockSize - 7.0f;
        constexpr float scale = kDockSize / 256.0f;
        return RectF(x + opaque.x * scale, y + opaque.y * scale,
                     opaque.width * scale, opaque.height * scale);
    }
    const int row = app_logic::select_floating_sprite_row(state.state);
    const int frame = app_logic::select_floating_frame(state.state, state.animation_tick);
    const auto& metric = kFloatingMetrics[static_cast<std::size_t>(std::clamp(row, 0, 4))]
                                          [static_cast<std::size_t>(std::clamp(frame, 0, 7))];
    const auto destination = floating_destination(state, row, frame);
    constexpr float scale_x = kPetWidth / 192.0f;
    constexpr float scale_y = kPetHeight / 208.0f;
    const float opaque_x = state.mirror ? 192.0f - (metric.x + metric.width) : metric.x;
    return RectF(destination.X + opaque_x * scale_x, destination.Y + metric.y * scale_y,
                 metric.width * scale_x, metric.height * scale_y);
}

Color header_color(ReminderState state) {
    switch (state) {
        case ReminderState::Busy: return Color(255, 43, 105, 168);
        case ReminderState::Completed: return Color(255, 43, 139, 87);
        case ReminderState::Error: return Color(255, 194, 57, 52);
        default: return Color(255, 67, 105, 139);
    }
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

float smooth_step(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return static_cast<float>(value * value * (3.0 - 2.0 * value));
}

std::string normalized_text(std::string value) {
    std::string result;
    result.reserve(value.size());
    bool whitespace = false;
    for (const auto ch : value) {
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\t' || ch == ' ') {
            whitespace = true;
            continue;
        }
        if (whitespace && !result.empty()) result.push_back(' ');
        whitespace = false;
        result.push_back(ch);
    }
    return result;
}

std::string selected_body(const RenderState& state) {
    if (!state.task_titles.empty() && state.selected_task_index >= 0 &&
        state.selected_task_index < static_cast<int>(state.task_titles.size())) {
        return state.task_titles[static_cast<std::size_t>(state.selected_task_index)];
    }
    return state.thought_text;
}

int float_resource_id(int row, int frame) {
    return 1000 + row * 8 + frame;
}

int tray_resource_id(ReminderState state, int frame) {
    if (state == ReminderState::Completed) return IDR_ICON_STATUS_COMPLETED;
    if (state == ReminderState::Error) return IDR_ICON_STATUS_ERROR;
    if (state == ReminderState::Busy) return IDR_ICON_STATUS_BUSY_0 + frame;
    return IDR_ICON_STATUS_IDLE;
}

int audio_resource_id(NotificationSound sound) {
    switch (sound) {
        case NotificationSound::Completed: return IDR_AUDIO_VOICE_COMPLETE;
        case NotificationSound::Error: return IDR_AUDIO_VOICE_ERROR;
        default: return IDR_AUDIO_VOICE_START;
    }
}

void fill_octagon(Graphics& graphics, const RectF& rect, float notch, Color color) {
    PointF points[] = {{rect.X+notch,rect.Y},{rect.GetRight()-notch,rect.Y},{rect.GetRight(),rect.Y+notch},{rect.GetRight(),rect.GetBottom()-notch},{rect.GetRight()-notch,rect.GetBottom()},{rect.X+notch,rect.GetBottom()},{rect.X,rect.GetBottom()-notch},{rect.X,rect.Y+notch}};
    SolidBrush brush(color); graphics.FillPolygon(&brush, points, 8);
}
void draw_thought_dot(Graphics& graphics, const RectF& rect) {
    fill_octagon(graphics, rect, 3.0f, Color(255,42,50,60));
    if(rect.Width>6&&rect.Height>6) fill_octagon(graphics,RectF(rect.X+3,rect.Y+3,rect.Width-6,rect.Height-6),1.0f,Color(255,241,248,255));
}

} // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(HINSTANCE instance, std::string* error) {
    instance_ = instance;
    GdiplusStartupInput input;
    if (GdiplusStartup(&gdiplus_token_, &input, nullptr) != Ok) {
        if (error) *error = "GDI+ 初始化失败";
        return false;
    }
    application_icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_CODEXPETS_ICON),
                                                        IMAGE_ICON, 32, 32, LR_DEFAULTSIZE));
    if (!application_icon_) {
        if (error) *error = "应用图标加载失败";
        return false;
    }
    cloud_bitmap_ = load_bitmap(IDR_CLOUD_BUBBLE);
    if (!cloud_bitmap_) {
        if (error) *error = "云朵资源加载失败";
        return false;
    }
    {
        // Keep a compact 2x working copy resident instead of the full 2122x734
        // source bitmap (~6 MB decoded) to minimize memory without losing detail.
        BitmapPtr scaled(new Bitmap(540, 220, PixelFormat32bppPARGB));
        if (scaled && scaled->GetLastStatus() == Ok) {
            Graphics graphics(scaled.get());
            graphics.SetCompositingMode(CompositingModeSourceCopy);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            graphics.DrawImage(cloud_bitmap_.get(), RectF(0, 0, 540.0f, 220.0f));
            cloud_bitmap_ = std::move(scaled);
        }
    }
    return true;
}

void Renderer::shutdown() noexcept {
    for (auto& icon : tray_icons_) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
    if (application_icon_) DestroyIcon(application_icon_);
    application_icon_ = nullptr;
    cloud_bitmap_.reset();
    for (auto& bitmap : dock_cache_) bitmap.reset();
    dock_cache_side_ = -1;
    for (auto& bitmap : floating_cache_) bitmap.reset();
    floating_cache_row_ = -1;
    if (previous_bitmap_ && memory_dc_) SelectObject(memory_dc_, previous_bitmap_);
    if (memory_bitmap_) DeleteObject(memory_bitmap_);
    if (memory_dc_) DeleteDC(memory_dc_);
    memory_bitmap_ = nullptr;
    memory_dc_ = nullptr;
    previous_bitmap_ = nullptr;
    dib_bits_ = nullptr;
    if (gdiplus_token_) GdiplusShutdown(gdiplus_token_);
    gdiplus_token_ = 0;
}

Renderer::BitmapPtr Renderer::load_bitmap(int resource_id) {
    if (!instance_) return {};
    const auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) return {};
    const auto size = SizeofResource(instance_, resource);
    const auto loaded = LoadResource(instance_, resource);
    if (!loaded || size == 0) return {};
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return {};
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return {}; }
    std::memcpy(target, LockResource(loaded), size);
    GlobalUnlock(memory);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(memory, TRUE, &stream) != S_OK) {
        GlobalFree(memory);
        return {};
    }
    auto* source = Bitmap::FromStream(stream, FALSE);
    if (!source || source->GetLastStatus() != Ok) {
        delete source;
        stream->Release();
        return {};
    }
    auto result = BitmapPtr(new Bitmap(source->GetWidth(), source->GetHeight(), PixelFormat32bppPARGB));
    if (!result || result->GetLastStatus() != Ok) {
        delete source;
        stream->Release();
        return {};
    }
    Graphics graphics(result.get());
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.DrawImage(source, 0, 0, source->GetWidth(), source->GetHeight());
    delete source;
    stream->Release();
    return result;
}

HICON Renderer::load_icon(int resource_id) {
    auto bitmap = load_bitmap(resource_id);
    if (!bitmap) return nullptr;
    HICON icon = nullptr;
    if (bitmap->GetHICON(&icon) != Ok) return nullptr;
    return icon;
}

void Renderer::clear_floating_cache(int row) {
    if (floating_cache_row_ == row) return;
    for (auto& bitmap : floating_cache_) bitmap.reset();
    floating_cache_row_ = row;
}

Renderer::BitmapPtr& Renderer::floating_bitmap(int row, int frame) {
    clear_floating_cache(row);
    auto& result = floating_cache_[static_cast<std::size_t>(std::clamp(frame, 0, 7))];
    if (!result) result = load_bitmap(float_resource_id(row, frame));
    return result;
}

Renderer::BitmapPtr& Renderer::dock_bitmap(int frame) {
    frame = std::clamp(frame, 0, 19);
    const int side = frame >= 10 ? 1 : 0;
    if (dock_cache_side_ != side) {
        for (auto& bitmap : dock_cache_) bitmap.reset();
        dock_cache_side_ = side;
    }
    auto& result = dock_cache_[static_cast<std::size_t>(frame)];
    if (!result) result = load_bitmap(IDR_DOCK_0 + frame);
    return result;
}

bool Renderer::ensure_surface(int width, int height, std::string* error) {
    if (memory_dc_ && pixel_width_ == width && pixel_height_ == height) return true;
    if (previous_bitmap_ && memory_dc_) SelectObject(memory_dc_, previous_bitmap_);
    if (memory_bitmap_) DeleteObject(memory_bitmap_);
    if (memory_dc_) DeleteDC(memory_dc_);
    memory_dc_ = CreateCompatibleDC(nullptr);
    if (!memory_dc_) { if (error) *error = "创建渲染 DC 失败"; return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    memory_bitmap_ = CreateDIBSection(memory_dc_, &info, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
    if (!memory_bitmap_) {
        DeleteDC(memory_dc_); memory_dc_ = nullptr;
        if (error) *error = "创建透明位图失败";
        return false;
    }
    previous_bitmap_ = static_cast<HBITMAP>(SelectObject(memory_dc_, memory_bitmap_));
    pixel_width_ = width;
    pixel_height_ = height;
    return true;
}

bool Renderer::render(const RenderState& state, double scale, std::string* error) {
    if (!gdiplus_token_) { if (error) *error = "渲染器未初始化"; return false; }
    scale = std::clamp(scale, 0.5, 4.0);
    const auto width = std::max(1, static_cast<int>(std::lround(LogicalWidth * scale)));
    const auto height = std::max(1, static_cast<int>(std::lround(LogicalHeight * scale)));
    if (!ensure_surface(width, height, error)) return false;
    scale_ = scale;
    Graphics graphics(memory_dc_);
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    graphics.ScaleTransform(static_cast<REAL>(scale), static_cast<REAL>(scale));
    draw_scene(graphics, state, scale);
    graphics.Flush(FlushIntentionSync);
    return true;
}

void Renderer::draw_scene(Graphics& graphics, const RenderState& state, double scale) {
    if (state.bubble_visible) draw_cloud(graphics, state, scale);
    draw_pet(graphics, state, scale);
}

void Renderer::draw_cloud(Graphics& graphics, const RenderState& state, double /*scale*/) {
    const auto cloud = bubble_bounds(state);
    const auto visible_cloud = visible_cloud_bounds(state);
    if (cloud_bitmap_) {
        const auto previous_interpolation = graphics.GetInterpolationMode();
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.DrawImage(cloud_bitmap_.get(), cloud);
        graphics.SetInterpolationMode(previous_interpolation);
    }

    constexpr float large_w = 17.0f, large_h = 15.0f, small_w = 11.0f, small_h = 10.0f;
    RectF large_dot, small_dot;
    const auto pet = visible_pet_bounds(state);
    if (state.docked) {
        const float inset = std::min(38.0f, visible_cloud.Width * 0.18f);
        const float cloud_x = state.dock_edge == DockEdge::Left
            ? visible_cloud.X + inset : visible_cloud.GetRight() - inset;
        const float inward = state.dock_edge == DockEdge::Left ? 1.0f : -1.0f;
        const PointF cloud_anchor(cloud_x, state.bubble_below ? visible_cloud.Y - 1.0f
                                                              : visible_cloud.GetBottom() + 1.0f);
        const PointF pet_anchor(pet.X + pet.Width / 2.0f + inward * 2.0f,
                                state.bubble_below ? pet.GetBottom() + 1.0f : pet.Y - 1.0f);
        const float dx = pet_anchor.X - cloud_anchor.X, dy = pet_anchor.Y - cloud_anchor.Y;
        const float distance = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
        const float ux = dx / distance, uy = dy / distance;
        const float large_radius = (std::abs(ux) * large_w + std::abs(uy) * large_h) / 2.0f;
        const float small_radius = (std::abs(ux) * small_w + std::abs(uy) * small_h) / 2.0f;
        const float gap = std::max(1.0f, (distance - 2 * large_radius - 2 * small_radius) / 3.0f);
        float large_d = gap + large_radius;
        float small_d = large_d + large_radius + gap + small_radius;
        if (small_d + small_radius > distance) { large_d = distance / 3.0f; small_d = distance * 2.0f / 3.0f; }
        const float perpendicular = state.dock_edge == DockEdge::Left
            ? (state.bubble_below ? 8.0f : -8.0f) : (state.bubble_below ? -8.0f : 8.0f);
        const PointF lc(cloud_anchor.X + ux * large_d, cloud_anchor.Y + uy * large_d);
        const PointF sc(cloud_anchor.X + ux * small_d - uy * perpendicular,
                        cloud_anchor.Y + uy * small_d + ux * perpendicular);
        large_dot = RectF(lc.X-large_w/2,lc.Y-large_h/2,large_w,large_h);
        small_dot = RectF(sc.X-small_w/2,sc.Y-small_h/2,small_w,small_h);
    } else {
        const float direction = state.mirror ? 1.0f : -1.0f;
        const float small_x = cloud.X + cloud.Width / 2.0f + direction * 11.0f;
        const float large_x = cloud.X + cloud.Width / 2.0f + direction * 7.0f;
        float small_y{}, large_y{};
        if (state.bubble_below) {
            small_y = pet.GetBottom() + 2.0f;
            large_y = std::min(visible_cloud.Y - large_h - 3.0f, small_y + small_h + 3.0f);
        } else {
            const float desired = pet.Y - small_h - 0.5f;
            large_y = std::clamp(desired - large_h - 3.0f,
                                 visible_cloud.GetBottom() + 3.0f,
                                 visible_cloud.GetBottom() + 12.0f);
            small_y = std::max(desired, large_y + large_h + 1.0f);
        }
        large_dot = RectF(large_x-large_w/2,large_y,large_w,large_h);
        small_dot = RectF(small_x-small_w/2,small_y,small_w,small_h);
    }
    draw_thought_dot(graphics, large_dot);
    draw_thought_dot(graphics, small_dot);

    const float origin_x = std::round(visible_cloud.X + visible_cloud.Width * 0.10f);
    const float origin_y = std::round(visible_cloud.Y + visible_cloud.Height * 0.32f);
    const Color glow = state.state == ReminderState::Error ? Color(255,226,62,55) : Color(255,83,169,236);
    const Color highlight = state.state == ReminderState::Error ? Color(255,255,174,154) : Color(255,202,232,255);
    SolidBrush outline(Color(255,68,43,25)), glow_brush(glow), highlight_brush(highlight), base(Color(255,91,78,70));
    auto cell=[&](SolidBrush& b,int x,int y,int w,int h){graphics.FillRectangle(&b,origin_x+x*2.5f,origin_y+y*2.5f,w*2.5f,h*2.5f);};
    cell(outline,6,0,1,2);cell(outline,2,2,1,1);cell(outline,10,2,1,1);cell(outline,0,6,2,1);cell(outline,11,6,2,1);
    for(const auto& row:std::array<std::array<int,3>,11>{{{3,4,5},{4,3,7},{5,2,9},{6,2,9},{7,2,9},{8,3,7},{9,4,5},{10,5,3},{11,4,5},{12,4,5},{13,5,3}}})cell(outline,row[1],row[0],row[2],1);
    cell(glow_brush,4,4,5,1);cell(glow_brush,3,5,7,3);cell(glow_brush,4,8,5,1);cell(glow_brush,5,9,3,1);cell(highlight_brush,4,4,2,1);cell(highlight_brush,3,5,2,2);cell(outline,5,7,1,2);cell(outline,7,7,1,2);cell(outline,6,9,1,1);cell(base,5,11,3,2);cell(outline,5,13,3,1);
    draw_text(graphics, state, 1.0);
}

void Renderer::draw_text(Graphics& graphics, const RenderState& state, double /*scale*/) {
    const auto cloud = bubble_bounds(state);
    const auto selected_index = std::clamp(state.selected_task_index, 0, std::max(0, static_cast<int>(state.progress_labels.size()) - 1));
    std::optional<std::string_view> progress;
    if (state.state == ReminderState::Busy && selected_index < static_cast<int>(state.progress_labels.size()) && state.progress_labels[static_cast<std::size_t>(selected_index)]) progress = *state.progress_labels[static_cast<std::size_t>(selected_index)];
    const auto header = state.state == ReminderState::Busy ? app_logic::format_busy_header(progress, state.selected_task_index, static_cast<int>(state.task_titles.size())) : state.status_text;
    const auto body = normalized_text(selected_body(state));
    FontFamily family(L"Segoe UI"); Font header_font(&family,12.5f,FontStyleBold,UnitPixel); Font body_font(&family,11.5f,FontStyleRegular,UnitPixel);
    SolidBrush header_brush(header_color(state.state)), body_brush(Color(255,45,60,78));
    StringFormat hf;hf.SetAlignment(StringAlignmentCenter);hf.SetLineAlignment(StringAlignmentCenter);
    const RectF hr(cloud.X+cloud.Width*.26f,cloud.Y+cloud.Height*.10f,cloud.Width*.52f,cloud.Height*.20f);
    const auto hw=wide(header);graphics.DrawString(hw.c_str(),-1,&header_font,hr,&hf,&header_brush);
    StringFormat bf;bf.SetAlignment(StringAlignmentNear);bf.SetLineAlignment(StringAlignmentNear);bf.SetTrimming(StringTrimmingNone);bf.SetFormatFlags(StringFormatFlagsLineLimit);
    const RectF br(cloud.X+cloud.Width*.30f,cloud.Y+cloud.Height*.34f,156,45);Region old;graphics.GetClip(&old);graphics.SetClip(br,CombineModeIntersect);
    const auto bw=wide(body);graphics.DrawString(bw.c_str(),-1,&body_font,RectF(br.X,br.Y-static_cast<REAL>(state.scroll_offset),br.Width,1000),&bf,&body_brush);graphics.SetClip(&old);
}

void Renderer::draw_pet(Graphics& graphics, const RenderState& state, double /*scale*/) {
    const auto previous_interpolation = graphics.GetInterpolationMode();
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    if (state.docked) {
        const auto index=app_logic::select_dock_sprite_index(state.dock_edge,state.state,state.animation_tick);auto& bitmap=dock_bitmap(index);
        if(!bitmap){graphics.SetInterpolationMode(previous_interpolation);return;}
        const float shown=smooth_step(state.dock_visibility),hidden=kDockSize*(1-shown);const float x=state.dock_edge==DockEdge::Left?-hidden:LogicalWidth-kDockSize+hidden;const float y=state.bubble_below?4.0f:LogicalHeight-kDockSize-7.0f;
        graphics.DrawImage(bitmap.get(),RectF(x,y,kDockSize,kDockSize));
        graphics.SetInterpolationMode(previous_interpolation);
        return;
    }
    const auto row=app_logic::select_floating_sprite_row(state.state);const auto frame=app_logic::select_floating_frame(state.state,state.animation_tick);auto& bitmap=floating_bitmap(row,frame);
    if(!bitmap){graphics.SetInterpolationMode(previous_interpolation);return;}
    const auto dest=floating_destination(state,row,frame);
    if(state.mirror){graphics.TranslateTransform(dest.X+dest.Width/2,0);graphics.ScaleTransform(-1,1);graphics.TranslateTransform(-(dest.X+dest.Width/2),0);}
    graphics.DrawImage(bitmap.get(),dest);
    if(state.mirror){graphics.TranslateTransform(dest.X+dest.Width/2,0);graphics.ScaleTransform(-1,1);graphics.TranslateTransform(-(dest.X+dest.Width/2),0);}
    graphics.SetInterpolationMode(previous_interpolation);
}
int Renderer::png_encoder_clsid(CLSID* clsid) {
    UINT count{}, bytes{};
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return -1;
    std::vector<std::uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, bytes, encoders) != Ok) return -1;
    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Renderer::save_png(const std::filesystem::path& path, std::string* error) {
    CLSID clsid{};
    if (png_encoder_clsid(&clsid) < 0) {
        if (error) *error = "找不到 PNG 编码器";
        return false;
    }
    auto* bitmap = Bitmap::FromHBITMAP(memory_bitmap_, nullptr);
    if (!bitmap || bitmap->GetLastStatus() != Ok) {
        delete bitmap;
        if (error) *error = "无法创建预览位图";
        return false;
    }
    std::filesystem::create_directories(path.parent_path());
    const auto status = bitmap->Save(path.c_str(), &clsid, nullptr);
    delete bitmap;
    if (status != Ok) {
        if (error) *error = "保存 PNG 预览失败";
        return false;
    }
    return true;
}

bool Renderer::save_preview(const std::filesystem::path& output, const RenderState& state,
                            double scale, std::string* error) {
    if (!render(state, scale, error)) return false;
    return save_png(output, error);
}

bool Renderer::validate(std::string* error) {
    const std::array<std::pair<int, std::pair<UINT, UINT>>, 4> expected{{
        {IDR_FLOAT_IDLE_0, {192, 208}}, {IDR_FLOAT_COMPLETED_0, {192, 208}},
        {IDR_FLOAT_BUSY_0, {192, 208}}, {IDR_DOCK_0, {256, 256}}
    }};
    for (const auto& [id, dimensions] : expected) {
        auto bitmap = load_bitmap(id);
        if (!bitmap || bitmap->GetWidth() != dimensions.first || bitmap->GetHeight() != dimensions.second) {
            if (error) *error = "资源尺寸校验失败";
            return false;
        }
    }
    auto cloud = load_bitmap(IDR_CLOUD_BUBBLE);
    if (!cloud || cloud->GetWidth() != 2122 || cloud->GetHeight() != 734) {
        if (error) *error = "云朵资源缺失或尺寸错误";
        return false;
    }
    for (int row = 0; row < 5; ++row) {
        for (int frame = 0; frame < 8; ++frame) {
            if (!load_bitmap(float_resource_id(row, frame))) {
                if (error) *error = "浮动精灵资源缺失";
                return false;
            }
        }
    }
    for (int frame = 0; frame < 20; ++frame) {
        if (!load_bitmap(IDR_DOCK_0 + frame)) {
            if (error) *error = "扒边精灵资源缺失";
            return false;
        }
    }
    const std::array<int, 11> icon_ids{{
        IDR_ICON_STATUS_IDLE, IDR_ICON_STATUS_COMPLETED, IDR_ICON_STATUS_ERROR,
        IDR_ICON_STATUS_BUSY_0, IDR_ICON_STATUS_BUSY_1, IDR_ICON_STATUS_BUSY_2,
        IDR_ICON_STATUS_BUSY_3, IDR_ICON_STATUS_BUSY_4, IDR_ICON_STATUS_BUSY_5,
        IDR_ICON_STATUS_BUSY_6, IDR_ICON_STATUS_BUSY_7}};
    for (const auto id : icon_ids) {
        auto bitmap = load_bitmap(id);
        if (!bitmap || bitmap->GetWidth() != 64 || bitmap->GetHeight() != 64) {
            if (error) *error = "状态图标资源缺失或尺寸错误";
            return false;
        }
    }
    for (const auto sound : {NotificationSound::Started, NotificationSound::Completed, NotificationSound::Error}) {
        const auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(audio_resource_id(sound)), RT_RCDATA);
        if (!resource || SizeofResource(instance_, resource) == 0) {
            if (error) *error = "语音资源缺失";
            return false;
        }
    }
    return true;
}

HICON Renderer::tray_icon(ReminderState state, int frame) {
    const int normalized_frame = std::abs(frame) % 8;
    int index = 0;
    if (state == ReminderState::Completed) index = 1;
    else if (state == ReminderState::Error) index = 2;
    else if (state == ReminderState::Busy) index = 3 + normalized_frame;
    if (!tray_icon_loaded_[static_cast<std::size_t>(index)]) {
        tray_icons_[static_cast<std::size_t>(index)] = load_icon(
            tray_resource_id(state, normalized_frame));
        tray_icon_loaded_[static_cast<std::size_t>(index)] = true;
    }
    return tray_icons_[static_cast<std::size_t>(index)];
}

bool Renderer::extract_audio(NotificationSound sound, const std::filesystem::path& destination,
                             std::string* error) {
    const auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(audio_resource_id(sound)), RT_RCDATA);
    if (!resource) { if (error) *error = "语音资源不存在"; return false; }
    const auto size = SizeofResource(instance_, resource);
    const auto data = LockResource(LoadResource(instance_, resource));
    if (!data || size == 0) { if (error) *error = "语音资源读取失败"; return false; }
    try {
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
        if (!stream) { if (error) *error = "无法创建语音缓存"; return false; }
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(stream);
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool Renderer::hit_test_alpha(int x, int y, std::uint8_t threshold) const noexcept {
    if (!dib_bits_ || x < 0 || y < 0 || x >= pixel_width_ || y >= pixel_height_) return false;
    const auto* pixels = static_cast<const std::uint8_t*>(dib_bits_);
    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(pixel_width_) +
                         static_cast<std::size_t>(x)) * 4;
    return pixels[offset + 3] >= threshold;
}

} // namespace codexpets::windows
