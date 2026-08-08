#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <ServiceManagement/ServiceManagement.h>

#include <dispatch/dispatch.h>

#include <unistd.h>

#include "app_logic.h"
#include "monitor_policy.h"
#include "monitor_update_queue.h"
#include "paths.h"
#include "platform_text.h"
#include "presentation.h"
#include "render_layout.h"
#include "session_monitor.h"
#include "settings.h"
#include "visual_state.h"
#include "xiaomi_browser_login.h"
#include "xiaomi_credentials.h"
#include "xiaomi_transport.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <sstream>
#include <string>
#include <vector>

using namespace codexpets;

namespace {

NSString* Ns(std::string_view value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data() length:value.size()
                                               encoding:NSUTF8StringEncoding];
    return result ? result : @"";
}

std::string Utf8(NSString* value) {
    if (!value) return {};
    const char* text = value.UTF8String;
    return text ? std::string(text) : std::string{};
}

NSString* ResourcePath(NSString* directory, NSString* name, NSString* extension) {
    return [[NSBundle mainBundle] pathForResource:name ofType:extension inDirectory:directory];
}

BOOL PngResourceHasPixelSize(NSString* directory, NSString* name, NSInteger expectedWidth,
                             NSInteger expectedHeight) {
    @autoreleasepool {
        NSString* path = ResourcePath(directory, name, @"png");
        if (!path) return NO;
        NSURL* url = [NSURL fileURLWithPath:path isDirectory:NO];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (!source) return NO;
        CFDictionaryRef rawProperties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
        CFRelease(source);
        if (!rawProperties) return NO;
        NSNumber* width = (__bridge NSNumber*)CFDictionaryGetValue(
            rawProperties, kCGImagePropertyPixelWidth);
        NSNumber* height = (__bridge NSNumber*)CFDictionaryGetValue(
            rawProperties, kCGImagePropertyPixelHeight);
        const BOOL matches = width && height && width.integerValue == expectedWidth &&
                             height.integerValue == expectedHeight;
        CFRelease(rawProperties);
        return matches;
    }
}

void RemoveCachedImagesWithPrefix(NSMutableDictionary<NSString*, NSImage*>* cache,
                                  NSString* prefix) {
    NSArray<NSString*>* keys = [cache.allKeys copy];
    for (NSString* key in keys) {
        if ([key hasPrefix:prefix]) [cache removeObjectForKey:key];
    }
}

void RemoveCachedImage(NSMutableDictionary<NSString*, NSImage*>* cache, NSString* key) {
    [cache removeObjectForKey:key];
}

std::string StateName(ReminderState state) {
    switch (state) {
        case ReminderState::Busy: return "busy";
        case ReminderState::Completed: return "completed";
        case ReminderState::Error: return "error";
        case ReminderState::Interrupted: return "interrupted";
        default: return "idle";
    }
}

std::string NormalizedText(std::string value) {
    std::string result;
    result.reserve(value.size());
    bool whitespace = false;
    for (const auto ch : value) {
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\t' || ch == ' ') { whitespace = true; continue; }
        if (whitespace && !result.empty()) result.push_back(' ');
        whitespace = false;
        result.push_back(ch);
    }
    return result;
}

struct MacRenderState {
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
    std::vector<std::string> status_lines{"空闲"};
    std::string thought_text{"主人，现在没有在进行中的任务!别让我歇着!"};
    std::vector<std::string> task_titles;
    std::vector<std::optional<std::string>> progress_labels;
};

NSColor* HeaderColor(ReminderState state) {
    switch (state) {
        case ReminderState::Busy: return [NSColor colorWithSRGBRed:43/255.0 green:105/255.0 blue:168/255.0 alpha:1];
        case ReminderState::Completed: return [NSColor colorWithSRGBRed:43/255.0 green:139/255.0 blue:87/255.0 alpha:1];
        case ReminderState::Error: return [NSColor colorWithSRGBRed:194/255.0 green:57/255.0 blue:52/255.0 alpha:1];
        default: return [NSColor colorWithSRGBRed:67/255.0 green:105/255.0 blue:139/255.0 alpha:1];
    }
}

constexpr CGFloat kPetWindowWidth = static_cast<CGFloat>(render_layout::logical_width);
constexpr CGFloat kPetWindowHeight = static_cast<CGFloat>(render_layout::logical_height);
constexpr NSTimeInterval kUiTimerInterval = 0.10;
constexpr NSTimeInterval kUiTimerTolerance = 0.025;

int PetAnimationFrame(ReminderState state, DockEdge edge, int animation_tick) noexcept {
    return edge == DockEdge::None
        ? app_logic::select_floating_frame(state, animation_tick)
        : app_logic::select_dock_sprite_index(edge, state, animation_tick);
}

int StatusAnimationFrame(ReminderState state, int animation_tick) noexcept {
    return state == ReminderState::Busy ? std::abs(animation_tick / 2) % 8 : 0;
}

render_layout::State LayoutStateFor(const MacRenderState& state) noexcept {
    return {state.state, state.dock_edge, state.docked, state.bubble_below,
            state.mirror, state.dock_visibility, state.animation_tick};
}

NSRect NsRect(const RectD& value) {
    return NSMakeRect(static_cast<CGFloat>(value.x), static_cast<CGFloat>(value.y),
                      static_cast<CGFloat>(value.width), static_cast<CGFloat>(value.height));
}

NSRect BubbleBounds(const MacRenderState& state, NSRect) {
    return NsRect(render_layout::bubble_bounds(LayoutStateFor(state)));
}

NSRect FloatingDestination(const MacRenderState& state, NSRect) {
    return NsRect(render_layout::floating_pet_bounds(LayoutStateFor(state)));
}

NSRect DockPetBounds(const MacRenderState& state, NSRect) {
    return NsRect(render_layout::dock_pet_bounds(LayoutStateFor(state)));
}

NSRect PetInteractionBounds(const MacRenderState& state, NSRect) {
    return NsRect(render_layout::pet_interaction_bounds(LayoutStateFor(state)));
}

NSScreen* PrimaryScreen() {
    if (NSScreen.mainScreen) return NSScreen.mainScreen;
    return NSScreen.screens.firstObject;
}

NSNumber* ScreenNumber(NSScreen* screen) {
    return screen.deviceDescription[@"NSScreenNumber"];
}

std::string ScreenIdentifier(NSScreen* screen) {
    if (!screen) return {};
    const NSRect frame = screen.frame;
    NSString* name = screen.localizedName ? screen.localizedName : @"Display";
    std::ostringstream output;
    output << Utf8(name) << "|" << ScreenNumber(screen).unsignedIntValue << "|"
           << std::lround(NSMinX(frame)) << "," << std::lround(NSMinY(frame)) << ","
           << std::lround(NSWidth(frame)) << "," << std::lround(NSHeight(frame));
    return output.str();
}

NSScreen* ScreenForIdentifier(std::string_view identifier) {
    if (NSScreen.screens.count == 0) return nil;
    for (NSScreen* screen in NSScreen.screens) {
        if (ScreenIdentifier(screen) == identifier) return screen;
    }

    const auto firstSeparator = identifier.find('|');
    const std::string_view first = firstSeparator == std::string_view::npos
        ? identifier : identifier.substr(0, firstSeparator);
    if (first.rfind("display:", 0) == 0) {
        const auto wanted = static_cast<unsigned long>(std::strtoul(
            std::string(first.substr(8)).c_str(), nullptr, 10));
        for (NSScreen* screen in NSScreen.screens) {
            if (ScreenNumber(screen).unsignedLongValue == wanted) return screen;
        }
    }

    if (firstSeparator != std::string_view::npos) {
        const auto secondSeparator = identifier.find('|', firstSeparator + 1);
        if (secondSeparator != std::string_view::npos) {
            const auto numberText = identifier.substr(firstSeparator + 1,
                                                       secondSeparator - firstSeparator - 1);
            const auto wanted = static_cast<unsigned long>(std::strtoul(
                std::string(numberText).c_str(), nullptr, 10));
            if (wanted != 0) {
                for (NSScreen* screen in NSScreen.screens) {
                    if (ScreenNumber(screen).unsignedLongValue == wanted) return screen;
                }
            }
        }
    }

    NSString* legacyName = Ns(first);
    for (NSScreen* screen in NSScreen.screens) {
        if ([screen.localizedName isEqualToString:legacyName]) return screen;
    }
    return PrimaryScreen();
}

NSScreen* ScreenAtPoint(NSPoint point) {
    for (NSScreen* screen in NSScreen.screens) {
        if (NSPointInRect(point, screen.frame)) return screen;
    }
    NSScreen* nearest = PrimaryScreen();
    CGFloat bestDistance = std::numeric_limits<CGFloat>::max();
    for (NSScreen* screen in NSScreen.screens) {
        const NSRect frame = screen.frame;
        const CGFloat dx = point.x < NSMinX(frame) ? NSMinX(frame) - point.x
            : point.x > NSMaxX(frame) ? point.x - NSMaxX(frame) : 0;
        const CGFloat dy = point.y < NSMinY(frame) ? NSMinY(frame) - point.y
            : point.y > NSMaxY(frame) ? point.y - NSMaxY(frame) : 0;
        const CGFloat distance = dx * dx + dy * dy;
        if (distance < bestDistance) { bestDistance = distance; nearest = screen; }
    }
    return nearest;
}

CGFloat ClampOrigin(CGFloat value, CGFloat minimum, CGFloat maximum, CGFloat extent) {
    return std::clamp(value, minimum, std::max(minimum, maximum - extent));
}

NSTextField* MakeLabel(NSString* text, NSRect frame) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.frame = frame;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

NSTextField* MakeTextField(NSString* value, NSRect frame) {
    NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
    field.stringValue = value ? value : @"";
    field.bezelStyle = NSTextFieldRoundedBezel;
    return field;
}

NSButton* MakeButton(NSString* title, id target, SEL action, NSRect frame) {
    NSButton* button = [NSButton buttonWithTitle:title target:target action:action];
    button.frame = frame;
    button.bezelStyle = NSBezelStyleRounded;
    return button;
}

NSFont* CloudFont(CGFloat size, BOOL bold) {
    // Use only the built-in system family. Looking up optional downloadable CJK
    // faces can block headless macOS packaging while CoreText activates fonts.
    NSFont* system = [NSFont systemFontOfSize:size
                                      weight:bold ? NSFontWeightBold : NSFontWeightRegular];
    NSFontDescriptor* rounded = [system.fontDescriptor
        fontDescriptorWithDesign:NSFontDescriptorSystemDesignRounded];
    return rounded ? [NSFont fontWithDescriptor:rounded size:size] : system;
}

NSFont* CloudBodyFont(CGFloat size) {
    // The system semibold CJK fallback (PingFang on Chinese macOS) keeps small
    // task text clear without triggering optional font activation.
    return [NSFont systemFontOfSize:size weight:NSFontWeightSemibold];
}

std::string ArgumentAt(int argc, const char* const* argv, int index) {
    return index >= 0 && index < argc && argv[index] ? std::string(argv[index]) : std::string{};
}

} // namespace

@class MacRenderer;

@interface PreviewCanvas : NSView
- (instancetype)initWithRenderer:(MacRenderer*)renderer state:(const MacRenderState&)state;
@end

@interface MacRenderer : NSObject
- (BOOL)validate:(NSString* __autoreleasing*)error;
- (void)drawState:(const MacRenderState&)state inRect:(NSRect)rect;
- (BOOL)savePreview:(const MacRenderState&)state toPath:(NSString*)path error:(NSString* __autoreleasing*)error;
- (NSImage*)statusImageForState:(ReminderState)state frame:(int)frame;
- (NSImage*)cloudImage;
- (void)discardCloudImage;
- (void)trimTransientImages;
@end

@implementation MacRenderer {
    NSMutableDictionary<NSString*, NSImage*>* _cache;
    NSArray<NSDictionary*>* _headerAttributes;
    NSDictionary* _bodyAttributes;
    NSColor* _dotFillColor;
    NSColor* _dotOutlineColor;
    int _floatingRow;
    int _floatingPair;
    int _dockSide;
    int _dockPair;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _cache = [NSMutableDictionary dictionary];
        NSMutableParagraphStyle* headerStyle = [[NSMutableParagraphStyle alloc] init];
        headerStyle.alignment = NSTextAlignmentCenter;
        NSFont* headerFont = CloudFont(13.0, YES);
        _headerAttributes = @[
            @{NSFontAttributeName: headerFont, NSForegroundColorAttributeName: HeaderColor(ReminderState::Idle),
              NSParagraphStyleAttributeName: headerStyle},
            @{NSFontAttributeName: headerFont, NSForegroundColorAttributeName: HeaderColor(ReminderState::Completed),
              NSParagraphStyleAttributeName: headerStyle},
            @{NSFontAttributeName: headerFont, NSForegroundColorAttributeName: HeaderColor(ReminderState::Busy),
              NSParagraphStyleAttributeName: headerStyle},
            @{NSFontAttributeName: headerFont, NSForegroundColorAttributeName: HeaderColor(ReminderState::Error),
              NSParagraphStyleAttributeName: headerStyle},
            @{NSFontAttributeName: headerFont, NSForegroundColorAttributeName: HeaderColor(ReminderState::Idle),
              NSParagraphStyleAttributeName: headerStyle}
        ];
        NSMutableParagraphStyle* bodyStyle = [[NSMutableParagraphStyle alloc] init];
        bodyStyle.lineBreakMode = NSLineBreakByWordWrapping;
        bodyStyle.minimumLineHeight = 16;
        bodyStyle.maximumLineHeight = 16;
        _bodyAttributes = @{
            NSFontAttributeName: CloudBodyFont(12.0),
            NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:34/255.0 green:45/255.0 blue:62/255.0 alpha:1],
            NSParagraphStyleAttributeName: bodyStyle
        };
        _dotFillColor = [NSColor colorWithSRGBRed:241/255.0 green:248/255.0 blue:1 alpha:1];
        _dotOutlineColor = [NSColor colorWithSRGBRed:42/255.0 green:50/255.0 blue:60/255.0 alpha:1];
        _floatingRow = -1;
        _floatingPair = -1;
        _dockSide = -1;
        _dockPair = -1;
    }
    return self;
}

- (NSImage*)imageInDirectory:(NSString*)directory name:(NSString*)name {
    NSString* key = [NSString stringWithFormat:@"%@/%@", directory, name];
    NSImage* image = _cache[key];
    if (image) return image;
    NSString* path = ResourcePath(directory, name, @"png");
    if (!path) return nil;
    image = [[NSImage alloc] initByReferencingFile:path];
    if (image) {
        // Keep only the explicitly bounded images below, while allowing AppKit to
        // reuse one decoded bitmap instead of allocating a temporary one per draw.
        image.cacheMode = NSImageCacheBySize;
        _cache[key] = image;
    }
    return image;
}

- (NSImage*)cloudImage {
    return [self imageInDirectory:@"cloud" name:@"cloud-bubble-540"];
}

- (NSImage*)floatingImageForState:(ReminderState)state frame:(int)frame {
    if (_dockSide != -1) {
        RemoveCachedImagesWithPrefix(_cache, @"dock/");
        _dockSide = -1;
        _dockPair = -1;
    }
    const int row = app_logic::select_floating_sprite_row(state);
    const int safeFrame = std::clamp(frame, 0, 7);
    const int pair = safeFrame / 2;
    if (_floatingRow != row || _floatingPair != pair) {
        RemoveCachedImagesWithPrefix(_cache, @"floating/");
        _floatingRow = row;
        _floatingPair = pair;
    }
    NSString* name = [NSString stringWithFormat:@"%s-%d", StateName(state).c_str(), safeFrame];
    return [self imageInDirectory:@"floating" name:name];
}

- (NSImage*)dockImage:(int)frame {
    frame = std::clamp(frame, 0, 19);
    if (_floatingRow != -1) {
        RemoveCachedImagesWithPrefix(_cache, @"floating/");
        _floatingRow = -1;
        _floatingPair = -1;
    }
    const int side = frame >= 10 ? 1 : 0;
    const int pair = (frame % 10) / 2;
    if (_dockSide != side || _dockPair != pair) {
        RemoveCachedImagesWithPrefix(_cache, @"dock/");
        _dockSide = side;
        _dockPair = pair;
    }
    return [self imageInDirectory:@"dock" name:[NSString stringWithFormat:@"dock-%d", frame]];
}

- (NSImage*)statusImageForState:(ReminderState)state frame:(int)frame {
    NSString* name = state == ReminderState::Busy
        ? [NSString stringWithFormat:@"status-busy-%d", std::abs(frame) % 8]
        : state == ReminderState::Completed ? @"status-completed"
        : state == ReminderState::Error ? @"status-error" : @"status-idle";
    return [self imageInDirectory:@"icons" name:name];
}

- (BOOL)validate:(NSString* __autoreleasing*)error {
    for (int row = 0; row < 5; ++row) {
        const char* state = row == 0 ? "idle" : row == 1 ? "completed" : row == 2 ? "busy" :
                            row == 3 ? "error" : "interrupted";
        for (int frame = 0; frame < 8; ++frame) {
            NSString* name = [NSString stringWithFormat:@"%s-%d", state, frame];
            if (!PngResourceHasPixelSize(@"floating", name, 192, 208)) {
                if (error) *error = @"浮动精灵资源缺失或尺寸错误";
                return NO;
            }
        }
    }
    for (int frame = 0; frame < 20; ++frame) {
        NSString* name = [NSString stringWithFormat:@"dock-%d", frame];
        if (!PngResourceHasPixelSize(@"dock", name, 256, 256)) {
            if (error) *error = @"扒边精灵资源缺失或尺寸错误";
            return NO;
        }
    }
    if (!PngResourceHasPixelSize(@"cloud", @"cloud-bubble-540",
                                 render_layout::cloud_bitmap_width,
                                 render_layout::cloud_bitmap_height)) {
        if (error) *error = @"云朵资源缺失或尺寸错误";
        return NO;
    }
    for (NSString* name in @[@"status-idle", @"status-completed", @"status-error",
                              @"status-busy-0", @"status-busy-1", @"status-busy-2", @"status-busy-3",
                              @"status-busy-4", @"status-busy-5", @"status-busy-6", @"status-busy-7"]) {
        if (!PngResourceHasPixelSize(@"icons", name, 64, 64)) {
            if (error) *error = @"状态图标资源缺失或尺寸错误";
            return NO;
        }
    }
    for (NSString* name in @[@"voice-start", @"voice-complete", @"voice-error", @"voice-interrupted"]) {
        if (!ResourcePath(@"audio", name, @"mp3")) {
            if (error) *error = @"语音资源缺失";
            return NO;
        }
    }
    return YES;
}

- (void)discardCloudImage {
    RemoveCachedImage(_cache, @"cloud/cloud-bubble-540");
}

- (void)trimTransientImages {
    RemoveCachedImagesWithPrefix(_cache, @"floating/");
    RemoveCachedImagesWithPrefix(_cache, @"dock/");
    [self discardCloudImage];
    _floatingRow = -1;
    _floatingPair = -1;
    _dockSide = -1;
    _dockPair = -1;
}

- (void)drawCloudForState:(const MacRenderState&)state inRect:(NSRect)rect {
    (void)rect;
    const auto layout = LayoutStateFor(state);
    const NSRect cloud = NsRect(render_layout::bubble_bounds(layout));
    NSImage* image = [self cloudImage];
    if (image) {
        [NSGraphicsContext saveGraphicsState];
        CGContextSetInterpolationQuality(NSGraphicsContext.currentContext.CGContext,
                                         kCGInterpolationHigh);
        [image drawInRect:cloud fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                 fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
    }

    const auto dotBounds = render_layout::thought_dot_bounds(layout);
    const NSRect largeDot = NsRect(dotBounds.large);
    const NSRect smallDot = NsRect(dotBounds.secondary);
    auto drawDot = [&](NSRect bounds, CGFloat outerNotch, CGFloat innerNotch) {
        NSBezierPath* outer = [NSBezierPath bezierPath];
        [outer moveToPoint:NSMakePoint(NSMinX(bounds) + outerNotch, NSMinY(bounds))];
        [outer lineToPoint:NSMakePoint(NSMaxX(bounds) - outerNotch, NSMinY(bounds))];
        [outer lineToPoint:NSMakePoint(NSMaxX(bounds), NSMinY(bounds) + outerNotch)];
        [outer lineToPoint:NSMakePoint(NSMaxX(bounds), NSMaxY(bounds) - outerNotch)];
        [outer lineToPoint:NSMakePoint(NSMaxX(bounds) - outerNotch, NSMaxY(bounds))];
        [outer lineToPoint:NSMakePoint(NSMinX(bounds) + outerNotch, NSMaxY(bounds))];
        [outer lineToPoint:NSMakePoint(NSMinX(bounds), NSMaxY(bounds) - outerNotch)];
        [outer lineToPoint:NSMakePoint(NSMinX(bounds), NSMinY(bounds) + outerNotch)];
        [outer closePath];
        [_dotOutlineColor setFill];
        [outer fill];
        if (NSWidth(bounds) <= 6 || NSHeight(bounds) <= 6) return;
        const NSRect innerBounds = NSInsetRect(bounds, 3, 3);
        NSBezierPath* inner = [NSBezierPath bezierPath];
        [inner moveToPoint:NSMakePoint(NSMinX(innerBounds) + innerNotch, NSMinY(innerBounds))];
        [inner lineToPoint:NSMakePoint(NSMaxX(innerBounds) - innerNotch, NSMinY(innerBounds))];
        [inner lineToPoint:NSMakePoint(NSMaxX(innerBounds), NSMinY(innerBounds) + innerNotch)];
        [inner lineToPoint:NSMakePoint(NSMaxX(innerBounds), NSMaxY(innerBounds) - innerNotch)];
        [inner lineToPoint:NSMakePoint(NSMaxX(innerBounds) - innerNotch, NSMaxY(innerBounds))];
        [inner lineToPoint:NSMakePoint(NSMinX(innerBounds) + innerNotch, NSMaxY(innerBounds))];
        [inner lineToPoint:NSMakePoint(NSMinX(innerBounds), NSMaxY(innerBounds) - innerNotch)];
        [inner lineToPoint:NSMakePoint(NSMinX(innerBounds), NSMinY(innerBounds) + innerNotch)];
        [inner closePath];
        [_dotFillColor setFill];
        [inner fill];
    };
    drawDot(largeDot, 3, 1);
    drawDot(smallDot, 3, 1);

    const auto bulbOrigin = render_layout::bulb_origin(layout);
    const CGFloat ox = static_cast<CGFloat>(bulbOrigin.x);
    const CGFloat oy = static_cast<CGFloat>(bulbOrigin.y);
    NSColor* glow = state.state == ReminderState::Error
        ? [NSColor colorWithSRGBRed:226/255.0 green:62/255.0 blue:55/255.0 alpha:1]
        : [NSColor colorWithSRGBRed:83/255.0 green:169/255.0 blue:236/255.0 alpha:1];
    NSColor* highlight = state.state == ReminderState::Error
        ? [NSColor colorWithSRGBRed:1 green:174/255.0 blue:154/255.0 alpha:1]
        : [NSColor colorWithSRGBRed:202/255.0 green:232/255.0 blue:1 alpha:1];
    NSColor* outline = [NSColor colorWithSRGBRed:68/255.0 green:43/255.0 blue:25/255.0 alpha:1];
    NSColor* base = [NSColor colorWithSRGBRed:91/255.0 green:78/255.0 blue:70/255.0 alpha:1];
    auto cell = [&](NSColor* color, int x, int y, int width, int height) {
        [color setFill];
        NSRectFill(NSMakeRect(ox + x * 2.5, oy + y * 2.5, width * 2.5, height * 2.5));
    };
    cell(outline, 6, 0, 1, 2); cell(outline, 2, 2, 1, 1); cell(outline, 10, 2, 1, 1);
    cell(outline, 0, 6, 2, 1); cell(outline, 11, 6, 2, 1);
    const int silhouette[][3] = {{3,4,5},{4,3,7},{5,2,9},{6,2,9},{7,2,9},{8,3,7},
                                  {9,4,5},{10,5,3},{11,4,5},{12,4,5},{13,5,3}};
    for (const auto& row : silhouette) cell(outline, row[1], row[0], row[2], 1);
    cell(glow, 4, 4, 5, 1); cell(glow, 3, 5, 7, 3); cell(glow, 4, 8, 5, 1);
    cell(glow, 5, 9, 3, 1); cell(highlight, 4, 4, 2, 1); cell(highlight, 3, 5, 2, 2);
    cell(outline, 5, 7, 1, 2); cell(outline, 7, 7, 1, 2); cell(outline, 6, 9, 1, 1);
    cell(base, 5, 11, 3, 2); cell(outline, 5, 13, 3, 1);

    std::optional<std::string_view> progress;
    const int progressIndex = std::clamp(state.selected_task_index, 0,
        std::max(0, static_cast<int>(state.progress_labels.size()) - 1));
    if (progressIndex < static_cast<int>(state.progress_labels.size()) &&
        state.progress_labels[static_cast<std::size_t>(progressIndex)]) {
        progress = *state.progress_labels[static_cast<std::size_t>(progressIndex)];
    }
    const int titleIndex = std::clamp(state.selected_task_index, 0,
        std::max(0, static_cast<int>(state.task_titles.size()) - 1));
    const std::string header = state.state == ReminderState::Busy
        ? app_logic::format_busy_header(progress, state.selected_task_index,
                                        static_cast<int>(state.task_titles.size()))
        : state.status_text;
    const std::string body = NormalizedText(!state.task_titles.empty()
        ? state.task_titles[static_cast<std::size_t>(titleIndex)] : state.thought_text);
    const int headerIndex = state.state == ReminderState::Completed ? 1
        : state.state == ReminderState::Busy ? 2
        : state.state == ReminderState::Error ? 3
        : state.state == ReminderState::Interrupted ? 4 : 0;
    const NSRect headerRect = NsRect(render_layout::header_bounds(layout));
    NSDictionary* headerAttributes = _headerAttributes[static_cast<std::size_t>(headerIndex)];
    const NSSize headerSize = [Ns(header) sizeWithAttributes:headerAttributes];
    const CGFloat headerY = NSMinY(headerRect) + std::max<CGFloat>(0, (NSHeight(headerRect) - headerSize.height) / 2);
    [Ns(header) drawInRect:NSMakeRect(NSMinX(headerRect), headerY, NSWidth(headerRect), headerSize.height)
             withAttributes:headerAttributes];
    const NSRect viewport = NsRect(render_layout::body_bounds(layout));
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:viewport] addClip];
    [Ns(body) drawInRect:NSMakeRect(NSMinX(viewport), NSMinY(viewport) - state.scroll_offset,
                                    NSWidth(viewport), 1000)
             withAttributes:_bodyAttributes];
    [NSGraphicsContext restoreGraphicsState];
}
- (void)drawPetForState:(const MacRenderState&)state inRect:(NSRect)rect {
    if (state.docked) {
        const int frame = app_logic::select_dock_sprite_index(state.dock_edge, state.state, state.animation_tick);
        NSImage* image = [self dockImage:frame];
        if (!image) return;
        const NSRect destination = DockPetBounds(state, rect);
        [NSGraphicsContext saveGraphicsState];
        CGContextSetInterpolationQuality(NSGraphicsContext.currentContext.CGContext,
                                         kCGInterpolationHigh);
        [image drawInRect:destination fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver fraction:1
           respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        return;
    }
    const int frame = app_logic::select_floating_frame(state.state, state.animation_tick);
    NSImage* image = [self floatingImageForState:state.state frame:frame];
    if (!image) return;
    const NSRect destination = FloatingDestination(state, rect);
    if (state.mirror) {
        [NSGraphicsContext saveGraphicsState];
        NSAffineTransform* transform = [NSAffineTransform transform];
        [transform translateXBy:NSMidX(destination) yBy:0];
        [transform scaleXBy:-1 yBy:1];
        [transform translateXBy:-NSMidX(destination) yBy:0];
        [transform concat];
    }
    [image drawInRect:destination fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver fraction:1
       respectFlipped:YES hints:nil];
    if (state.mirror) [NSGraphicsContext restoreGraphicsState];
}

- (void)drawState:(const MacRenderState&)state inRect:(NSRect)rect {
    [[NSColor clearColor] setFill]; NSRectFillUsingOperation(rect, NSCompositingOperationCopy);
    if (state.bubble_visible) [self drawCloudForState:state inRect:rect];
    [self drawPetForState:state inRect:rect];
}

- (BOOL)savePreview:(const MacRenderState&)state toPath:(NSString*)path error:(NSString* __autoreleasing*)error {
    PreviewCanvas* canvas = [[PreviewCanvas alloc] initWithRenderer:self state:state];
    const NSRect bounds = canvas.bounds;
    NSBitmapImageRep* bitmap = [canvas bitmapImageRepForCachingDisplayInRect:bounds];
    if (!bitmap) {
        if (error) *error = @"无法创建预览位图";
        return NO;
    }
    [canvas cacheDisplayInRect:bounds toBitmapImageRep:bitmap];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png || ![png writeToFile:path atomically:YES]) {
        if (error) *error = @"保存预览失败";
        return NO;
    }
    return YES;
}

@end

@implementation PreviewCanvas {
    MacRenderer* _renderer;
    MacRenderState _state;
}

- (instancetype)initWithRenderer:(MacRenderer*)renderer state:(const MacRenderState&)state {
    self = [super initWithFrame:NSMakeRect(0, 0, render_layout::logical_width,
                                            render_layout::logical_height)];
    if (self) {
        _renderer = renderer;
        _state = state;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [_renderer drawState:_state inRect:self.bounds];
}

@end

@interface PetPanel : NSPanel
@end

@implementation PetPanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@class AppDelegate;

@interface PetView : NSView
@property(nonatomic, weak) AppDelegate* owner;
@end

using PendingMacUpdate = PendingMonitorUpdate;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate, NSSoundDelegate> {
@private
    NSStatusItem* _statusItem;
    PetPanel* _petWindow;
    PetView* _petView;
    NSTimer* _timer;
    MacRenderer* _renderer;
    NSSound* _currentSound;
    NSWindow* _settingsWindow;
    NSTextField* _settingsHoverField;
    NSTextField* _settingsIdleField;
    NSTextField* _settingsRevealField;
    NSTextField* _settingsNotificationField;
    NSTextField* _settingsRootField;
    NSButton* _settingsSoundButton;
    NSButton* _settingsXiaoAiEnabledButton;
    NSTextField* _settingsXiaoAiParallelField;
    NSButton* _settingsXiaoAiSelectAllButton;
    NSButton* _settingsXiaoAiLoginButton;
    NSButton* _settingsXiaoAiScanButton;
    NSButton* _settingsXiaoAiTestButton;
    NSTextField* _settingsXiaoAiStatusField;
    NSStackView* _settingsXiaoAiDevicesStack;
    BOOL _xiaoaiOperationInFlight;
    std::unique_ptr<JsonSettingsStore> _settingsStore;
    AppSettings _settings;
    VisualStateCoordinator _visualCoordinator;
    MonitorSnapshot _snapshot;
    std::unique_ptr<MonitorWorker> _monitorWorker;
    std::unique_ptr<XiaoAiNotifier> _xiaoaiNotifier;
    std::vector<XiaoAiDeviceInfo> _xiaoaiDevices;
    MacRenderState _renderState;
    DockEdge _dockEdge;
    std::string _dockScreenIdentifier;
    CGFloat _dockCoordinate;
    double _dockVisibility;
    Clock::time_point _dockLastChange;
    Clock::time_point _dockThoughtUntil;
    Clock::time_point _dockRevealUntil;
    Clock::time_point _lastTick;
    double _animationAccumulator;
    int _animationTick;
    int _selectedTaskIndex;
    double _scrollOffset;
    double _scrollHold;
    bool _scrollAtEnd;
    double _rotationSeconds;
    bool _dragPending;
    bool _dragging;
    bool _dragStartedDocked;
    NSPoint _dragStartCursor;
    NSPoint _dragStartOrigin;
    NSPoint _lastCursor;
    NSPoint _lastHoverCursor;
    bool _hasLastHoverCursor;
    NSInteger _lastMouseReception;
    id _globalMouseMonitor;
    MonitorUpdateQueue _pendingMonitorUpdates;
    std::uint64_t _monitorGeneration;
    bool _hasSnapshot;
    bool _expressionDemo;
    int _expressionDemoIndex;
    Clock::time_point _expressionDemoNext;
    std::atomic_bool _terminating;
}
- (void)loadOrMigrateSettings;
- (void)saveSettings;
- (void)playSoundNamed:(NSString*)name;
- (void)releaseCurrentSound;
- (void)notifyXiaoAi:(XiaoAiEvent)event context:(std::string_view)context;
- (void)openXiaoAiLogin:(id)sender;
- (void)setXiaoAiOperationInFlight:(BOOL)inFlight;
- (void)scanXiaoAiDevices:(id)sender;
- (void)loadXiaoAiDevicesShowingErrors:(BOOL)showErrors;
- (void)testXiaoAi:(id)sender;
- (void)populateXiaoAiDevices;
- (void)toggleXiaoAiAllDevices:(id)sender;
- (void)xiaoAiDeviceSelectionChanged:(id)sender;
- (void)updateXiaoAiSelectionSummary;
- (std::vector<std::string>)selectedXiaoAiDeviceIds;
- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action;
- (void)updateMenu:(NSMenu*)menu;
- (void)togglePet:(id)sender;
- (void)toggleSound:(id)sender;
- (void)toggleXiaoAi:(id)sender;
- (BOOL)autoStartEnabled;
- (void)toggleStartup:(id)sender;
- (void)openSessionsFolder:(id)sender;
- (void)openUpdate:(id)sender;
- (void)quit:(id)sender;
- (void)createPetWindow;
- (void)installMouseMonitor;
- (void)removeMouseMonitor;
- (void)createStatusItem;
- (void)placeDefault;
- (void)restorePosition;
- (void)savePosition;
- (void)clampToWorkArea;
- (void)updateDockWindowPosition;
- (void)trySnapOrClamp:(NSPoint)cursor;
- (void)updateMousePassThrough;
- (BOOL)dockHovering:(NSPoint)cursor;
- (BOOL)dockHoverPathCrossesFrom:(NSPoint)from to:(NSPoint)to;
- (void)revealDock;
- (void)drawPetView:(NSRect)rect;
- (BOOL)isInteractivePoint:(NSPoint)point;
- (void)petMouseDown:(NSEvent*)event;
- (void)petMouseDragged:(NSEvent*)event;
- (void)petMouseUp:(NSEvent*)event;
- (NSMenu*)buildMenu;
- (void)showSettings:(id)sender;
- (void)browseSessionsRoot:(id)sender;
- (void)restoreSettingsDefaults:(id)sender;
- (void)cancelSettings:(id)sender;
- (void)applySettings:(id)sender;
- (void)setExpressionDemoEnabled:(BOOL)enabled;
- (void)startMonitor;
- (void)enqueueMonitorUpdate:(PendingMacUpdate)update;
- (void)processMonitorUpdates;
- (void)applyMonitorUpdate:(PendingMacUpdate&)update;
- (void)showExpressionDemoState:(int)index now:(Clock::time_point)now;
- (void)refreshVisual:(BOOL)force;
- (void)updateRenderGeometry;
- (void)onTimer:(NSTimer*)timer;
@end

@implementation PetView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)drawRect:(NSRect)dirtyRect {
    @autoreleasepool {
        [super drawRect:dirtyRect];
        [self.owner drawPetView:self.bounds];
    }
}
- (NSView*)hitTest:(NSPoint)point {
    // NSView hitTest: receives a point in the superview's coordinate system.
    // The content view is unflipped while PetView is flipped, so passing the
    // point directly made the lower body/legs miss and left the window
    // click-through until a later click happened to activate it.
    const NSPoint local = [self convertPoint:point fromView:self.superview];
    return [self.owner isInteractivePoint:local] ? self : nil;
}
- (void)mouseDown:(NSEvent*)event { [self.owner petMouseDown:event]; }
- (void)mouseDragged:(NSEvent*)event { [self.owner petMouseDragged:event]; }
- (void)mouseUp:(NSEvent*)event { [self.owner petMouseUp:event]; }
- (void)rightMouseDown:(NSEvent*)event {
    [NSMenu popUpContextMenu:[self.owner buildMenu] withEvent:event forView:self];
}
@end

@implementation AppDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        _renderer = [[MacRenderer alloc] init];
        _settingsStore = std::make_unique<JsonSettingsStore>();
        _settings = AppSettings{};
        _dockEdge = DockEdge::None;
        _dockVisibility = 1;
        _dockLastChange = Clock::now();
        _dockThoughtUntil = Clock::time_point::min();
        _dockRevealUntil = Clock::time_point::min();
        _lastTick = Clock::now();
        _scrollHold = 1.9;
        _lastMouseReception = -1;
        _terminating.store(false, std::memory_order_relaxed);
        _expressionDemoIndex = -1;
        _expressionDemoNext = Clock::time_point::min();
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    NSString* validationError = nil;
    if (![_renderer validate:&validationError]) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"CodeXPets 资源校验失败";
        alert.informativeText = validationError ? validationError : @"未知错误";
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    [self loadOrMigrateSettings];
    [self createPetWindow];
    [self installMouseMonitor];
    [self createStatusItem];
    [self placeDefault];
    [self restorePosition];
    if (!_expressionDemo) [self startMonitor];
    if (_expressionDemo || _settings.pet_visible) [_petWindow orderFrontRegardless];
    _timer = [NSTimer scheduledTimerWithTimeInterval:kUiTimerInterval target:self
                                            selector:@selector(onTimer:) userInfo:nil repeats:YES];
    _timer.tolerance = kUiTimerTolerance;
    if (_expressionDemo) [self showExpressionDemoState:0 now:Clock::now()];
    else [self refreshVisual:YES];
    [self updateMousePassThrough];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    _terminating.store(true, std::memory_order_release);
    _pendingMonitorUpdates.clear();
    [_timer invalidate];
    [self removeMouseMonitor];
    if (_monitorWorker) _monitorWorker->stop();
    if (_xiaoaiNotifier) _xiaoaiNotifier->stop();
    [self releaseCurrentSound];
    [_renderer trimTransientImages];
    [self savePosition];
    return NSTerminateNow;
}

- (void)loadOrMigrateSettings {
    if (std::filesystem::exists(_settingsStore->settings_file_path())) {
        _settings = _settingsStore->load();
    } else {
        AppSettings migrated;
        NSUserDefaults* defaults = [[NSUserDefaults alloc]
            initWithSuiteName:@"com.mrliugangqiang.codexpets"];
        auto integer = [&](NSString* key, int fallback) {
            return [defaults objectForKey:key] ? static_cast<int>([defaults integerForKey:key])
                                               : fallback;
        };
        migrated.dock_hover_height = integer(@"DockHoverHeight", migrated.dock_hover_height);
        migrated.dock_idle_hide_seconds = integer(@"DockIdleHideSeconds", migrated.dock_idle_hide_seconds);
        migrated.dock_reveal_seconds = integer(@"DockRevealSeconds", migrated.dock_reveal_seconds);
        migrated.dock_notification_seconds = integer(@"DockNotificationSeconds",
                                                      migrated.dock_notification_seconds);
        if ([defaults objectForKey:@"SoundEnabled"]) {
            migrated.sound_enabled = [defaults boolForKey:@"SoundEnabled"];
        }
        NSString* root = [defaults stringForKey:@"SessionsRoot.macOS"];
        if (!root) root = [defaults stringForKey:@"SessionsRoot"];
        if (root.length > 0) migrated.sessions_root = path_from_utf8(Utf8(root));
        NSData* position = [defaults dataForKey:@"PetPositionV1.macOS"];
        if (!position) position = [defaults dataForKey:@"PetPositionV1"];
        if (position.length > 0) {
            std::string json(static_cast<const char*>(position.bytes), position.length);
            migrated.pet_position = deserialize_legacy_macos_position(json);
        }
        migrated.normalize();
        _settings = migrated;
        std::string ignored;
        _settingsStore->save(_settings, &ignored);
    }
    _settings.normalize();
    _settings.xiaoai.auth_cookies = macos::load_xiaoai_authorization();
    _xiaoaiNotifier = std::make_unique<XiaoAiNotifier>(macos::make_xiaoai_http_transport());
    _xiaoaiNotifier->configure(_settings.xiaoai);
}

- (void)createPetWindow {
    _petWindow = [[PetPanel alloc] initWithContentRect:NSMakeRect(0, 0, kPetWindowWidth, kPetWindowHeight)
                                            styleMask:NSWindowStyleMaskBorderless |
                                                      NSWindowStyleMaskNonactivatingPanel
                                              backing:NSBackingStoreBuffered defer:NO];
    _petWindow.opaque = NO;
    _petWindow.backgroundColor = NSColor.clearColor;
    _petWindow.hasShadow = NO;
    _petWindow.level = NSFloatingWindowLevel;
    _petWindow.hidesOnDeactivate = NO;
    _petWindow.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorFullScreenAuxiliary |
                                    NSWindowCollectionBehaviorStationary;
    _petWindow.releasedWhenClosed = NO;
    _petWindow.becomesKeyOnlyIfNeeded = YES;
    _petWindow.acceptsMouseMovedEvents = YES;
    _petWindow.ignoresMouseEvents = YES;
    _petView = [[PetView alloc] initWithFrame:NSMakeRect(0, 0, kPetWindowWidth, kPetWindowHeight)];
    _petView.owner = self;
    _petWindow.contentView = _petView;
}

- (void)installMouseMonitor {
    if (_globalMouseMonitor) return;
    __weak AppDelegate* weakSelf = self;
    _globalMouseMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
        handler:^(NSEvent* event) {
            @autoreleasepool {
                (void)event;
                AppDelegate* strongSelf = weakSelf;
                if (strongSelf) [strongSelf updateMousePassThrough];
            }
        }];
}

- (void)removeMouseMonitor {
    if (!_globalMouseMonitor) return;
    [NSEvent removeMonitor:_globalMouseMonitor];
    _globalMouseMonitor = nil;
}

- (void)createStatusItem {
    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    _statusItem.button.image = [_renderer statusImageForState:ReminderState::Idle frame:0];
    _statusItem.button.image.size = NSMakeSize(18, 18);
    _statusItem.button.toolTip = @"CodeXPets · 空闲";
    _statusItem.menu = [self buildMenu];
}

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    return item;
}

- (NSMenu*)buildMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"CodeXPets"];
    menu.delegate = self;
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* pet = [self menuItem:@"显示桌面宠物" action:@selector(togglePet:)]; pet.tag = 101;
    NSMenuItem* sound = [self menuItem:@"播放语音提醒" action:@selector(toggleSound:)]; sound.tag = 102;
    NSMenuItem* xiaoai = [self menuItem:@"推送到小爱音箱" action:@selector(toggleXiaoAi:)]; xiaoai.tag = 104;
    NSMenuItem* startup = [self menuItem:@"登录时自动运行" action:@selector(toggleStartup:)]; startup.tag = 103;
    [menu addItem:pet]; [menu addItem:sound]; [menu addItem:xiaoai]; [menu addItem:startup];
    [menu addItem:[self menuItem:@"打开 Codex 会话目录" action:@selector(openSessionsFolder:)]];
    [menu addItem:[self menuItem:@"设置…" action:@selector(showSettings:)]];
    [menu addItem:[self menuItem:@"查看更新…" action:@selector(openUpdate:)]];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* version = [[NSMenuItem alloc]
        initWithTitle:Ns(std::string("版本：v") + CODEXPETS_VERSION)
               action:nil keyEquivalent:@""];
    version.enabled = NO;
    [menu addItem:version];
    [menu addItem:[self menuItem:@"退出" action:@selector(quit:)]];
    [self updateMenu:menu];
    return menu;
}

- (void)menuNeedsUpdate:(NSMenu*)menu { [self updateMenu:menu]; }

- (void)updateMenu:(NSMenu*)menu {
    while (NSMenuItem* status = [menu itemWithTag:100]) [menu removeItem:status];
    NSUInteger index = 0;
    const auto add_status = [&](std::string_view line) {
        NSMenuItem* status = [[NSMenuItem alloc] initWithTitle:Ns(line) action:nil keyEquivalent:@""];
        status.enabled = NO;
        status.tag = 100;
        [menu insertItem:status atIndex:index++];
    };
    if (_renderState.status_lines.empty()) add_status(_renderState.status_text);
    else for (const auto& line : _renderState.status_lines) add_status(line);
    [menu itemWithTag:101].state = _settings.pet_visible ? NSControlStateValueOn : NSControlStateValueOff;
    [menu itemWithTag:102].state = _settings.sound_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [menu itemWithTag:104].state = _settings.xiaoai.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [menu itemWithTag:103].state = [self autoStartEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)togglePet:(id)sender {
    (void)sender;
    _settings.pet_visible = !_settings.pet_visible;
    if (_settings.pet_visible) {
        [_petWindow orderFrontRegardless];
        [_petView setNeedsDisplay:YES];
    } else {
        [_petWindow orderOut:nil];
        [_renderer trimTransientImages];
    }
    [self saveSettings];
}

- (void)toggleSound:(id)sender {
    (void)sender;
    _settings.sound_enabled = !_settings.sound_enabled;
    if (!_settings.sound_enabled) [self releaseCurrentSound];
    [self saveSettings];
}

- (void)toggleXiaoAi:(id)sender {
    (void)sender;
    _settings.xiaoai.enabled = !_settings.xiaoai.enabled;
    if (_xiaoaiNotifier) _xiaoaiNotifier->configure(_settings.xiaoai);
    _settingsXiaoAiEnabledButton.state = _settings.xiaoai.enabled
        ? NSControlStateValueOn : NSControlStateValueOff;
    [self saveSettings];
}

- (BOOL)autoStartEnabled {
    if (@available(macOS 13.0, *)) return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
    return NO;
}

- (void)toggleStartup:(id)sender {
    (void)sender;
    if (@available(macOS 13.0, *)) {
        NSError* error = nil;
        SMAppService* service = SMAppService.mainAppService;
        BOOL ok = service.status == SMAppServiceStatusEnabled
            ? [service unregisterAndReturnError:&error]
            : [service registerAndReturnError:&error];
        if (!ok && error) {
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = @"无法更改登录启动";
            alert.informativeText = error.localizedDescription;
            [alert runModal];
        }
    }
}

- (void)openSessionsFolder:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:Ns(path_to_utf8(_settings.sessions_root))]];
}

- (void)openUpdate:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://github.com/MrLiuGangQiang/codex-pets/releases/latest"]];
}

- (void)quit:(id)sender { (void)sender; [NSApp terminate:nil]; }

- (void)saveSettings {
    std::string ignored;
    _settingsStore->save(_settings, &ignored);
}

- (void)releaseCurrentSound {
    if (!_currentSound) return;
    _currentSound.delegate = nil;
    [_currentSound stop];
    _currentSound = nil;
}

- (void)playSoundNamed:(NSString*)name {
    if (!_settings.sound_enabled) return;
    NSString* path = ResourcePath(@"audio", name, @"mp3");
    if (!path) return;
    [self releaseCurrentSound];
    _currentSound = [[NSSound alloc] initWithContentsOfFile:path byReference:YES];
    _currentSound.delegate = self;
    if (![_currentSound play]) [self releaseCurrentSound];
}

- (void)sound:(NSSound*)sound didFinishPlaying:(BOOL)finishedPlaying {
    (void)finishedPlaying;
    if (sound != _currentSound) return;
    _currentSound.delegate = nil;
    _currentSound = nil;
}

- (void)notifyXiaoAi:(XiaoAiEvent)event context:(std::string_view)context {
    if (_xiaoaiNotifier) _xiaoaiNotifier->notify(event, context);
}

- (void)setXiaoAiOperationInFlight:(BOOL)inFlight {
    _xiaoaiOperationInFlight = inFlight;
    const BOOL enabled = !inFlight;
    _settingsXiaoAiLoginButton.enabled = enabled;
    _settingsXiaoAiScanButton.enabled = enabled;
    _settingsXiaoAiTestButton.enabled = enabled;
}

- (void)openXiaoAiLogin:(id)sender {
    (void)sender;
    if (_xiaoaiOperationInFlight || !_xiaoaiNotifier) return;
    [self setXiaoAiOperationInFlight:YES];
    __weak AppDelegate* weakSelf = self;
    macos::start_xiaomi_browser_login(_settingsWindow,
        [weakSelf](std::string cookies, std::string error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                AppDelegate* strongSelf = weakSelf;
                if (!strongSelf || strongSelf->_terminating.load(std::memory_order_acquire)) return;
                if (!error.empty()) {
                    [strongSelf setXiaoAiOperationInFlight:NO];
                    NSAlert* alert = [[NSAlert alloc] init];
                    alert.messageText = @"小米登录失败";
                    alert.informativeText = Ns(error);
                    [alert runModal];
                    return;
                }
                if (!strongSelf->_xiaoaiNotifier) {
                    [strongSelf setXiaoAiOperationInFlight:NO];
                    return;
                }
                XiaoAiSettings candidate = strongSelf->_settings.xiaoai;
                candidate.auth_cookies = std::move(cookies);
                strongSelf->_xiaoaiNotifier->validate_async(std::move(candidate),
                    [weakSelf](XiaoAiSettings validated, std::string validationError) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            AppDelegate* resultSelf = weakSelf;
                            if (!resultSelf || resultSelf->_terminating.load(std::memory_order_acquire)) return;
                            if (!validationError.empty()) {
                                [resultSelf setXiaoAiOperationInFlight:NO];
                                NSAlert* alert = [[NSAlert alloc] init];
                                alert.messageText = @"小米授权校验失败";
                                alert.informativeText = Ns(validationError);
                                [alert runModal];
                                return;
                            }
                            std::string saveError;
                            const auto authorization = compact_xiaoai_authorization(validated.auth_cookies);
                            if (!macos::save_xiaoai_authorization(authorization, &saveError)) {
                                [resultSelf setXiaoAiOperationInFlight:NO];
                                NSAlert* alert = [[NSAlert alloc] init];
                                alert.messageText = @"无法保存小米授权";
                                alert.informativeText = Ns(saveError);
                                [alert runModal];
                                return;
                            }
                            if (!resultSelf->_xiaoaiNotifier) {
                                [resultSelf setXiaoAiOperationInFlight:NO];
                                return;
                            }
                            resultSelf->_settings.xiaoai.enabled = true;
                            resultSelf->_settings.xiaoai.auth_cookies = authorization;
                            resultSelf->_xiaoaiNotifier->configure(resultSelf->_settings.xiaoai);
                            resultSelf->_settingsXiaoAiEnabledButton.state = NSControlStateValueOn;
                            resultSelf->_settingsXiaoAiStatusField.stringValue = @"登录成功，正在加载音箱列表…";
                            [resultSelf saveSettings];
                            resultSelf->_xiaoaiNotifier->discover_devices_async(resultSelf->_settings.xiaoai,
                                [weakSelf](std::vector<XiaoAiDeviceInfo> devices, std::string scanError) {
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                        AppDelegate* scanSelf = weakSelf;
                                        if (!scanSelf || scanSelf->_terminating.load(std::memory_order_acquire)) return;
                                        [scanSelf setXiaoAiOperationInFlight:NO];
                                        if (!scanError.empty()) {
                                            scanSelf->_settingsXiaoAiStatusField.stringValue =
                                                @"登录成功，但音箱列表加载失败；请点击重新加载。";
                                            return;
                                        }
                                        scanSelf->_xiaoaiDevices = std::move(devices);
                                        [scanSelf populateXiaoAiDevices];
                                        scanSelf->_settingsXiaoAiStatusField.stringValue = [NSString stringWithFormat:
                                            @"已加载 %lu 台音箱",
                                            static_cast<unsigned long>(scanSelf->_xiaoaiDevices.size())];
                                    });
                                });
                        });
                    });
            });
        });
}

- (void)populateXiaoAiDevices {
    if (!_settingsXiaoAiDevicesStack) return;
    for (NSView* view in [_settingsXiaoAiDevicesStack.arrangedSubviews copy]) {
        [_settingsXiaoAiDevicesStack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    std::unordered_set<std::string> selected(_settings.xiaoai.device_ids.begin(),
                                             _settings.xiaoai.device_ids.end());
    if (selected.empty() && !_settings.xiaoai.device_id.empty()) {
        selected.insert(_settings.xiaoai.device_id);
    }
    for (std::size_t index = 0; index < _xiaoaiDevices.size(); ++index) {
        const auto& device = _xiaoaiDevices[index];
        std::string label = device.alias.empty() ? device.name : device.alias;
        if (label.empty()) label = device.id;
        NSButton* button = [NSButton checkboxWithTitle:Ns(label) target:self
                                                        action:@selector(xiaoAiDeviceSelectionChanged:)];
        button.tag = static_cast<NSInteger>(index);
        button.state = selected.contains(device.id) ? NSControlStateValueOn : NSControlStateValueOff;
        [_settingsXiaoAiDevicesStack addArrangedSubview:button];
    }
    const CGFloat height = std::max<CGFloat>(94, static_cast<CGFloat>(_xiaoaiDevices.size()) * 24);
    _settingsXiaoAiDevicesStack.frame = NSMakeRect(0, 0, 300, height);
    [self updateXiaoAiSelectionSummary];
}

- (std::vector<std::string>)selectedXiaoAiDeviceIds {
    std::vector<std::string> result;
    for (NSView* view in _settingsXiaoAiDevicesStack.arrangedSubviews) {
        if (![view isKindOfClass:NSButton.class]) continue;
        NSButton* button = static_cast<NSButton*>(view);
        if (button.state != NSControlStateValueOn) continue;
        const auto index = static_cast<std::size_t>(button.tag);
        if (index < _xiaoaiDevices.size()) result.push_back(_xiaoaiDevices[index].id);
    }
    return result;
}

- (void)toggleXiaoAiAllDevices:(id)sender {
    const BOOL selected = [sender state] == NSControlStateValueOn;
    for (NSView* view in _settingsXiaoAiDevicesStack.arrangedSubviews) {
        if ([view isKindOfClass:NSButton.class]) {
            static_cast<NSButton*>(view).state = selected
                ? NSControlStateValueOn : NSControlStateValueOff;
        }
    }
    [self updateXiaoAiSelectionSummary];
}

- (void)xiaoAiDeviceSelectionChanged:(id)sender {
    (void)sender;
    [self updateXiaoAiSelectionSummary];
}

- (void)updateXiaoAiSelectionSummary {
    std::size_t selected = 0;
    std::size_t total = 0;
    for (NSView* view in _settingsXiaoAiDevicesStack.arrangedSubviews) {
        if (![view isKindOfClass:NSButton.class]) continue;
        ++total;
        if (static_cast<NSButton*>(view).state == NSControlStateValueOn) ++selected;
    }
    _settingsXiaoAiSelectAllButton.state = total > 0 && selected == total
        ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)scanXiaoAiDevices:(id)sender {
    (void)sender;
    [self loadXiaoAiDevicesShowingErrors:YES];
}

- (void)loadXiaoAiDevicesShowingErrors:(BOOL)showErrors {
    if (_xiaoaiOperationInFlight || !_xiaoaiNotifier) return;
    [self setXiaoAiOperationInFlight:YES];
    _settingsXiaoAiStatusField.stringValue = @"正在加载音箱列表…";
    const XiaoAiSettings candidate = _settings.xiaoai;
    __weak AppDelegate* weakSelf = self;
    _xiaoaiNotifier->discover_devices_async(candidate,
        [weakSelf, showErrors](std::vector<XiaoAiDeviceInfo> devices, std::string error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                AppDelegate* strongSelf = weakSelf;
                if (!strongSelf || strongSelf->_terminating.load(std::memory_order_acquire)) return;
                [strongSelf setXiaoAiOperationInFlight:NO];
                if (!error.empty()) {
                    strongSelf->_settingsXiaoAiStatusField.stringValue =
                        @"音箱列表加载失败，请点击重新加载。";
                    if (showErrors) {
                        NSAlert* alert = [[NSAlert alloc] init];
                        alert.messageText = @"扫描小爱音箱失败";
                        alert.informativeText = Ns(error);
                        [alert runModal];
                    }
                    return;
                }
                strongSelf->_xiaoaiDevices = std::move(devices);
                [strongSelf populateXiaoAiDevices];
                strongSelf->_settingsXiaoAiStatusField.stringValue = [NSString stringWithFormat:
                    @"已加载 %lu 台音箱", static_cast<unsigned long>(strongSelf->_xiaoaiDevices.size())];
            });
        });
}

- (void)testXiaoAi:(id)sender {
    (void)sender;
    if (_xiaoaiOperationInFlight || !_xiaoaiNotifier) return;
    XiaoAiSettings candidate = _settings.xiaoai;
    candidate.device_ids = [self selectedXiaoAiDeviceIds];
    candidate.device_id = candidate.device_ids.empty() ? std::string{} : candidate.device_ids.front();
    [self setXiaoAiOperationInFlight:YES];
    __weak AppDelegate* weakSelf = self;
    _xiaoaiNotifier->test_async(std::move(candidate), [weakSelf](std::string error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf || strongSelf->_terminating.load(std::memory_order_acquire)) return;
            [strongSelf setXiaoAiOperationInFlight:NO];
            if (!error.empty()) {
                NSAlert* alert = [[NSAlert alloc] init];
                alert.messageText = @"小米测试播报失败";
                alert.informativeText = Ns(error);
                [alert runModal];
            } else {
                strongSelf->_settingsXiaoAiStatusField.stringValue = @"测试播报已发送";
            }
        });
    });
}

- (void)setExpressionDemoEnabled:(BOOL)enabled { _expressionDemo = enabled; }

- (void)startMonitor {
    __weak AppDelegate* weakSelf = self;
    const auto generation = ++_monitorGeneration;
    _monitorWorker = std::make_unique<MonitorWorker>(_settings.sessions_root,
        [weakSelf, generation](std::vector<MonitorEventKind> events, MonitorSnapshot snapshot) {
            @autoreleasepool {
                AppDelegate* strongSelf = weakSelf;
                if (!strongSelf) return;
                [strongSelf enqueueMonitorUpdate:PendingMacUpdate{
                    generation, std::move(events), std::move(snapshot)}];
            }
        }, MonitorWorkerOptions{false, false});
    _monitorWorker->start();
}

- (void)enqueueMonitorUpdate:(PendingMacUpdate)update {
    if (_terminating.load(std::memory_order_acquire)) return;
    (void)_pendingMonitorUpdates.push(std::move(update));
}

- (void)processMonitorUpdates {
    auto updates = _pendingMonitorUpdates.take();
    for (auto& update : updates) [self applyMonitorUpdate:update];
}

- (void)applyMonitorUpdate:(PendingMacUpdate&)update {
    if (update.generation != _monitorGeneration) return;
    const BOOL first = !_hasSnapshot;
    _snapshot = std::move(update.snapshot);
    _hasSnapshot = true;
    const auto effects = apply_monitor_event_policy(
        _visualCoordinator, _snapshot, update.events, _settings);
    for (const auto& effect : effects) {
        if (effect.reveal_pet && _settings.pet_visible) [_petWindow orderFrontRegardless];
        if (effect.sound && _settings.sound_enabled) {
            switch (*effect.sound) {
                case SoundCue::Started: [self playSoundNamed:@"voice-start"]; break;
                case SoundCue::Completed: [self playSoundNamed:@"voice-complete"]; break;
                case SoundCue::Error: [self playSoundNamed:@"voice-error"]; break;
                case SoundCue::Interrupted: [self playSoundNamed:@"voice-interrupted"]; break;
            }
        }
        if (effect.xiaoai_event) {
            [self notifyXiaoAi:*effect.xiaoai_event context:effect.xiaoai_context];
        }
    }
    if (first || !update.events.empty()) [self refreshVisual:YES];
}

- (void)showExpressionDemoState:(int)index now:(Clock::time_point)now {
    static constexpr std::array<ReminderState, 5> states{{
        ReminderState::Idle, ReminderState::Busy, ReminderState::Completed,
        ReminderState::Error, ReminderState::Interrupted
    }};
    _expressionDemoIndex = std::clamp(index, 0, static_cast<int>(states.size()) - 1);
    _expressionDemoNext = now + std::chrono::seconds(4);
    _visualCoordinator = VisualStateCoordinator{};
    _snapshot = MonitorSnapshot{};
    _snapshot.active_titles.clear();
    _snapshot.active_plan_progress_labels.clear();
    _snapshot.active_count = 0;
    const auto state = states[static_cast<std::size_t>(_expressionDemoIndex)];
    switch (state) {
        case ReminderState::Busy:
            _snapshot.active_count = 1;
            _snapshot.active_titles = {"表情测试：正在认真工作"};
            _snapshot.active_plan_progress_labels = {std::optional<std::string>("2/5")};
            _snapshot.total_plan_step_count = 5;
            _snapshot.completed_plan_step_count = 2;
            _visualCoordinator.record_started();
            break;
        case ReminderState::Completed:
            _snapshot.last_completed_title = "表情测试：任务顺利完成";
            _visualCoordinator.record_completed(now, std::chrono::hours(1));
            break;
        case ReminderState::Error:
            _snapshot.last_aborted_title = "表情测试：任务失败";
            _visualCoordinator.record_aborted(now, std::chrono::hours(1));
            break;
        case ReminderState::Interrupted:
            _snapshot.last_interrupted_title = "表情测试：任务被中断";
            _visualCoordinator.record_interrupted(now, std::chrono::hours(1));
            break;
        case ReminderState::Idle:
        default:
            break;
    }
    _dockLastChange = now;
    _dockRevealUntil = now + std::chrono::hours(1);
    _dockVisibility = 1.0;
    [self refreshVisual:YES];
}

- (void)refreshVisual:(BOOL)force {
    const auto now = Clock::now();
    const auto state = _visualCoordinator.select(_snapshot.active_count, now);
    if (force && _dockEdge != DockEdge::None && state != ReminderState::Idle) {
        _dockLastChange = now;
        _dockThoughtUntil = now + std::chrono::seconds(
            app_logic::cloud_notification_seconds(state, _settings.dock_notification_seconds));
        _dockVisibility = 1.0;
    }
    std::string selectedTitle;
    if (_selectedTaskIndex >= 0 &&
        _selectedTaskIndex < static_cast<int>(_renderState.task_titles.size())) {
        selectedTitle = _renderState.task_titles[static_cast<std::size_t>(_selectedTaskIndex)];
    }
    if (_renderState.state != state) {
        _animationTick = 0;
        _animationAccumulator = 0;
        _scrollOffset = 0;
        _scrollHold = 1.9;
        _scrollAtEnd = false;
        _rotationSeconds = 0;
    }

    auto content = make_visual_content(state, _snapshot);
    const bool selectNewest = _visualCoordinator.show_newest_task_on_next_refresh() &&
                              state == ReminderState::Busy;
    const int preferred = app_logic::select_preferred_task_index(
        selectNewest, _visualCoordinator.preferred_task_index());
    _selectedTaskIndex = app_logic::reconcile_task_selection(
        state, content.task_titles, _selectedTaskIndex, selectedTitle, selectNewest, preferred);
    _renderState.state = state;
    _renderState.status_text = std::move(content.status_text);
    _renderState.status_lines = std::move(content.status_lines);
    _renderState.thought_text = std::move(content.thought_text);
    _renderState.task_titles = std::move(content.task_titles);
    _renderState.progress_labels = std::move(content.progress_labels);
    _renderState.selected_task_index = _selectedTaskIndex;
    if (selectNewest) _visualCoordinator.consume_newest_task_focus();

    [self updateRenderGeometry];
    [_petView setNeedsDisplay:YES];
    _statusItem.button.image = [_renderer statusImageForState:state frame:_animationTick / 2];
    _statusItem.button.image.size = NSMakeSize(18, 18);
    _statusItem.button.toolTip = Ns(_renderState.status_text);
}

- (void)updateRenderGeometry {
    NSScreen* screen = _dockEdge != DockEdge::None
        ? ScreenForIdentifier(_dockScreenIdentifier)
        : (_petWindow.screen ? _petWindow.screen : PrimaryScreen());
    if (!screen || !_petWindow) return;
    const NSRect work = screen.visibleFrame;
    _renderState.docked = _dockEdge != DockEdge::None;
    _renderState.bubble_visible = app_logic::should_show_thought_bubble(
        _renderState.docked, _renderState.state, Clock::now(), _dockThoughtUntil);
    _renderState.dock_edge = _dockEdge;
    _renderState.dock_visibility = _dockVisibility;
    _renderState.animation_tick = _animationTick;
    _renderState.selected_task_index = _selectedTaskIndex;
    _renderState.scroll_offset = _scrollOffset;
    _renderState.bubble_below = _renderState.docked && _dockCoordinate >
        NSMaxY(work) - render_layout::dock_bubble_switch_margin;
    _renderState.mirror = !_renderState.docked && NSMidX(_petWindow.frame) < NSMidX(work);
}

- (void)drawPetView:(NSRect)rect { [_renderer drawState:_renderState inRect:rect]; }

- (BOOL)isInteractivePoint:(NSPoint)point {
    if (_dragPending || _dragging) return YES;
    const NSRect rect = _petView ? _petView.bounds : NSMakeRect(0, 0, kPetWindowWidth, kPetWindowHeight);
    if (NSPointInRect(point, PetInteractionBounds(_renderState, rect))) return YES;
    return _renderState.bubble_visible && NSPointInRect(point, BubbleBounds(_renderState, rect));
}

- (void)onTimer:(NSTimer*)timer {
    @autoreleasepool {
    (void)timer;
    if (!_expressionDemo) [self processMonitorUpdates];
    const auto now = Clock::now();
    if (_expressionDemo && now >= _expressionDemoNext) {
        [self showExpressionDemoState:(_expressionDemoIndex + 1) % 5 now:now];
    }
    double elapsed = std::chrono::duration<double>(now - _lastTick).count();
    _lastTick = now;
    elapsed = std::clamp(elapsed, 0.001, 0.25);
    _animationAccumulator += elapsed;
    const int previousAnimationTick = _animationTick;
    while (_animationAccumulator >= 0.12) {
        _animationAccumulator -= 0.12;
        _animationTick = (_animationTick + 1) % 6400;
    }
    bool changed = PetAnimationFrame(_renderState.state, _dockEdge, previousAnimationTick) !=
                   PetAnimationFrame(_renderState.state, _dockEdge, _animationTick);
    const bool statusFrameChanged =
        StatusAnimationFrame(_renderState.state, previousAnimationTick) !=
        StatusAnimationFrame(_renderState.state, _animationTick);

    bool dockChanged = false;
    if (_dockEdge != DockEdge::None) {
        const NSPoint cursor = NSEvent.mouseLocation;
        const BOOL hoveringNow = [self dockHovering:cursor];
        const BOOL hoveringPath = !hoveringNow && _hasLastHoverCursor &&
            [self dockHoverPathCrossesFrom:_lastHoverCursor to:cursor];
        if (hoveringNow || hoveringPath) {
            _dockRevealUntil = now + std::chrono::seconds(_settings.dock_reveal_seconds);
        }
        _hasLastHoverCursor = true;
        _lastHoverCursor = cursor;
        const BOOL show = app_logic::should_show_dock(_dockLastChange, now,
            _dragPending || _dragging, hoveringNow || hoveringPath,
            _dockRevealUntil, _settings.dock_idle_hide_seconds);
        const double target = show ? 1.0 : 0.0;
        if (std::abs(target - _dockVisibility) < .001) {
            dockChanged = _dockVisibility != target;
            _dockVisibility = target;
        } else {
            _dockVisibility += (target > _dockVisibility ? 1 : -1) * elapsed /
                (target > _dockVisibility ? .30 : .55);
            _dockVisibility = std::clamp(_dockVisibility, 0.0, 1.0);
            dockChanged = true;
        }
    } else {
        _hasLastHoverCursor = false;
    }

    const BOOL previousBubble = _renderState.bubble_visible;
    _renderState.bubble_visible = app_logic::should_show_thought_bubble(
        _dockEdge != DockEdge::None, _renderState.state, now, _dockThoughtUntil);
    const BOOL bubble = _renderState.bubble_visible;
    const BOOL bubbleChanged = bubble != previousBubble;
    if (!bubble) [_renderer discardCloudImage];
    if (bubble) {
        if (_renderState.task_titles.size() > 1) {
            _rotationSeconds += elapsed;
            if (_rotationSeconds >= 6.0) {
                const auto steps = std::max(1, static_cast<int>(_rotationSeconds / 6.0));
                _rotationSeconds -= steps * 6.0;
                _selectedTaskIndex = (_selectedTaskIndex + steps) %
                    static_cast<int>(_renderState.task_titles.size());
                _scrollOffset = 0;
                _scrollHold = 1.9;
                _scrollAtEnd = false;
                changed = true;
            }
        } else {
            _rotationSeconds = 0;
        }
        const std::string& text = _renderState.task_titles.empty() ? _renderState.thought_text
            : _renderState.task_titles[std::clamp(_selectedTaskIndex, 0,
                                                   static_cast<int>(_renderState.task_titles.size()) - 1)];
        const int lines = std::max(1, static_cast<int>(text.size() / 24) +
            static_cast<int>(std::count(text.begin(), text.end(), '\n')));
        const double maxScroll = std::max(0.0, (lines - 3) * 15.0);
        if (maxScroll > 0) {
            if (_scrollHold > 0) _scrollHold = std::max(0.0, _scrollHold - elapsed);
            else if (!_scrollAtEnd) {
                _scrollOffset = std::min(maxScroll, _scrollOffset + 15 * elapsed);
                if (_scrollOffset >= maxScroll) { _scrollAtEnd = true; _scrollHold = 1.7; }
                changed = true;
            } else {
                _scrollOffset = 0;
                _scrollHold = 1.9;
                _scrollAtEnd = false;
                changed = true;
            }
        } else if (_scrollOffset != 0) {
            _scrollOffset = 0;
            changed = true;
        }
    } else {
        _rotationSeconds = 0;
        if (_scrollOffset != 0) { _scrollOffset = 0; changed = true; }
    }
    const auto nextState = _visualCoordinator.select(_snapshot.active_count, now);
    if (nextState != _renderState.state) { [self refreshVisual:YES]; return; }
    _renderState.animation_tick = _animationTick;
    if (changed || dockChanged || bubbleChanged) [self updateRenderGeometry];
    [self updateMousePassThrough];
    if (changed || dockChanged || bubbleChanged) [_petView setNeedsDisplay:YES];
    if (statusFrameChanged) {
        _statusItem.button.image = [_renderer statusImageForState:_renderState.state frame:_animationTick / 2];
        _statusItem.button.image.size = NSMakeSize(18, 18);
    }
    }
}

- (void)applicationDidChangeScreenParameters:(NSNotification*)notification {
    (void)notification;
    if (!_petWindow) return;
    if (_dockEdge == DockEdge::None) [self clampToWorkArea];
    else [self updateDockWindowPosition];
    [self updateRenderGeometry];
    [_petView setNeedsDisplay:YES];
    [self savePosition];
}

- (void)placeDefault {
    NSScreen* screen = PrimaryScreen();
    if (!screen) return;
    const NSRect work = screen.visibleFrame;
    const CGFloat x = ClampOrigin(NSMaxX(work) - kPetWindowWidth - 24,
                                  NSMinX(work), NSMaxX(work), kPetWindowWidth);
    const CGFloat y = ClampOrigin(NSMinY(work) + 24,
                                  NSMinY(work), NSMaxY(work), kPetWindowHeight);
    [_petWindow setFrame:NSMakeRect(x, y, kPetWindowWidth, kPetWindowHeight) display:NO];
}

- (void)restorePosition {
    if (!_settings.pet_position) return;
    const PetPositionState saved = *_settings.pet_position;
    NSScreen* screen = ScreenForIdentifier(saved.screen_identifier);
    if (!screen) return;
    const NSRect work = screen.visibleFrame;
    _dockEdge = saved.dock_edge;
    _dockScreenIdentifier = ScreenIdentifier(screen);
    _dockVisibility = 1.0;
    if (_dockEdge != DockEdge::None) {
        _dockCoordinate = NSMaxY(work) - saved.relative_y * NSHeight(work);
        [self updateDockWindowPosition];
        return;
    }

    _dockScreenIdentifier.clear();
    const CGFloat anchorX = NSMinX(work) + saved.relative_x * NSWidth(work);
    const CGFloat anchorY = NSMaxY(work) - saved.relative_y * NSHeight(work);
    const CGFloat x = ClampOrigin(anchorX - kPetWindowWidth / 2,
                                  NSMinX(work), NSMaxX(work), kPetWindowWidth);
    const CGFloat y = ClampOrigin(anchorY,
                                  NSMinY(work), NSMaxY(work), kPetWindowHeight);
    [_petWindow setFrame:NSMakeRect(x, y, kPetWindowWidth, kPetWindowHeight) display:NO];
}

- (void)savePosition {
    if (!_petWindow) return;
    const NSRect frame = _petWindow.frame;
    NSScreen* screen = _dockEdge == DockEdge::None
        ? ScreenAtPoint(NSMakePoint(NSMidX(frame), NSMinY(frame)))
        : ScreenForIdentifier(_dockScreenIdentifier);
    if (!screen) return;
    const NSRect work = screen.visibleFrame;
    const CGFloat width = std::max<CGFloat>(1, NSWidth(work));
    const CGFloat height = std::max<CGFloat>(1, NSHeight(work));
    PetPositionState position;
    position.dock_edge = _dockEdge;
    position.screen_identifier = ScreenIdentifier(screen);
    if (_dockEdge == DockEdge::None) {
        position.relative_x = (NSMidX(frame) - NSMinX(work)) / width;
        position.relative_y = (NSMaxY(work) - NSMinY(frame)) / height;
    } else {
        position.relative_x = _dockEdge == DockEdge::Left ? 0.0 : 1.0;
        position.relative_y = (NSMaxY(work) - _dockCoordinate) / height;
    }
    position.normalize();
    _settings.pet_position = position;
    [self saveSettings];
}

- (void)clampToWorkArea {
    if (!_petWindow) return;
    if (_dockEdge != DockEdge::None) {
        [self updateDockWindowPosition];
        return;
    }
    const NSRect frame = _petWindow.frame;
    NSScreen* screen = ScreenAtPoint(NSMakePoint(NSMidX(frame), NSMidY(frame)));
    if (!screen) return;
    const NSRect work = screen.visibleFrame;
    const CGFloat x = ClampOrigin(NSMinX(frame), NSMinX(work), NSMaxX(work), NSWidth(frame));
    const CGFloat y = ClampOrigin(NSMinY(frame), NSMinY(work), NSMaxY(work), NSHeight(frame));
    [_petWindow setFrameOrigin:NSMakePoint(x, y)];
}

- (void)updateDockWindowPosition {
    if (_dockEdge == DockEdge::None || !_petWindow) return;
    NSScreen* screen = ScreenForIdentifier(_dockScreenIdentifier);
    if (!screen) return;
    _dockScreenIdentifier = ScreenIdentifier(screen);
    const NSRect work = screen.visibleFrame;
    _dockCoordinate = std::clamp(_dockCoordinate, NSMinY(work), NSMaxY(work));
    const bool bubbleBelow = _dockCoordinate >
        NSMaxY(work) - render_layout::dock_bubble_switch_margin;
    const render_layout::State layout{_renderState.state, _dockEdge, true, bubbleBelow,
                                      false, _dockVisibility, _animationTick};
    const CGFloat visibleCenterOffset = static_cast<CGFloat>(render_layout::logical_height -
        render_layout::dock_pet_center_y(layout));
    const CGFloat x = _dockEdge == DockEdge::Left
        ? NSMinX(work) : NSMaxX(work) - kPetWindowWidth;
    const CGFloat y = ClampOrigin(_dockCoordinate - visibleCenterOffset,
                                  NSMinY(work), NSMaxY(work), kPetWindowHeight);
    [_petWindow setFrame:NSMakeRect(x, y, kPetWindowWidth, kPetWindowHeight) display:NO];
}

- (void)trySnapOrClamp:(NSPoint)cursor {
    NSScreen* screen = ScreenAtPoint(cursor);
    if (!screen) { [self clampToWorkArea]; return; }
    const NSRect work = screen.visibleFrame;
    const DockEdge edge = app_logic::select_snap_edge(
        PointD{cursor.x, cursor.y},
        RectD{NSMinX(work), NSMinY(work), NSWidth(work), NSHeight(work)}, 36.0);
    if (edge == DockEdge::None) {
        _dockEdge = DockEdge::None;
        _dockScreenIdentifier.clear();
        _dockVisibility = 1.0;
        _dockThoughtUntil = Clock::time_point::min();
        _dockRevealUntil = Clock::time_point::min();
        _hasLastHoverCursor = false;
        [self clampToWorkArea];
    } else {
        _dockEdge = edge;
        _dockScreenIdentifier = ScreenIdentifier(screen);
        _dockCoordinate = cursor.y;
        _dockVisibility = 1.0;
        _dockLastChange = Clock::now();
        _dockRevealUntil = Clock::time_point::min();
        _hasLastHoverCursor = false;
        [self updateDockWindowPosition];
    }
    [self refreshVisual:YES];
}

- (BOOL)dockHovering:(NSPoint)cursor {
    if (_dockEdge == DockEdge::None) return NO;
    NSScreen* screen = ScreenForIdentifier(_dockScreenIdentifier);
    if (!screen || !_petWindow) return NO;
    const NSRect work = screen.visibleFrame;
    const RectD trigger = app_logic::dock_hover_bounds(
        _dockEdge, RectD{NSMinX(work), NSMinY(work), NSWidth(work), NSHeight(work)},
        _dockCoordinate, 1.0, _dockVisibility <= 0.01, _settings.dock_hover_height);
    if (trigger.contains(PointD{cursor.x, cursor.y})) return YES;

    const bool bubbleBelow = _dockCoordinate >
        NSMaxY(work) - render_layout::dock_bubble_switch_margin;
    const render_layout::State layout{_renderState.state, _dockEdge, true, bubbleBelow,
                                      false, _dockVisibility, _animationTick};
    const auto pet = render_layout::visible_pet_bounds(layout);
    const NSRect frame = _petWindow.frame;
    const NSRect petGlobal = NSMakeRect(NSMinX(frame) + pet.x,
        NSMinY(frame) + render_layout::logical_height - pet.y - pet.height,
        pet.width, pet.height);
    return NSPointInRect(cursor, petGlobal);
}

- (BOOL)dockHoverPathCrossesFrom:(NSPoint)from to:(NSPoint)to {
    if (_dockEdge == DockEdge::None) return NO;
    NSScreen* screen = ScreenForIdentifier(_dockScreenIdentifier);
    if (!screen) return NO;
    const NSRect work = screen.visibleFrame;
    const RectD trigger = app_logic::dock_hover_bounds(
        _dockEdge, RectD{NSMinX(work), NSMinY(work), NSWidth(work), NSHeight(work)},
        _dockCoordinate, 1.0, _dockVisibility <= 0.01, _settings.dock_hover_height);
    if (app_logic::segment_intersects_rect(PointD{from.x, from.y}, PointD{to.x, to.y}, trigger)) {
        return YES;
    }
    return [self dockHovering:from] || [self dockHovering:to];
}

- (void)revealDock {
    if (_dockEdge == DockEdge::None) return;
    const auto now = Clock::now();
    _dockVisibility = 1.0;
    _dockLastChange = now;
    _dockThoughtUntil = _renderState.state == ReminderState::Idle
        ? Clock::time_point::min()
        : now + std::chrono::seconds(app_logic::cloud_notification_seconds(
            _renderState.state, _settings.dock_notification_seconds));
    _dockRevealUntil = now + std::chrono::seconds(_settings.dock_reveal_seconds);
    [self updateRenderGeometry];
    [_petView setNeedsDisplay:YES];
}

- (void)updateMousePassThrough {
    if (!_petWindow || !_petWindow.isVisible) return;
    const NSPoint cursor = NSEvent.mouseLocation;
    const BOOL hovering = _dockEdge != DockEdge::None && [self dockHovering:cursor];
    const NSPoint windowPoint = [_petWindow convertPointFromScreen:cursor];
    const NSPoint local = [_petView convertPoint:windowPoint fromView:nil];
    const BOOL receive = _dragPending || _dragging || hovering || [self isInteractivePoint:local];
    const NSInteger next = receive ? 1 : 0;
    if (_lastMouseReception == next) return;
    _lastMouseReception = next;
    _petWindow.ignoresMouseEvents = !receive;
}

- (void)petMouseDown:(NSEvent*)event {
    const NSPoint local = [_petView convertPoint:event.locationInWindow fromView:nil];
    if (![self isInteractivePoint:local]) return;
    const BOOL bubbleVisible = _renderState.bubble_visible;
    const auto layout = LayoutStateFor(_renderState);
    const RectD bubble = render_layout::bubble_bounds(layout);
    const RectD content = render_layout::body_bounds(layout);
    if (app_logic::is_task_switch_point(_dockEdge != DockEdge::None, bubbleVisible,
            _renderState.state, static_cast<int>(_renderState.task_titles.size()),
            bubble, content, PointD{local.x, local.y})) {
        if (_renderState.task_titles.size() > 1) {
            _selectedTaskIndex = (_selectedTaskIndex + 1) %
                static_cast<int>(_renderState.task_titles.size());
            _scrollOffset = 0; _scrollHold = 1.9; _scrollAtEnd = false;
            _rotationSeconds = 0;
            [self revealDock];
            [self refreshVisual:YES];
        }
        return;
    }

    _petWindow.ignoresMouseEvents = NO;
    _lastMouseReception = 1;
    [_petWindow makeFirstResponder:_petView];
    _dragPending = true;
    _dragging = false;
    _dragStartedDocked = _dockEdge != DockEdge::None;
    _dragStartCursor = NSEvent.mouseLocation;
    _lastCursor = _dragStartCursor;
    _dragStartOrigin = _petWindow.frame.origin;
    if (_dragStartedDocked) [self revealDock];
}

- (void)petMouseDragged:(NSEvent*)event {
    (void)event;
    if (!_dragPending) return;
    const NSPoint cursor = NSEvent.mouseLocation;
    _lastCursor = cursor;
    CGFloat dx = cursor.x - _dragStartCursor.x;
    CGFloat dy = cursor.y - _dragStartCursor.y;
    if (!_dragging && std::abs(dx) + std::abs(dy) < 4) return;
    if (!_dragging && _dockEdge != DockEdge::None) {
        _dockEdge = DockEdge::None;
        _dockScreenIdentifier.clear();
        _dockVisibility = 1.0;
        _dockThoughtUntil = Clock::time_point::min();
        _dockRevealUntil = Clock::time_point::min();
        _hasLastHoverCursor = false;
        _dragStartCursor = cursor;
        _dragStartOrigin = NSMakePoint(cursor.x - kPetWindowWidth / 2, cursor.y - 70);
        [_petWindow setFrameOrigin:_dragStartOrigin];
        dx = 0; dy = 0;
    }
    _dragging = true;
    [_petWindow setFrameOrigin:NSMakePoint(_dragStartOrigin.x + dx, _dragStartOrigin.y + dy)];
    [self updateRenderGeometry];
    [_petView setNeedsDisplay:YES];
}

- (void)petMouseUp:(NSEvent*)event {
    (void)event;
    if (!_dragPending) return;
    const BOOL moved = _dragging;
    const BOOL startedDocked = _dragStartedDocked;
    const NSPoint cursor = NSEvent.mouseLocation;
    _dragPending = false;
    _dragging = false;
    _dragStartedDocked = false;
    if (!moved) {
        if (startedDocked && _dockEdge != DockEdge::None) [self revealDock];
        return;
    }
    [self trySnapOrClamp:cursor];
    [self savePosition];
    [self updateMousePassThrough];
}

- (void)showSettings:(id)sender {
    (void)sender;
    if (!_settingsWindow) {
        _settingsWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 620)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        _settingsWindow.title = @"CodeXPets 设置";
        _settingsWindow.releasedWhenClosed = NO;
        _settingsWindow.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace;
        NSView* content = _settingsWindow.contentView;

        NSTextField* heading = MakeLabel(@"桌面宠物与会话监听", NSMakeRect(22, 572, 380, 28));
        heading.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
        [content addSubview:heading];

        const CGFloat labelX = 22, fieldX = 270;
        [content addSubview:MakeLabel(@"边缘唤出区域高度（像素）", NSMakeRect(labelX, 526, 235, 24))];
        [content addSubview:MakeLabel(@"吸附隐藏（秒，0=关闭）", NSMakeRect(labelX, 486, 235, 24))];
        [content addSubview:MakeLabel(@"鼠标唤出后保持（秒）", NSMakeRect(labelX, 446, 235, 24))];
        [content addSubview:MakeLabel(@"任务状态云朵保持（秒）", NSMakeRect(labelX, 406, 235, 24))];
        [content addSubview:MakeLabel(@"Codex 会话目录", NSMakeRect(labelX, 363, 235, 24))];
        _settingsHoverField = MakeTextField(@"", NSMakeRect(fieldX, 522, 130, 28));
        _settingsIdleField = MakeTextField(@"", NSMakeRect(fieldX, 482, 130, 28));
        _settingsRevealField = MakeTextField(@"", NSMakeRect(fieldX, 442, 130, 28));
        _settingsNotificationField = MakeTextField(@"", NSMakeRect(fieldX, 402, 130, 28));
        _settingsRootField = MakeTextField(@"", NSMakeRect(fieldX, 359, 242, 28));
        for (NSTextField* field in @[_settingsHoverField, _settingsIdleField,
                                    _settingsRevealField, _settingsNotificationField,
                                    _settingsRootField]) [content addSubview:field];
        [content addSubview:MakeButton(@"浏览…", self, @selector(browseSessionsRoot:),
                                       NSMakeRect(520, 358, 78, 30))];
        _settingsSoundButton = [NSButton checkboxWithTitle:@"播放开始、完成和异常语音提醒"
                                                    target:nil action:nil];
        _settingsSoundButton.frame = NSMakeRect(22, 320, 390, 28);
        [content addSubview:_settingsSoundButton];

        _settingsXiaoAiEnabledButton = [NSButton checkboxWithTitle:@"启用小爱音箱主动播报"
                                                              target:nil action:nil];
        _settingsXiaoAiEnabledButton.frame = NSMakeRect(22, 278, 300, 28);
        [content addSubview:_settingsXiaoAiEnabledButton];
        [content addSubview:MakeLabel(@"并发播报数（1-8）", NSMakeRect(360, 280, 140, 24))];
        _settingsXiaoAiParallelField = MakeTextField(@"", NSMakeRect(510, 276, 88, 28));
        [content addSubview:_settingsXiaoAiParallelField];
        [content addSubview:MakeLabel(@"目标音箱（可多选）", NSMakeRect(22, 238, 220, 24))];
        _settingsXiaoAiSelectAllButton = [NSButton checkboxWithTitle:@"全选" target:self
                                                               action:@selector(toggleXiaoAiAllDevices:)];
        _settingsXiaoAiSelectAllButton.frame = NSMakeRect(270, 234, 100, 28);
        [content addSubview:_settingsXiaoAiSelectAllButton];
        NSScrollView* devicesScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(270, 132, 328, 94)];
        devicesScroll.hasVerticalScroller = YES;
        devicesScroll.borderType = NSBezelBorder;
        _settingsXiaoAiDevicesStack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, 300, 94)];
        _settingsXiaoAiDevicesStack.orientation = NSUserInterfaceLayoutOrientationVertical;
        _settingsXiaoAiDevicesStack.alignment = NSLayoutAttributeLeading;
        _settingsXiaoAiDevicesStack.spacing = 2;
        devicesScroll.documentView = _settingsXiaoAiDevicesStack;
        [content addSubview:devicesScroll];
        _settingsXiaoAiLoginButton = MakeButton(@"浏览器登录", self, @selector(openXiaoAiLogin:),
                                                  NSMakeRect(22, 92, 108, 30));
        [content addSubview:_settingsXiaoAiLoginButton];
        _settingsXiaoAiScanButton = MakeButton(@"扫描设备", self, @selector(scanXiaoAiDevices:),
                                                NSMakeRect(142, 92, 108, 30));
        [content addSubview:_settingsXiaoAiScanButton];
        _settingsXiaoAiTestButton = MakeButton(@"测试播报", self, @selector(testXiaoAi:),
                                                NSMakeRect(262, 92, 108, 30));
        [content addSubview:_settingsXiaoAiTestButton];
        _settingsXiaoAiStatusField = MakeLabel(@"", NSMakeRect(382, 94, 216, 28));
        _settingsXiaoAiStatusField.textColor = NSColor.secondaryLabelColor;
        [content addSubview:_settingsXiaoAiStatusField];

        NSTextField* hint = MakeLabel(
            @"打开设置会自动加载音箱；列表中的复选框可连续选择多台。CodeXPets 仅增量读取 JSONL 会话文件；"
             @"小米授权只保存在系统钥匙串中。",
            NSMakeRect(22, 50, 576, 36));
        hint.textColor = NSColor.secondaryLabelColor;
        hint.lineBreakMode = NSLineBreakByWordWrapping;
        hint.maximumNumberOfLines = 2;
        [content addSubview:hint];
        [content addSubview:MakeButton(@"恢复默认", self, @selector(restoreSettingsDefaults:),
                                       NSMakeRect(22, 20, 92, 32))];
        [content addSubview:MakeButton(@"取消", self, @selector(cancelSettings:),
                                       NSMakeRect(414, 20, 82, 32))];
        NSButton* save = MakeButton(@"保存", self, @selector(applySettings:),
                                    NSMakeRect(506, 20, 92, 32));
        save.keyEquivalent = @"\r";
        [content addSubview:save];
        [_settingsWindow center];
    }

    _settingsHoverField.integerValue = _settings.dock_hover_height;
    _settingsIdleField.integerValue = _settings.dock_idle_hide_seconds;
    _settingsRevealField.integerValue = _settings.dock_reveal_seconds;
    _settingsNotificationField.integerValue = _settings.dock_notification_seconds;
    _settingsRootField.stringValue = Ns(path_to_utf8(_settings.sessions_root));
    _settingsSoundButton.state = _settings.sound_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    _settingsXiaoAiEnabledButton.state = _settings.xiaoai.enabled
        ? NSControlStateValueOn : NSControlStateValueOff;
    _settingsXiaoAiParallelField.integerValue = _settings.xiaoai.max_parallel_requests;
    [self populateXiaoAiDevices];
    if (_settings.xiaoai.auth_cookies.empty()) {
        _settingsXiaoAiStatusField.stringValue = @"登录小米账号后自动加载音箱";
    } else if (!_xiaoaiOperationInFlight) {
        _settingsXiaoAiStatusField.stringValue = @"准备加载音箱列表…";
    }
    [self setXiaoAiOperationInFlight:_xiaoaiOperationInFlight];
    [_settingsWindow makeKeyAndOrderFront:nil];
    if (!_settings.xiaoai.auth_cookies.empty()) [self loadXiaoAiDevicesShowingErrors:NO];
}

- (void)browseSessionsRoot:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"选择 Codex sessions 目录";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    NSString* current = _settingsRootField.stringValue;
    if (current.length > 0) panel.directoryURL = [NSURL fileURLWithPath:current];
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        _settingsRootField.stringValue = panel.URL.path;
    }
}

- (void)restoreSettingsDefaults:(id)sender {
    (void)sender;
    AppSettings defaults;
    defaults.normalize();
    _settingsHoverField.integerValue = defaults.dock_hover_height;
    _settingsIdleField.integerValue = defaults.dock_idle_hide_seconds;
    _settingsRevealField.integerValue = defaults.dock_reveal_seconds;
    _settingsNotificationField.integerValue = defaults.dock_notification_seconds;
    _settingsRootField.stringValue = Ns(path_to_utf8(defaults.sessions_root));
    _settingsSoundButton.state = defaults.sound_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    _settingsXiaoAiEnabledButton.state = NSControlStateValueOff;
    _settingsXiaoAiParallelField.integerValue = defaults.xiaoai.max_parallel_requests;
    _settingsXiaoAiSelectAllButton.state = NSControlStateValueOff;
    [self toggleXiaoAiAllDevices:_settingsXiaoAiSelectAllButton];
}

- (void)cancelSettings:(id)sender {
    (void)sender;
    [_settingsWindow orderOut:nil];
}

- (void)applySettings:(id)sender {
    (void)sender;
    AppSettings next = _settings;
    next.dock_hover_height = static_cast<int>(_settingsHoverField.integerValue);
    next.dock_idle_hide_seconds = static_cast<int>(_settingsIdleField.integerValue);
    next.dock_reveal_seconds = static_cast<int>(_settingsRevealField.integerValue);
    next.dock_notification_seconds = static_cast<int>(_settingsNotificationField.integerValue);
    next.sessions_root = path_from_utf8(Utf8(_settingsRootField.stringValue));
    next.sound_enabled = _settingsSoundButton.state == NSControlStateValueOn;
    next.xiaoai.enabled = _settingsXiaoAiEnabledButton.state == NSControlStateValueOn;
    next.xiaoai.max_parallel_requests = static_cast<int>(_settingsXiaoAiParallelField.integerValue);
    next.xiaoai.auth_cookies = _settings.xiaoai.auth_cookies;
    next.xiaoai.device_ids = [self selectedXiaoAiDeviceIds];
    next.xiaoai.device_id = next.xiaoai.device_ids.empty() ? std::string{} : next.xiaoai.device_ids.front();
    next.normalize();
    const bool rootChanged = next.sessions_root != _settings.sessions_root;
    _settings = std::move(next);
    if (_xiaoaiNotifier) _xiaoaiNotifier->configure(_settings.xiaoai);
    [self saveSettings];
    if (rootChanged) {
        if (_monitorWorker) _monitorWorker->stop();
        _monitorWorker.reset();
        _snapshot = MonitorSnapshot{};
        _hasSnapshot = false;
        _visualCoordinator = VisualStateCoordinator{};
        _pendingMonitorUpdates.clear();
        [self startMonitor];
    }
    [_settingsWindow orderOut:nil];
    [self refreshVisual:YES];
}

@end

namespace {

bool HasUtilityArgument(int argc, const char* const* argv, std::string_view wanted) {
    for (int index = 1; index < argc; ++index) {
        if (ArgumentAt(argc, argv, index) == wanted) return true;
    }
    return false;
}

int RunMacUtility(int argc, const char* const* argv) {
    if (HasUtilityArgument(argc, argv, "--version")) {
        std::cout << CODEXPETS_VERSION << '\n';
        return 0;
    }

    [NSApplication sharedApplication];
    MacRenderer* renderer = [[MacRenderer alloc] init];
    NSString* validationError = nil;
    if (![renderer validate:&validationError]) {
        std::cerr << "resources: " << Utf8(validationError ? validationError : @"unknown error") << '\n';
        return 1;
    }
    if (HasUtilityArgument(argc, argv, "--validate-resources")) {
        std::cout << "resources: ok\n";
        return 0;
    }

    if (HasUtilityArgument(argc, argv, "--smoke-test") ||
        HasUtilityArgument(argc, argv, "--preview")) {
        std::filesystem::path outputDirectory;
        bool temporary = true;
        for (int index = 1; index < argc; ++index) {
            if (ArgumentAt(argc, argv, index) == "--preview" && index + 1 < argc) {
                outputDirectory = path_from_utf8(ArgumentAt(argc, argv, index + 1));
                temporary = false;
                break;
            }
        }
        if (outputDirectory.empty()) {
            outputDirectory = std::filesystem::temp_directory_path() /
                ("codexpets-smoke-" + std::to_string(getpid()));
        }
        std::error_code ioError;
        std::filesystem::create_directories(outputDirectory, ioError);
        if (ioError) { std::cerr << ioError.message() << '\n'; return 1; }

        MacRenderState state;
        MonitorSnapshot previewSnapshot;
        previewSnapshot.active_count = 1;
        previewSnapshot.active_titles = {"原生渲染检查"};
        previewSnapshot.active_plan_progress_labels = {std::optional<std::string>("1/3")};
        previewSnapshot.completed_plan_step_count = 1;
        previewSnapshot.total_plan_step_count = 3;
        previewSnapshot.last_completed_title = "原生渲染检查：任务已完成";
        previewSnapshot.last_aborted_title = "原生渲染检查：模拟异常";
        previewSnapshot.last_interrupted_title = "原生渲染检查：任务已中断";
        const auto configurePreviewState = [&](ReminderState visual) {
            MonitorSnapshot snapshot = previewSnapshot;
            if (visual == ReminderState::Idle) {
                snapshot.active_count = 0;
                snapshot.active_titles.clear();
                snapshot.active_plan_progress_labels.clear();
                snapshot.completed_plan_step_count = 0;
                snapshot.total_plan_step_count = 0;
            }
            const auto content = make_visual_content(visual, snapshot);
            state.state = visual;
            state.status_text = content.status_text;
            state.thought_text = content.thought_text;
            state.task_titles = content.task_titles;
            state.progress_labels = content.progress_labels;
            state.selected_task_index = 0;
            state.scroll_offset = 0;
            state.animation_tick = (visual == ReminderState::Busy ||
                                    visual == ReminderState::Completed) ? 18 : 0;
        };
        const std::array<std::pair<ReminderState, const char*>, 5> states{{
            {ReminderState::Idle, "idle"}, {ReminderState::Busy, "busy"},
            {ReminderState::Completed, "completed"}, {ReminderState::Error, "error"},
            {ReminderState::Interrupted, "interrupted"}}};
        NSString* renderError = nil;
        for (const auto& [visual, name] : states) {
            configurePreviewState(visual);
            const auto path = outputDirectory / (std::string(name) + ".png");
            if (![renderer savePreview:state toPath:Ns(path_to_utf8(path)) error:&renderError]) {
                std::cerr << Utf8(renderError ? renderError : @"render failed") << '\n';
                if (temporary) std::filesystem::remove_all(outputDirectory, ioError);
                return 1;
            }
        }
        state.docked = true;
        for (const auto& [visual, name] : states) {
            configurePreviewState(visual);
            for (const auto edge : {DockEdge::Left, DockEdge::Right}) {
                state.dock_edge = edge;
                const auto side = edge == DockEdge::Left ? "left" : "right";
                const auto path = outputDirectory /
                    (std::string("dock-") + side + "-" + name + ".png");
                if (![renderer savePreview:state toPath:Ns(path_to_utf8(path))
                                      error:&renderError]) return 1;
            }
        }
        if (temporary) std::filesystem::remove_all(outputDirectory, ioError);
        std::cout << (temporary ? "smoke-test: ok\n" : "preview: ok\n");
        return 0;
    }

    if (HasUtilityArgument(argc, argv, "--test-sound")) {
        for (NSString* name in @[@"voice-start", @"voice-complete", @"voice-error", @"voice-interrupted"]) {
            NSString* path = ResourcePath(@"audio", name, @"mp3");
            NSSound* sound = path ? [[NSSound alloc] initWithContentsOfFile:path byReference:NO] : nil;
            if (!sound || ![sound play]) {
                std::cerr << "sound: unable to play " << Utf8(name) << '\n';
                return 1;
            }
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:15];
            while (sound.isPlaying && deadline.timeIntervalSinceNow > 0) {
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                       beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
            [sound stop];
        }
        std::cout << "sound: ok\n";
        return 0;
    }
    return 0;
}

bool AnotherInstanceIsRunning() {
    NSString* identifier = NSBundle.mainBundle.bundleIdentifier;
    if (identifier.length == 0) return false;
    const pid_t current = getpid();
    for (NSRunningApplication* application in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:identifier]) {
        if (!application.terminated && application.processIdentifier != current) return true;
    }
    return false;
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        bool utility = false;
        bool expressionDemo = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = ArgumentAt(argc, argv, index);
            if (argument == "--version" || argument == "--preview" ||
                argument == "--validate-resources" || argument == "--smoke-test" ||
                argument == "--test-sound") {
                utility = true;
                break;
            }
            if (argument == "--expression-demo") expressionDemo = true;
        }
        if (utility) return RunMacUtility(argc, argv);
        [NSApplication sharedApplication];
        if (AnotherInstanceIsRunning()) {
            std::cerr << "CodeXPets 已经在菜单栏里运行。\n";
            return 0;
        }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        [delegate setExpressionDemoEnabled:expressionDemo];
        NSApp.delegate = delegate;
        [NSApp run];
        (void)delegate;
    }
    return 0;
}
