#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ServiceManagement/ServiceManagement.h>
#import <dispatch/dispatch.h>
#import <mach/mach.h>

#include <unistd.h>

#include "app_logic.h"
#include "paths.h"
#include "platform_text.h"
#include "session_monitor.h"
#include "settings.h"
#include "visual_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

CGFloat SmoothStep(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return static_cast<CGFloat>(value * value * (3.0 - 2.0 * value));
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

constexpr CGFloat kPetWindowWidth = 420;
constexpr CGFloat kPetWindowHeight = 260;

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

const char* ArchitectureName() {
#if defined(__arm64__) || defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string ArgumentAt(int argc, const char* const* argv, int index) {
    return index >= 0 && index < argc && argv[index] ? std::string(argv[index]) : std::string{};
}

} // namespace

@interface MacRenderer : NSObject
- (BOOL)validate:(NSString* __autoreleasing*)error;
- (void)drawState:(const MacRenderState&)state inRect:(NSRect)rect;
- (BOOL)savePreview:(const MacRenderState&)state toPath:(NSString*)path error:(NSString* __autoreleasing*)error;
- (NSImage*)statusImageForState:(ReminderState)state frame:(int)frame;
@end

@implementation MacRenderer {
    NSMutableDictionary<NSString*, NSImage*>* _cache;
    int _floatingRow;
    int _dockSide;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _cache = [NSMutableDictionary dictionary];
        _floatingRow = -1;
        _dockSide = -1;
    }
    return self;
}

- (NSImage*)imageInDirectory:(NSString*)directory name:(NSString*)name {
    NSString* key = [NSString stringWithFormat:@"%@/%@", directory, name];
    NSImage* image = _cache[key];
    if (image) return image;
    NSString* path = ResourcePath(directory, name, @"png");
    if (!path) return nil;
    image = [[NSImage alloc] initWithContentsOfFile:path];
    if (image) _cache[key] = image;
    return image;
}

- (NSImage*)floatingImageForState:(ReminderState)state frame:(int)frame {
    const int row = app_logic::select_floating_sprite_row(state);
    if (_floatingRow != row) {
        NSArray* keys = [_cache.allKeys copy];
        for (NSString* key in keys) if ([key hasPrefix:@"floating/"]) [_cache removeObjectForKey:key];
        _floatingRow = row;
    }
    NSString* name = [NSString stringWithFormat:@"%s-%d", StateName(state).c_str(), std::clamp(frame, 0, 7)];
    return [self imageInDirectory:@"floating" name:name];
}

- (NSImage*)dockImage:(int)frame {
    // app_logic::select_dock_sprite_index returns 0..9 for the left edge and
    // 10..19 for the right edge. Keep all 20 shared dock frames available.
    frame = std::clamp(frame, 0, 19);
    const int side = frame >= 10 ? 1 : 0;
    if (_dockSide != side) {
        NSArray* keys = [_cache.allKeys copy];
        for (NSString* key in keys) if ([key hasPrefix:@"dock/"]) [_cache removeObjectForKey:key];
        _dockSide = side;
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
            NSImage* image = [self imageInDirectory:@"floating"
                                               name:[NSString stringWithFormat:@"%s-%d", state, frame]];
            if (!image || std::lround(image.size.width) != 192 || std::lround(image.size.height) != 208) {
                if (error) *error = @"浮动精灵资源缺失或尺寸错误";
                return NO;
            }
        }
    }
    for (int frame = 0; frame < 20; ++frame) {
        NSImage* image = [self dockImage:frame];
        if (!image || std::lround(image.size.width) != 256 || std::lround(image.size.height) != 256) {
            if (error) *error = @"扒边精灵资源缺失或尺寸错误";
            return NO;
        }
    }
    for (NSString* name in @[@"status-idle", @"status-completed", @"status-error",
                             @"status-busy-0", @"status-busy-1", @"status-busy-2", @"status-busy-3",
                             @"status-busy-4", @"status-busy-5", @"status-busy-6", @"status-busy-7"]) {
        if (![self imageInDirectory:@"icons" name:name]) {
            if (error) *error = @"状态图标资源缺失";
            return NO;
        }
    }
    for (NSString* name in @[@"voice-start", @"voice-complete", @"voice-error"]) {
        if (!ResourcePath(@"audio", name, @"mp3")) {
            if (error) *error = @"语音资源缺失";
            return NO;
        }
    }
    NSArray* keys = [_cache.allKeys copy];
    for (NSString* key in keys) {
        if ([key hasPrefix:@"floating/"] || [key hasPrefix:@"dock/"]) {
            [_cache removeObjectForKey:key];
        }
    }
    _floatingRow = -1;
    _dockSide = -1;
    return YES;
}

- (void)drawCloudForState:(const MacRenderState&)state inRect:(NSRect)rect {
    const CGFloat bubbleWidth = 270;
    const CGFloat bubbleHeight = 110;
    CGFloat x = (rect.size.width - bubbleWidth) / 2;
    if (state.docked) x += state.dock_edge == DockEdge::Left ? -48 : 48;
    const CGFloat y = state.bubble_below ? rect.size.height - bubbleHeight - 45 : 45;
    const NSRect cloud = NSMakeRect(x, y, bubbleWidth, bubbleHeight);
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:cloud xRadius:18 yRadius:18];
    [[NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:0.94] setFill];
    [[NSColor colorWithSRGBRed:39/255.0 green:50/255.0 blue:60/255.0 alpha:0.88] setStroke];
    path.lineWidth = 1.2;
    [path fill]; [path stroke];

    const CGFloat anchorX = state.docked
        ? (state.dock_edge == DockEdge::Left ? NSMinX(cloud) + 18 : NSMaxX(cloud) - 18)
        : NSMidX(cloud);
    const CGFloat anchorY = state.bubble_below ? NSMinY(cloud) : NSMaxY(cloud);
    const CGFloat petX = state.docked
        ? (state.dock_edge == DockEdge::Left ? 18 : rect.size.width - 18)
        : (state.mirror ? rect.size.width / 2 + 36 : rect.size.width / 2 - 36);
    const CGFloat petY = state.docked ? (state.bubble_below ? 12 : rect.size.height - 12)
                                      : rect.size.height - 32;
    const CGFloat dx = petX - anchorX, dy = petY - anchorY;
    const CGFloat distance = std::max<CGFloat>(1, std::sqrt(dx * dx + dy * dy));
    auto drawDot = [&](CGFloat fraction, CGFloat size) {
        const CGFloat cx = anchorX + dx / distance * std::min(distance * fraction, 46.0);
        const CGFloat cy = anchorY + dy / distance * std::min(distance * fraction, 46.0);
        NSBezierPath* dot = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - size / 2, cy - size / 2, size, size)];
        [[NSColor colorWithSRGBRed:241/255.0 green:248/255.0 blue:1 alpha:1] setFill];
        [[NSColor colorWithSRGBRed:42/255.0 green:50/255.0 blue:60/255.0 alpha:1] setStroke];
        [dot fill]; [dot stroke];
    };
    drawDot(0.33, 17); drawDot(0.66, 10);

    const CGFloat ox = NSMinX(cloud) + bubbleWidth * 0.10;
    const CGFloat oy = NSMinY(cloud) + bubbleHeight * 0.32;
    NSColor* glow = state.state == ReminderState::Error
        ? [NSColor colorWithSRGBRed:226/255.0 green:62/255.0 blue:55/255.0 alpha:1]
        : [NSColor colorWithSRGBRed:83/255.0 green:169/255.0 blue:236/255.0 alpha:1];
    auto cell = [&](NSColor* color, int cx, int cy, int width, int height) {
        [color setFill];
        NSRectFill(NSMakeRect(ox + cx * 2.5, oy + cy * 2.5, width * 2.5, height * 2.5));
    };
    NSColor* outline = [NSColor colorWithSRGBRed:68/255.0 green:43/255.0 blue:25/255.0 alpha:1];
    cell(outline, 6, 0, 1, 2); cell(outline, 2, 2, 1, 1); cell(outline, 10, 2, 1, 1);
    cell(outline, 0, 6, 2, 1); cell(outline, 11, 6, 2, 1);
    const int silhouette[][3] = {{3,4,5},{4,3,7},{5,2,9},{6,2,9},{7,2,9},{8,3,7},
                                  {9,4,5},{10,5,3},{11,4,5},{12,4,5},{13,5,3}};
    for (const auto& row : silhouette) cell(outline, row[1], row[0], row[2], 1);
    cell(glow, 4, 4, 5, 1); cell(glow, 3, 5, 7, 3); cell(glow, 4, 8, 5, 1);
    cell(glow, 5, 9, 3, 1); cell(outline, 5, 7, 1, 2); cell(outline, 7, 7, 1, 2);
    cell([NSColor colorWithSRGBRed:91/255.0 green:78/255.0 blue:70/255.0 alpha:1], 5, 11, 3, 2);

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
    NSMutableParagraphStyle* centered = [[NSMutableParagraphStyle alloc] init];
    centered.alignment = NSTextAlignmentCenter;
    NSDictionary* headerAttributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightBold],
        NSForegroundColorAttributeName: HeaderColor(state.state),
        NSParagraphStyleAttributeName: centered
    };
    [Ns(header) drawInRect:NSMakeRect(NSMinX(cloud) + bubbleWidth * .26,
                                      NSMinY(cloud) + bubbleHeight * .09,
                                      bubbleWidth * .52, bubbleHeight * .22)
             withAttributes:headerAttributes];
    NSMutableParagraphStyle* bodyStyle = [[NSMutableParagraphStyle alloc] init];
    bodyStyle.lineBreakMode = NSLineBreakByWordWrapping;
    bodyStyle.minimumLineHeight = 15; bodyStyle.maximumLineHeight = 15;
    NSDictionary* bodyAttributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:11.5],
        NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:45/255.0 green:60/255.0 blue:78/255.0 alpha:1],
        NSParagraphStyleAttributeName: bodyStyle
    };
    const NSRect viewport = NSMakeRect(NSMinX(cloud) + bubbleWidth * .30,
                                       NSMinY(cloud) + bubbleHeight * .34, 156, 45);
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:viewport] addClip];
    [Ns(body) drawInRect:NSMakeRect(NSMinX(viewport), NSMinY(viewport) - state.scroll_offset,
                                    NSWidth(viewport), 1000)
             withAttributes:bodyAttributes];
    [NSGraphicsContext restoreGraphicsState];
}

- (void)drawPetForState:(const MacRenderState&)state inRect:(NSRect)rect {
    if (state.docked) {
        const int frame = app_logic::select_dock_sprite_index(state.dock_edge, state.state, state.animation_tick);
        NSImage* image = [self dockImage:frame];
        if (!image) return;
        const CGFloat hidden = 104 * (1 - SmoothStep(state.dock_visibility));
        const CGFloat x = state.dock_edge == DockEdge::Left ? -hidden : rect.size.width - 104 + hidden;
        const CGFloat y = state.bubble_below ? 4 : rect.size.height - 104 - 7;
        [image drawInRect:NSMakeRect(x, y, 104, 104)
                 fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1
           respectFlipped:YES hints:nil];
        return;
    }
    const int frame = app_logic::select_floating_frame(state.state, state.animation_tick);
    NSImage* image = [self floatingImageForState:state.state frame:frame];
    if (!image) return;
    const NSRect destination = NSMakeRect((rect.size.width - 130) / 2, rect.size.height - 143, 130, 140);
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
    constexpr size_t width = 420;
    constexpr size_t height = 260;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) {
        if (error) *error = @"无法创建预览色彩空间";
        return NO;
    }
    CGContextRef bitmapContext = CGBitmapContextCreate(nullptr, width, height, 8, width * 4,
        colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colorSpace);
    if (!bitmapContext) {
        if (error) *error = @"无法创建预览位图";
        return NO;
    }
    NSGraphicsContext* graphics = [NSGraphicsContext graphicsContextWithCGContext:bitmapContext
                                                                          flipped:YES];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = graphics;
    [self drawState:state inRect:NSMakeRect(0, 0, width, height)];
    [graphics flushGraphics];
    [NSGraphicsContext restoreGraphicsState];
    CGImageRef image = CGBitmapContextCreateImage(bitmapContext);
    CGContextRelease(bitmapContext);
    if (!image) {
        if (error) *error = @"无法生成预览图像";
        return NO;
    }
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    CGImageRelease(image);
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png || ![png writeToFile:path atomically:YES]) {
        if (error) *error = @"保存预览失败";
        return NO;
    }
    return YES;
}

@end

@class AppDelegate;

@interface PetView : NSView
@property(nonatomic, weak) AppDelegate* owner;
@end

struct PendingMacUpdate {
    std::uint64_t generation{};
    std::vector<MonitorEventKind> events;
    MonitorSnapshot snapshot;
};

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate> {
@private
    NSStatusItem* _statusItem;
    NSPanel* _petWindow;
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
    std::unique_ptr<JsonSettingsStore> _settingsStore;
    AppSettings _settings;
    VisualStateCoordinator _visualCoordinator;
    MonitorSnapshot _snapshot;
    std::unique_ptr<MonitorWorker> _monitorWorker;
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
    NSInteger _lastMouseReception;
    std::uint64_t _monitorGeneration;
    bool _hasSnapshot;
    bool _terminating;
}
- (void)loadOrMigrateSettings;
- (void)saveSettings;
- (void)playSoundNamed:(NSString*)name;
- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action;
- (void)updateMenu:(NSMenu*)menu;
- (void)togglePet:(id)sender;
- (void)toggleSound:(id)sender;
- (BOOL)autoStartEnabled;
- (void)toggleStartup:(id)sender;
- (void)openSessionsFolder:(id)sender;
- (void)openUpdate:(id)sender;
- (void)quit:(id)sender;
- (void)createPetWindow;
- (void)createStatusItem;
- (void)placeDefault;
- (void)restorePosition;
- (void)savePosition;
- (void)clampToWorkArea;
- (void)updateDockWindowPosition;
- (void)trySnapOrClamp:(NSPoint)cursor;
- (void)updateMousePassThrough;
- (BOOL)dockHovering:(NSPoint)cursor;
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
- (void)startMonitor;
- (void)applyMonitorUpdate:(const PendingMacUpdate&)update;
- (void)refreshVisual:(BOOL)force;
- (void)updateRenderGeometry;
- (void)onTimer:(NSTimer*)timer;
@end

@implementation PetView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    [self.owner drawPetView:self.bounds];
}
- (NSView*)hitTest:(NSPoint)point { return [self.owner isInteractivePoint:point] ? self : nil; }
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
    [self createStatusItem];
    [self placeDefault];
    [self restorePosition];
    [self startMonitor];
    if (_settings.pet_visible) [_petWindow orderFrontRegardless];
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self
                                            selector:@selector(onTimer:) userInfo:nil repeats:YES];
    [self refreshVisual:YES];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    _terminating = true;
    [_timer invalidate];
    if (_monitorWorker) _monitorWorker->stop();
    [self savePosition];
    return NSTerminateNow;
}

- (void)loadOrMigrateSettings {
    if (std::filesystem::exists(_settingsStore->settings_file_path())) {
        _settings = _settingsStore->load();
        return;
    }
    AppSettings migrated;
    NSUserDefaults* defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.mrliugangqiang.codexpets"];
    auto integer = [&](NSString* key, int fallback) {
        return [defaults objectForKey:key] ? static_cast<int>([defaults integerForKey:key]) : fallback;
    };
    migrated.dock_hover_height = integer(@"DockHoverHeight", migrated.dock_hover_height);
    migrated.dock_idle_hide_seconds = integer(@"DockIdleHideSeconds", migrated.dock_idle_hide_seconds);
    migrated.dock_reveal_seconds = integer(@"DockRevealSeconds", migrated.dock_reveal_seconds);
    migrated.dock_notification_seconds = integer(@"DockNotificationSeconds", migrated.dock_notification_seconds);
    if ([defaults objectForKey:@"SoundEnabled"]) migrated.sound_enabled = [defaults boolForKey:@"SoundEnabled"];
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
    std::string ignored; _settingsStore->save(_settings, &ignored);
}

- (void)createPetWindow {
    _petWindow = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 260)
                                            styleMask:NSWindowStyleMaskBorderless
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
    _petWindow.ignoresMouseEvents = YES;
    _petView = [[PetView alloc] initWithFrame:NSMakeRect(0, 0, 420, 260)];
    _petView.owner = self;
    _petWindow.contentView = _petView;
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
    NSMenuItem* status = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"状态：%@", Ns(_renderState.status_text)]
                                                      action:nil keyEquivalent:@""];
    status.enabled = NO; status.tag = 100;
    [menu addItem:status];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* pet = [self menuItem:@"显示桌面宠物" action:@selector(togglePet:)]; pet.tag = 101;
    NSMenuItem* sound = [self menuItem:@"播放语音提醒" action:@selector(toggleSound:)]; sound.tag = 102;
    NSMenuItem* startup = [self menuItem:@"登录时自动运行" action:@selector(toggleStartup:)]; startup.tag = 103;
    [menu addItem:pet]; [menu addItem:sound]; [menu addItem:startup];
    [menu addItem:[self menuItem:@"打开 Codex 会话目录" action:@selector(openSessionsFolder:)]];
    [menu addItem:[self menuItem:@"设置…" action:@selector(showSettings:)]];
    [menu addItem:[self menuItem:@"查看更新…" action:@selector(openUpdate:)]];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:[self menuItem:@"退出" action:@selector(quit:)]];
    [self updateMenu:menu];
    return menu;
}

- (void)menuNeedsUpdate:(NSMenu*)menu { [self updateMenu:menu]; }

- (void)updateMenu:(NSMenu*)menu {
    NSMenuItem* status = [menu itemWithTag:100];
    status.title = [NSString stringWithFormat:@"状态：%@", Ns(_renderState.status_text)];
    [menu itemWithTag:101].state = _settings.pet_visible ? NSControlStateValueOn : NSControlStateValueOff;
    [menu itemWithTag:102].state = _settings.sound_enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [menu itemWithTag:103].state = [self autoStartEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)togglePet:(id)sender {
    (void)sender;
    _settings.pet_visible = !_settings.pet_visible;
    if (_settings.pet_visible) [_petWindow orderFrontRegardless]; else [_petWindow orderOut:nil];
    [self saveSettings];
}

- (void)toggleSound:(id)sender { (void)sender; _settings.sound_enabled = !_settings.sound_enabled; [self saveSettings]; }

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

- (void)playSoundNamed:(NSString*)name {
    if (!_settings.sound_enabled) return;
    NSString* path = ResourcePath(@"audio", name, @"mp3");
    if (!path) return;
    _currentSound = [[NSSound alloc] initWithContentsOfFile:path byReference:YES];
    [_currentSound play];
}

- (void)startMonitor {
    __weak AppDelegate* weakSelf = self;
    const auto generation = ++_monitorGeneration;
    _monitorWorker = std::make_unique<MonitorWorker>(_settings.sessions_root,
        [weakSelf, generation](std::vector<MonitorEventKind> events, MonitorSnapshot snapshot) {
            auto update = std::make_shared<PendingMacUpdate>(
                PendingMacUpdate{generation, std::move(events), std::move(snapshot)});
            dispatch_async(dispatch_get_main_queue(), ^{
                AppDelegate* strongSelf = weakSelf;
                if (strongSelf) [strongSelf applyMonitorUpdate:*update];
            });
        });
    _monitorWorker->start();
}

- (void)applyMonitorUpdate:(const PendingMacUpdate&)update {
    if (update.generation != _monitorGeneration) return;
    const BOOL first = !_hasSnapshot;
    _snapshot = update.snapshot;
    _hasSnapshot = true;
    const auto now = Clock::now();
    for (const auto event : update.events) {
        const auto preferredTaskIndex = event == MonitorEventKind::PlanUpdated
            ? _snapshot.latest_plan_update_active_title_index
            : event == MonitorEventKind::TaskStarted
                ? _snapshot.latest_event_active_title_index
                : -1;
        if (event == MonitorEventKind::TaskStarted) {
            _visualCoordinator.record_started(preferredTaskIndex);
            if (_settings.pet_visible) [_petWindow orderFrontRegardless];
            [self playSoundNamed:@"voice-start"];
        } else if (event == MonitorEventKind::TaskCompleted) {
            _visualCoordinator.record_completed(now, std::chrono::seconds(
                app_logic::cloud_notification_seconds(ReminderState::Completed,
                                                       _settings.dock_notification_seconds)));
            if (_settings.pet_visible) [_petWindow orderFrontRegardless];
            [self playSoundNamed:@"voice-complete"];
        } else if (event == MonitorEventKind::TaskAborted) {
            _visualCoordinator.record_aborted(now, std::chrono::seconds(
                app_logic::cloud_notification_seconds(ReminderState::Error,
                                                       _settings.dock_notification_seconds)));
            if (_settings.pet_visible) [_petWindow orderFrontRegardless];
            [self playSoundNamed:@"voice-error"];
        } else if (event == MonitorEventKind::PlanUpdated) {
            _visualCoordinator.record_started(preferredTaskIndex);
        }
    }
    if (first || !update.events.empty()) [self refreshVisual:YES];
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
        _animationTick = 0; _animationAccumulator = 0;
        _scrollOffset = 0; _scrollHold = 1.9; _scrollAtEnd = false; _rotationSeconds = 0;
    }
    _renderState.state = state;
    _renderState.docked = _dockEdge != DockEdge::None;
    _renderState.dock_edge = _dockEdge;
    _renderState.dock_visibility = _dockVisibility;
    _renderState.animation_tick = _animationTick;
    _renderState.selected_task_index = _selectedTaskIndex;
    _renderState.scroll_offset = _scrollOffset;
    if (state == ReminderState::Error) {
        _renderState.status_text = "异常";
    } else if (state == ReminderState::Completed) {
        _renderState.status_text = "已完成";
    } else if (state == ReminderState::Busy) {
        std::string busyProgress;
        std::optional<std::string_view> progress;
        if (_snapshot.total_plan_step_count > 0) {
            busyProgress = std::to_string(_snapshot.completed_plan_step_count) + "/" +
                           std::to_string(_snapshot.total_plan_step_count);
            progress = busyProgress;
        }
        _renderState.status_text = app_logic::format_busy_header(
            progress, _snapshot.latest_event_active_title_index, _snapshot.active_count);
    } else {
        _renderState.status_text = "空闲";
    }
    _renderState.thought_text = state == ReminderState::Error ? "任务出现异常了。" :
        state == ReminderState::Completed ? "任务完成啦！" :
        state == ReminderState::Busy ? (_snapshot.active_titles.empty() ? "正在认真处理你的任务…"
                                                                        : _snapshot.active_titles.front()) :
        "主人，现在没有在进行中的任务!别让我歇着!";
    if (state == ReminderState::Error) {
        _renderState.task_titles = {app_logic::format_abnormal_task_text(_snapshot.last_aborted_title)};
        _renderState.progress_labels.clear();
    } else if (state == ReminderState::Completed && !_snapshot.last_completed_title.empty()) {
        _renderState.task_titles = {_snapshot.last_completed_title};
        _renderState.progress_labels.clear();
    } else {
        _renderState.task_titles = _snapshot.active_titles;
        _renderState.progress_labels = _snapshot.active_plan_progress_labels;
    }
    const bool selectNewest = _visualCoordinator.show_newest_task_on_next_refresh() && state == ReminderState::Busy;
    const int preferred = app_logic::select_preferred_task_index(
        selectNewest, _visualCoordinator.preferred_task_index());
    _selectedTaskIndex = app_logic::reconcile_task_selection(
        state, _renderState.task_titles, _selectedTaskIndex, selectedTitle, selectNewest, preferred);
    _renderState.selected_task_index = _selectedTaskIndex;
    if (selectNewest) _visualCoordinator.consume_newest_task_focus();
    [self updateRenderGeometry];
    [_petView setNeedsDisplay:YES];
    _statusItem.button.image = [_renderer statusImageForState:state frame:_animationTick / 2];
    _statusItem.button.image.size = NSMakeSize(18, 18);
    _statusItem.button.toolTip = [NSString stringWithFormat:@"CodeXPets · %@", Ns(_renderState.status_text)];
}

- (void)updateRenderGeometry {
    NSScreen* screen = _petWindow.screen ? _petWindow.screen : NSScreen.mainScreen;
    NSRect work = screen.visibleFrame;
    _renderState.docked = _dockEdge != DockEdge::None;
    _renderState.bubble_visible = app_logic::should_show_thought_bubble(
        _renderState.docked, _renderState.state, Clock::now(), _dockThoughtUntil);
    _renderState.dock_edge = _dockEdge;
    _renderState.dock_visibility = _dockVisibility;
    _renderState.animation_tick = _animationTick;
    _renderState.selected_task_index = _selectedTaskIndex;
    _renderState.scroll_offset = _scrollOffset;
    _renderState.bubble_below = _renderState.docked && _dockCoordinate > NSMaxY(work) - 150;
    _renderState.mirror = !_renderState.docked && NSMidX(_petWindow.frame) < NSMidX(work);
}

- (void)drawPetView:(NSRect)rect { [_renderer drawState:_renderState inRect:rect]; }

- (BOOL)isInteractivePoint:(NSPoint)point {
    if (_dragPending || _dragging) return YES;
    if (_renderState.docked) {
        const CGFloat hidden = 104 * (1 - SmoothStep(_dockVisibility));
        NSRect pet = _dockEdge == DockEdge::Left
            ? NSMakeRect(-hidden, _renderState.bubble_below ? 4 : 149, 104, 104)
            : NSMakeRect(420 - 104 + hidden, _renderState.bubble_below ? 4 : 149, 104, 104);
        if (NSPointInRect(point, pet)) return YES;
    } else if (NSPointInRect(point, NSMakeRect(145, 117, 130, 140))) return YES;
    if (_renderState.bubble_visible) {
        CGFloat x = 75 + (_renderState.docked ? (_dockEdge == DockEdge::Left ? -48 : 48) : 0);
        const CGFloat y = _renderState.bubble_below ? 105 : 45;
        if (NSPointInRect(point, NSMakeRect(x, y, 270, 110))) return YES;
    }
    return NO;
}

- (void)onTimer:(NSTimer*)timer {
    (void)timer;
    const auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - _lastTick).count();
    _lastTick = now;
    elapsed = std::clamp(elapsed, 0.001, 0.25);
    _animationAccumulator += elapsed;
    bool changed = false;
    while (_animationAccumulator >= 0.12) {
        _animationAccumulator -= 0.12;
        _animationTick = (_animationTick + 1) % 6400;
        changed = true;
    }
    if (_dockEdge != DockEdge::None) {
        const BOOL hovering = [self dockHovering:NSEvent.mouseLocation];
        if (hovering) {
            _dockRevealUntil = now + std::chrono::seconds(_settings.dock_reveal_seconds);
        }
        const BOOL show = app_logic::should_show_dock(_dockLastChange, now,
            _dragPending || _dragging, hovering, _dockRevealUntil, _settings.dock_idle_hide_seconds);
        const double target = show ? 1 : 0;
        if (std::abs(target - _dockVisibility) > .001) {
            _dockVisibility += (target > _dockVisibility ? 1 : -1) * elapsed / (target > _dockVisibility ? .30 : .55);
            _dockVisibility = std::clamp(_dockVisibility, 0.0, 1.0);
            changed = true;
        }
    }
    _renderState.bubble_visible = app_logic::should_show_thought_bubble(
        _dockEdge != DockEdge::None, _renderState.state, now, _dockThoughtUntil);
    const BOOL bubble = _renderState.bubble_visible;
    if (bubble && _renderState.task_titles.size() > 1) {
        _rotationSeconds += elapsed;
        if (_rotationSeconds >= 6) {
            _rotationSeconds -= 6;
            _selectedTaskIndex = (_selectedTaskIndex + 1) % static_cast<int>(_renderState.task_titles.size());
            _scrollOffset = 0; _scrollHold = 1.9; _scrollAtEnd = false; changed = true;
        }
    }
    if (bubble) {
        const std::string text = _renderState.task_titles.empty() ? _renderState.thought_text
            : _renderState.task_titles[std::clamp(_selectedTaskIndex, 0,
                                                   static_cast<int>(_renderState.task_titles.size()) - 1)];
        const int lines = std::max(1, static_cast<int>(text.size() / 24));
        const double maxScroll = std::max(0.0, (lines - 3) * 15.0);
        if (maxScroll > 0) {
            if (_scrollHold > 0) _scrollHold = std::max(0.0, _scrollHold - elapsed);
            else if (!_scrollAtEnd) {
                _scrollOffset = std::min(maxScroll, _scrollOffset + 15 * elapsed);
                if (_scrollOffset >= maxScroll) { _scrollAtEnd = true; _scrollHold = 1.7; }
                changed = true;
            } else { _scrollOffset = 0; _scrollHold = 1.9; _scrollAtEnd = false; changed = true; }
        }
    } else {
        _rotationSeconds = 0;
        if (_scrollOffset != 0) { _scrollOffset = 0; changed = true; }
    }
    const auto nextState = _visualCoordinator.select(_snapshot.active_count, now);
    if (nextState != _renderState.state) { [self refreshVisual:YES]; return; }
    [self updateRenderGeometry];
    [self updateMousePassThrough];
    if (changed) {
        [_petView setNeedsDisplay:YES];
        _statusItem.button.image = [_renderer statusImageForState:_renderState.state frame:_animationTick / 2];
        _statusItem.button.image.size = NSMakeSize(18, 18);
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
    const BOOL bubbleBelow = _dockCoordinate > NSMaxY(work) - 150;
    const CGFloat visibleCenterOffset = bubbleBelow ? 204.0 : 59.0;
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
        [self clampToWorkArea];
    } else {
        _dockEdge = edge;
        _dockScreenIdentifier = ScreenIdentifier(screen);
        _dockCoordinate = cursor.y;
        _dockVisibility = 1.0;
        _dockLastChange = Clock::now();
        _dockRevealUntil = Clock::time_point::min();
        [self updateDockWindowPosition];
    }
    [self refreshVisual:YES];
}

- (BOOL)dockHovering:(NSPoint)cursor {
    if (_dockEdge == DockEdge::None) return NO;
    NSScreen* screen = ScreenForIdentifier(_dockScreenIdentifier);
    if (!screen) return NO;
    const NSRect work = screen.visibleFrame;
    const CGFloat width = _dockVisibility <= 0.01 ? 28.0 : 56.0;
    const CGFloat halfHeight = std::max<CGFloat>(20.0, _settings.dock_hover_height / 2.0);
    const CGFloat x = _dockEdge == DockEdge::Left ? NSMinX(work) : NSMaxX(work) - width;
    const NSRect edgeTrigger = NSMakeRect(x,
        std::max(NSMinY(work), _dockCoordinate - halfHeight), width,
        std::max<CGFloat>(1.0,
            std::min(NSMaxY(work), _dockCoordinate + halfHeight) -
            std::max(NSMinY(work), _dockCoordinate - halfHeight)));
    if (NSPointInRect(cursor, edgeTrigger)) return YES;

    const BOOL bubbleBelow = _dockCoordinate > NSMaxY(work) - 150;
    const CGFloat hidden = 104 * (1 - SmoothStep(_dockVisibility));
    const CGFloat localX = _dockEdge == DockEdge::Left
        ? -hidden : kPetWindowWidth - 104 + hidden;
    const CGFloat localY = bubbleBelow ? 4 : 149;
    const NSRect frame = _petWindow.frame;
    const NSRect petGlobal = NSMakeRect(NSMinX(frame) + localX,
        NSMinY(frame) + kPetWindowHeight - localY - 104, 104, 104);
    return NSPointInRect(cursor, petGlobal);
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
    CGFloat cloudX = (kPetWindowWidth - 270) / 2;
    if (_dockEdge != DockEdge::None) cloudX += _dockEdge == DockEdge::Left ? -48 : 48;
    const CGFloat cloudY = _renderState.bubble_below ? 105 : 45;
    const RectD bubble{cloudX, cloudY, 270, 110};
    const RectD content{cloudX + 81, cloudY + 37.4, 156, 45};
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
}

- (void)showSettings:(id)sender {
    (void)sender;
    if (!_settingsWindow) {
        _settingsWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 440)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        _settingsWindow.title = @"CodeXPets 设置";
        _settingsWindow.releasedWhenClosed = NO;
        _settingsWindow.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace;
        NSView* content = _settingsWindow.contentView;

        NSTextField* heading = MakeLabel(@"桌面宠物与会话监听", NSMakeRect(22, 392, 380, 28));
        heading.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
        [content addSubview:heading];

        const CGFloat labelX = 22, fieldX = 270;
        [content addSubview:MakeLabel(@"边缘唤出区域高度（像素）", NSMakeRect(labelX, 346, 235, 24))];
        [content addSubview:MakeLabel(@"吸附隐藏（秒，0=关闭）", NSMakeRect(labelX, 306, 235, 24))];
        [content addSubview:MakeLabel(@"鼠标唤出后保持（秒）", NSMakeRect(labelX, 266, 235, 24))];
        [content addSubview:MakeLabel(@"任务状态云朵保持（秒）", NSMakeRect(labelX, 226, 235, 24))];
        [content addSubview:MakeLabel(@"Codex 会话目录", NSMakeRect(labelX, 183, 235, 24))];
        _settingsHoverField = MakeTextField(@"", NSMakeRect(fieldX, 342, 130, 28));
        _settingsIdleField = MakeTextField(@"", NSMakeRect(fieldX, 302, 130, 28));
        _settingsRevealField = MakeTextField(@"", NSMakeRect(fieldX, 262, 130, 28));
        _settingsNotificationField = MakeTextField(@"", NSMakeRect(fieldX, 222, 130, 28));
        _settingsRootField = MakeTextField(@"", NSMakeRect(fieldX, 179, 242, 28));
        for (NSTextField* field in @[_settingsHoverField, _settingsIdleField,
                                    _settingsRevealField, _settingsNotificationField,
                                    _settingsRootField]) [content addSubview:field];
        [content addSubview:MakeButton(@"浏览…", self, @selector(browseSessionsRoot:),
                                       NSMakeRect(520, 178, 78, 30))];
        _settingsSoundButton = [NSButton checkboxWithTitle:@"播放开始、完成和异常语音提醒"
                                                    target:nil action:nil];
        _settingsSoundButton.frame = NSMakeRect(22, 137, 390, 28);
        [content addSubview:_settingsSoundButton];

        NSTextField* hint = MakeLabel(
            @"CodeXPets 仅增量读取该目录中的 JSONL 会话文件；使用轻量轮询，"
             @"不会启动额外服务，也不会修改 Codex 文件。",
            NSMakeRect(22, 75, 576, 48));
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
    [_settingsWindow makeKeyAndOrderFront:nil];
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
    next.normalize();
    const bool rootChanged = next.sessions_root != _settings.sessions_root;
    _settings = std::move(next);
    [self saveSettings];
    if (rootChanged) {
        if (_monitorWorker) _monitorWorker->stop();
        _monitorWorker.reset();
        _snapshot = MonitorSnapshot{};
        _hasSnapshot = false;
        _visualCoordinator = VisualStateCoordinator{};
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
        state.task_titles = {"原生渲染检查"};
        state.progress_labels = {std::optional<std::string>("1/3")};
        const std::array<std::pair<ReminderState, const char*>, 5> states{{
            {ReminderState::Idle, "idle"}, {ReminderState::Busy, "busy"},
            {ReminderState::Completed, "completed"}, {ReminderState::Error, "error"},
            {ReminderState::Interrupted, "interrupted"}}};
        NSString* renderError = nil;
        for (const auto& [visual, name] : states) {
            state.state = visual;
            state.status_text = name;
            state.animation_tick = visual == ReminderState::Busy ? 18 : 0;
            const auto path = outputDirectory / (std::string(name) + ".png");
            if (![renderer savePreview:state toPath:Ns(path_to_utf8(path)) error:&renderError]) {
                std::cerr << Utf8(renderError ? renderError : @"render failed") << '\n';
                if (temporary) std::filesystem::remove_all(outputDirectory, ioError);
                return 1;
            }
        }
        state.docked = true;
        for (const auto& [visual, name] : states) {
            state.state = visual;
            state.animation_tick = visual == ReminderState::Idle ? 0 : 18;
            for (const auto edge : {DockEdge::Left, DockEdge::Right}) {
                state.dock_edge = edge;
                const auto side = edge == DockEdge::Left ? "left" : "right";
                state.status_text = std::string("dock-") + name + "-" + side;
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
        for (NSString* name in @[@"voice-start", @"voice-complete", @"voice-error"]) {
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
        for (int index = 1; index < argc; ++index) {
            const std::string argument = ArgumentAt(argc, argv, index);
            if (argument == "--version" || argument == "--preview" ||
                argument == "--validate-resources" || argument == "--smoke-test" ||
                argument == "--test-sound") {
                utility = true;
                break;
            }
        }
        if (utility) return RunMacUtility(argc, argv);
        [NSApplication sharedApplication];
        if (AnotherInstanceIsRunning()) {
            std::cerr << "CodeXPets 已经在菜单栏里运行。\n";
            return 0;
        }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        NSApp.delegate = delegate;
        [NSApp run];
        (void)delegate;
    }
    return 0;
}
