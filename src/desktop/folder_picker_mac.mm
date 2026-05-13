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
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories    = YES;
        panel.canChooseFiles          = NO;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories    = YES;
        panel.title  = @"选择项目文件夹";
        panel.prompt = @"选择";

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

} // namespace acecode::desktop

#endif // __APPLE__
