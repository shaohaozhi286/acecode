#pragma once

// 跨平台目录选择器,用于"+ 添加项目" 入口。Windows 走 IFileOpenDialog,POSIX
// 在 MVP 阶段返回 nullopt(后续 PR 接 GTK / Cocoa)。

#include <optional>
#include <string>

namespace acecode::desktop {

// parent_hwnd: Windows 上传 HWND 当 dialog 的 owner 让 modal 关系正确;
// nullptr 也合法但 dialog 会浮在所有窗口之上无关联。
// 返回:用户选定的绝对路径(正斜杠 normalize 由调用方做);取消 / 失败 → nullopt。
std::optional<std::string> pick_folder(void* parent_hwnd);

} // namespace acecode::desktop
