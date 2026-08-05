#include "renderer.h"

#include "../../../src/core/app_logic.h"
#include "../../../src/core/render_layout.h"
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

RectF as_rect(const RectD& value) {
    return RectF(static_cast<REAL>(value.x), static_cast<REAL>(value.y),
                 static_cast<REAL>(value.width), static_cast<REAL>(value.height));
}

render_layout::State layout_state(const RenderState& state) noexcept {
    return {state.state, state.dock_edge, state.docked, state.bubble_below,
            state.mirror, state.dock_visibility, state.animation_tick};
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


bool valid_embedded_loader(HINSTANCE instance) {
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_WEBVIEW2_LOADER), RT_RCDATA);
    if (!resource) return false;
    const auto size = SizeofResource(instance, resource);
    const auto loaded = LoadResource(instance, resource);
    const auto data = loaded ? static_cast<const std::byte*>(LockResource(loaded)) : nullptr;
    if (!data || size < 0x40 || data[0] != std::byte{'M'} || data[1] != std::byte{'Z'}) return false;
    std::uint32_t pe_offset{};
    std::memcpy(&pe_offset, data + 0x3c, sizeof(pe_offset));
    if (pe_offset > size - 6 || std::memcmp(data + pe_offset, "PE\0\0", 4) != 0) return false;
    std::uint16_t machine{};
    std::memcpy(&machine, data + pe_offset + 4, sizeof(machine));
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::uint16_t expected_machine = 0xaa64;
#else
    constexpr std::uint16_t expected_machine = 0x8664;
#endif
    return machine == expected_machine;
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
    if (!cloud_bitmap_ || cloud_bitmap_->GetWidth() != render_layout::cloud_bitmap_width ||
        cloud_bitmap_->GetHeight() != render_layout::cloud_bitmap_height) {
        if (error) *error = "云朵资源加载失败";
        return false;
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
    const auto layout = layout_state(state);
    const auto cloud = as_rect(render_layout::bubble_bounds(layout));
    if (cloud_bitmap_) {
        const auto previous_interpolation = graphics.GetInterpolationMode();
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.DrawImage(cloud_bitmap_.get(), cloud);
        graphics.SetInterpolationMode(previous_interpolation);
    }

    const auto dots = render_layout::thought_dot_bounds(layout);
    draw_thought_dot(graphics, as_rect(dots.large));
    draw_thought_dot(graphics, as_rect(dots.secondary));

    const auto origin = render_layout::bulb_origin(layout);
    const Color glow = state.state == ReminderState::Error ? Color(255,226,62,55) : Color(255,83,169,236);
    const Color highlight = state.state == ReminderState::Error ? Color(255,255,174,154) : Color(255,202,232,255);
    SolidBrush outline(Color(255,68,43,25)), glow_brush(glow), highlight_brush(highlight), base(Color(255,91,78,70));
    auto cell=[&](SolidBrush& b,int x,int y,int w,int h){
        graphics.FillRectangle(&b, static_cast<REAL>(origin.x + x * 2.5),
                               static_cast<REAL>(origin.y + y * 2.5),
                               static_cast<REAL>(w * 2.5), static_cast<REAL>(h * 2.5));
    };
    cell(outline,6,0,1,2);cell(outline,2,2,1,1);cell(outline,10,2,1,1);cell(outline,0,6,2,1);cell(outline,11,6,2,1);
    for(const auto& row:std::array<std::array<int,3>,11>{{{3,4,5},{4,3,7},{5,2,9},{6,2,9},{7,2,9},{8,3,7},{9,4,5},{10,5,3},{11,4,5},{12,4,5},{13,5,3}}})cell(outline,row[1],row[0],row[2],1);
    cell(glow_brush,4,4,5,1);cell(glow_brush,3,5,7,3);cell(glow_brush,4,8,5,1);cell(glow_brush,5,9,3,1);cell(highlight_brush,4,4,2,1);cell(highlight_brush,3,5,2,2);cell(outline,5,7,1,2);cell(outline,7,7,1,2);cell(outline,6,9,1,1);cell(base,5,11,3,2);cell(outline,5,13,3,1);
    draw_text(graphics, state, 1.0);
}
void Renderer::draw_text(Graphics& graphics, const RenderState& state, double /*scale*/) {
    const auto layout = layout_state(state);
    const auto selected_index = std::clamp(state.selected_task_index, 0, std::max(0, static_cast<int>(state.progress_labels.size()) - 1));
    std::optional<std::string_view> progress;
    if (state.state == ReminderState::Busy && selected_index < static_cast<int>(state.progress_labels.size()) && state.progress_labels[static_cast<std::size_t>(selected_index)]) progress = *state.progress_labels[static_cast<std::size_t>(selected_index)];
    const auto header = state.state == ReminderState::Busy ? app_logic::format_busy_header(progress, state.selected_task_index, static_cast<int>(state.task_titles.size())) : state.status_text;
    const auto body = normalized_text(selected_body(state));
    FontFamily family(L"Segoe UI"); Font header_font(&family,12.5f,FontStyleBold,UnitPixel); Font body_font(&family,11.5f,FontStyleRegular,UnitPixel);
    SolidBrush header_brush(header_color(state.state)), body_brush(Color(255,45,60,78));
    StringFormat hf;hf.SetAlignment(StringAlignmentCenter);hf.SetLineAlignment(StringAlignmentCenter);
    const auto header_rect = as_rect(render_layout::header_bounds(layout));
    const auto hw=wide(header);graphics.DrawString(hw.c_str(),-1,&header_font,header_rect,&hf,&header_brush);
    StringFormat bf;bf.SetAlignment(StringAlignmentNear);bf.SetLineAlignment(StringAlignmentNear);bf.SetTrimming(StringTrimmingNone);bf.SetFormatFlags(StringFormatFlagsLineLimit);
    const auto body_rect = as_rect(render_layout::body_bounds(layout));
    Region old;graphics.GetClip(&old);graphics.SetClip(body_rect,CombineModeIntersect);
    const auto bw=wide(body);graphics.DrawString(bw.c_str(),-1,&body_font,
        RectF(body_rect.X,body_rect.Y-static_cast<REAL>(state.scroll_offset),body_rect.Width,1000),&bf,&body_brush);
    graphics.SetClip(&old);
}
void Renderer::draw_pet(Graphics& graphics, const RenderState& state, double /*scale*/) {
    const auto previous_interpolation = graphics.GetInterpolationMode();
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    if (state.docked) {
        const auto index=app_logic::select_dock_sprite_index(state.dock_edge,state.state,state.animation_tick);auto& bitmap=dock_bitmap(index);
        if(!bitmap){graphics.SetInterpolationMode(previous_interpolation);return;}
        const auto destination = as_rect(render_layout::dock_pet_bounds(layout_state(state)));
        graphics.DrawImage(bitmap.get(), destination);
        graphics.SetInterpolationMode(previous_interpolation);
        return;
    }
    const auto row=app_logic::select_floating_sprite_row(state.state);const auto frame=app_logic::select_floating_frame(state.state,state.animation_tick);auto& bitmap=floating_bitmap(row,frame);
    if(!bitmap){graphics.SetInterpolationMode(previous_interpolation);return;}
    const auto dest=as_rect(render_layout::floating_pet_bounds(layout_state(state)));
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
    if (!dib_bits_ || pixel_width_ <= 0 || pixel_height_ <= 0) {
        if (error) *error = "无法创建预览位图";
        return false;
    }
    // FromHBITMAP drops the alpha channel of a DIB section. Wrap the existing
    // premultiplied BGRA pixels directly so Windows preview artifacts retain
    // the same transparent canvas as the AppKit renderer.
    Bitmap bitmap(pixel_width_, pixel_height_, pixel_width_ * 4, PixelFormat32bppPARGB,
                  static_cast<BYTE*>(dib_bits_));
    if (bitmap.GetLastStatus() != Ok) {
        if (error) *error = "无法创建预览位图";
        return false;
    }
    std::filesystem::create_directories(path.parent_path());
    if (bitmap.Save(path.c_str(), &clsid, nullptr) != Ok) {
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
    if (!valid_embedded_loader(instance_)) {
        if (error) *error = "WebView2 loader 资源缺失或架构错误";
        return false;
    }
    auto cloud = load_bitmap(IDR_CLOUD_BUBBLE);
    if (!cloud || cloud->GetWidth() != render_layout::cloud_bitmap_width ||
        cloud->GetHeight() != render_layout::cloud_bitmap_height) {
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
