#include "folder_picker.hpp"

#ifdef _WIN32

#include "../utils/logger.hpp"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <string>

namespace acecode::desktop {

namespace {

// UTF-16 → UTF-8。基础实现,夹带不可解码字节直接落空(MVP 容忍,实际选目录路径都是
// 系统合法 UTF-16)。
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

std::optional<std::string> pick_folder(void* parent_hwnd) {
    // CoInitializeEx: STA 模式;若已 init 过(webview 内部已初始化 COM)
    // 返回 RPC_E_CHANGED_MODE,这种情况直接当 OK 处理 — IFileOpenDialog 在 STA 用得正常。
    HRESULT hr_init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_init_com = SUCCEEDED(hr_init);
    if (!we_init_com && hr_init != RPC_E_CHANGED_MODE && hr_init != S_FALSE) {
        LOG_WARN("[folder_picker] CoInitializeEx failed");
        return std::nullopt;
    }

    std::optional<std::string> result;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        // FOS_PICKFOLDERS: 选目录而不是文件;FOS_FORCEFILESYSTEM: 强制返回真实磁盘路径,
        // 排除虚拟 namespace(虚拟回收站之类)。
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dlg->SetTitle(L"Select project folder");

        HWND parent = reinterpret_cast<HWND>(parent_hwnd);
        hr = dlg->Show(parent);
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = wide_to_utf8(std::wstring(path));
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        // hr 可能是 HRESULT_FROM_WIN32(ERROR_CANCELLED) — 用户取消,不报错
        dlg->Release();
    } else {
        LOG_WARN("[folder_picker] CoCreateInstance(IFileOpenDialog) failed");
    }

    if (we_init_com) ::CoUninitialize();
    return result;
}

} // namespace acecode::desktop

#else // POSIX stub

#include <optional>
#include <string>

namespace acecode::desktop {

std::optional<std::string> pick_folder(void* /*parent*/) {
    // POSIX 版本未实现 — 后续 PR 接 GTK FileChooser / NSOpenPanel。
    return std::nullopt;
}

} // namespace acecode::desktop

#endif
