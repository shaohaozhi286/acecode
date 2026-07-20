#include "desktop_about.hpp"

#include "../utils/encoding.hpp"

#include <iomanip>
#include <sstream>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commctrl.h>
#  ifdef _MSC_VER
#    pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#  endif
#endif

namespace acecode::desktop {

namespace {

std::string value_or_unknown(const std::string& value) {
    return value.empty() ? std::string("未知") : value;
}

std::string display_acecode_version(const std::string& version) {
    if (version.empty()) return "未知";
    if (version.front() == 'v' || version.front() == 'V') return version;
    return "v" + version;
}

std::string browser_label(const DesktopAboutInfo& info) {
    const std::string name = value_or_unknown(info.browser_name);
    return info.browser_version.empty() ? name : name + " " + info.browser_version;
}

} // namespace

std::string format_msvc_compiler_version(int msc_version,
                                         long long msc_full_version) {
    if (msc_version <= 0) return "Microsoft Visual C++";
    std::ostringstream out;
    out << "Microsoft Visual C++ "
        << (msc_version / 100) << '.'
        << std::setw(2) << std::setfill('0') << (msc_version % 100);
    if (msc_full_version > 0) {
        out << '.' << (msc_full_version % 100000);
    }
    return out.str();
}

std::string current_compiler_version() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
#  if defined(_MSC_FULL_VER)
    return format_msvc_compiler_version(_MSC_VER, _MSC_FULL_VER);
#  else
    return format_msvc_compiler_version(_MSC_VER, 0);
#  endif
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "未知";
#endif
}

std::string format_desktop_about_content(const DesktopAboutInfo& info) {
    std::ostringstream out;
    out << "ACECode 版本：" << display_acecode_version(info.acecode_version) << '\n'
        << "浏览器版本：" << browser_label(info) << '\n'
        << "编译器版本：" << value_or_unknown(info.compiler_version);
    return out.str();
}

bool show_desktop_about_dialog(void* parent_window,
                               const DesktopAboutInfo& info) {
#ifdef _WIN32
    using TaskDialogFn = HRESULT(WINAPI*)(
        HWND,
        HINSTANCE,
        PCWSTR,
        PCWSTR,
        PCWSTR,
        TASKDIALOG_COMMON_BUTTON_FLAGS,
        PCWSTR,
        int*);

    const std::wstring title = L"关于 ACECode";
    const std::wstring main_instruction = L"ACECode";
    const std::wstring content = acecode::utf8_to_wide(
        format_desktop_about_content(info));
    int button = 0;
    HRESULT result = E_NOTIMPL;
    HMODULE common_controls = ::LoadLibraryW(L"comctl32.dll");
    if (common_controls) {
        auto task_dialog = reinterpret_cast<TaskDialogFn>(
            ::GetProcAddress(common_controls, "TaskDialog"));
        if (task_dialog) {
            result = task_dialog(
                static_cast<HWND>(parent_window),
                nullptr,
                title.c_str(),
                main_instruction.c_str(),
                content.c_str(),
                TDCBF_OK_BUTTON,
                TD_INFORMATION_ICON,
                &button);
        }
        ::FreeLibrary(common_controls);
    }
    if (SUCCEEDED(result)) return true;
    return ::MessageBoxW(
               static_cast<HWND>(parent_window),
               content.c_str(),
               title.c_str(),
               MB_OK | MB_ICONINFORMATION) != 0;
#else
    (void)parent_window;
    (void)info;
    return false;
#endif
}

} // namespace acecode::desktop
