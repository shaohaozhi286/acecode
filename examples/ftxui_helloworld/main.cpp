#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32

bool has_non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    return value && *value;
}

std::wstring file_name_from_path(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

bool wide_equals_ignore_case(const std::wstring& lhs, const wchar_t* rhs) {
    const int result = CompareStringOrdinal(
        lhs.c_str(),
        static_cast<int>(lhs.size()),
        rhs,
        -1,
        TRUE);
    return result == CSTR_EQUAL;
}

std::wstring process_image_path(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {};

    std::wstring path(4096, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        CloseHandle(process);
        return {};
    }

    CloseHandle(process);
    path.resize(size);
    return path;
}

bool console_window_looks_like_conhost(HWND hwnd) {
    wchar_t class_name[256] = {};
    constexpr int class_name_count = static_cast<int>(sizeof(class_name) / sizeof(class_name[0]));
    if (GetClassNameW(hwnd, class_name, class_name_count) <= 0) {
        return false;
    }
    return wide_equals_ignore_case(class_name, L"ConsoleWindowClass");
}

bool is_running_under_conhost() {
    if (has_non_empty_env("WT_SESSION") || has_non_empty_env("ConEmuPID")) {
        return false;
    }

    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return false;
    if (!IsWindowVisible(hwnd)) return false;

    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != 0) {
        const auto image_path = process_image_path(process_id);
        if (!image_path.empty()) {
            return wide_equals_ignore_case(file_name_from_path(image_path), L"conhost.exe");
        }
    }

    return console_window_looks_like_conhost(hwnd);
}

void show_conhost_status() {
    const bool is_conhost = is_running_under_conhost();

    std::cout << "CONHOST: " << (is_conhost ? "YES" : "NO") << "\n";

    if (is_conhost) {
        std::cout
            << "Warning: this FTXUI demo appears to be running under Windows Console Host (conhost.exe).\n"
            << "On some Windows 10 machines, conhost can render this TUI poorly.\n"
            << "For the best result, run it from Windows Terminal.\n";
    }

    std::cout << "\nPress Enter to continue..." << std::flush;

    std::string ignored;
    std::getline(std::cin, ignored);
}

#else

void show_conhost_status() {
    std::cout
        << "CONHOST: NO\n"
        << "\nPress Enter to continue..." << std::flush;

    std::string ignored;
    std::getline(std::cin, ignored);
}

#endif

} // namespace

int main() {
    using namespace ftxui;

    show_conhost_status();

    auto screen = ScreenInteractive::TerminalOutput();

    auto renderer = Renderer([&] {
        auto top = window(text("TOP") | bold | center,
                          text("Hello World") | center)
                 | size(HEIGHT, EQUAL, 5);

        auto middle = window(text("MIDDLE") | bold | center,
                             vbox({
                                 filler(),
                                 text("FTXUI three-row layout test") | center,
                                 filler(),
                             }));

        auto bottom = window(text("BOTTOM") | bold | center,
                             text("Press q or Esc to exit") | center)
                    | size(HEIGHT, EQUAL, 5);

        return vbox({
                   top,
                   middle | flex,
                   bottom,
               })
             | border;
    });

    auto app = CatchEvent(renderer, [&](const Event& event) {
        if (event == Event::Escape || event == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(app);
    return 0;
}
