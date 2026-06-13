#ifdef __APPLE__

// macOS 目录选择器 — 用 NSOpenPanel (Cocoa) 弹出原生文件夹选择对话框。
// 必须编译为 Objective-C++ (.mm),仅 Apple 平台编译。
// CMakeLists.txt 通过显式列举把此文件加入 acecode_testable。

#include "folder_picker.hpp"

#include <optional>
#include <string>

#import <AppKit/AppKit.h>

namespace acecode::desktop {

std::optional<std::string> pick_folder(void* /* parent_window */) {
    // NSOpenPanel 必须在主线程上运行。
    // - desktop bridge 回调: 由 webview/webview 的 bind 机制 dispatch_async 到
    //   主线程,所以直接调用 runModal 启动嵌套 run loop,安全。
    // - daemon HTTP handler(--native-folder-picker 路径): 运行在 Crow 线程池,
    //   必须 dispatch_sync 到主线程。
    //
    // 注意: 不使用 beginSheetModalForWindow + dispatch_semaphore_wait —— 该
    // 组合在主线程上会死锁:semaphore wait 阻塞主线程,completion handler 也需
    // 要主线程来执行,导致永久等待。

    __block std::optional<std::string> result;

    dispatch_block_t work = ^{
        // daemon 是无 bundle 的后台进程,默认 activation policy 是 Prohibited:
        // 窗口能创建但进程永远成不了 active app,NSOpenPanel 必然垫在浏览器
        // 窗口下面(webapp 兼容模式的实际报障)。修法是业界标准组合:
        //   1. 升级 policy 到 Accessory(不出现在 Dock,但允许拿前台)
        //   2. activate 当前进程
        //   3. 抬 panel 的 window level 并 orderFrontRegardless
        NSApplication* app = [NSApplication sharedApplication];
        if (app.activationPolicy == NSApplicationActivationPolicyProhibited) {
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // macOS 14 起弃用但仍生效;后台进程没有"前台让渡"语境,新的 cooperative
        // [NSApp activate] 对这种场景反而不保证生效。
        [app activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop

        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories    = YES;
        panel.canChooseFiles          = NO;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories    = YES;
        panel.title  = @"选择项目文件夹";
        panel.prompt = @"选择";
        panel.level  = NSModalPanelWindowLevel;
        [panel orderFrontRegardless];

        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = panel.URL;
            if (url) {
                const char* path = url.fileSystemRepresentation;
                if (path) result = std::string(path);
            }
        }
    };

    if ([NSThread isMainThread]) {
        work();
    } else {
        dispatch_sync(dispatch_get_main_queue(), work);
    }

    return result;
}

FolderPickOutcome pick_folder_outcome(void* parent_window) {
    // macOS:NSOpenPanel 是系统组件,不存在"选择器工具缺失"形态;
    // 取消与选中都由 pick_folder 表达,error 恒空。
    FolderPickOutcome outcome;
    outcome.path = pick_folder(parent_window);
    return outcome;
}

} // namespace acecode::desktop

#endif // __APPLE__
