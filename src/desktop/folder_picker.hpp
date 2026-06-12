#pragma once

// 跨平台目录选择器,用于"+ 添加项目" 入口。Windows 走 IFileOpenDialog,POSIX
// 在 MVP 阶段返回 nullopt(后续 PR 接 GTK / Cocoa)。

#include <optional>
#include <string>

namespace acecode::desktop {

// parent_hwnd: Windows 上传 HWND 当 dialog 的 owner 让 modal 关系正确;
// nullptr 也合法,Windows 下会尝试使用当前前台窗口作为 owner。
// 返回:用户选定的绝对路径(正斜杠 normalize 由调用方做);取消 / 失败 → nullopt。
std::optional<std::string> pick_folder(void* parent_hwnd);

// 可区分结果的版本:"用户取消"与"环境缺失选择器工具"是两种必须区别对待的
// 失败 —— 前者静默合理,后者必须把原因透传到 UI,否则用户点"添加项目"看到的
// 就是毫无反应(Linux 上 zenity/kdialog 双缺失时的真实事故)。
struct FolderPickOutcome {
    std::optional<std::string> path;  // 有值 = 用户选中
    std::string error;                // 非空 = 环境问题(如缺 zenity/kdialog)
    // path 为空且 error 为空 = 用户取消
};
FolderPickOutcome pick_folder_outcome(void* parent_hwnd);

} // namespace acecode::desktop
