#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;

struct TerminalSize {
    int width = 80;
    int height = 24;
};

#ifdef _WIN32

bool has_non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    return value && *value;
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

bool is_console_window_class(HWND hwnd) {
    wchar_t class_name[256] = {};
    constexpr int class_name_count = static_cast<int>(sizeof(class_name) / sizeof(class_name[0]));
    if (GetClassNameW(hwnd, class_name, class_name_count) <= 0) {
        return false;
    }
    return wide_equals_ignore_case(class_name, L"ConsoleWindowClass");
}

bool console_handle_supports_vt(DWORD std_handle) {
    HANDLE handle = GetStdHandle(std_handle);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) return false;

    const DWORD vt_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(handle, vt_mode)) {
        SetConsoleMode(handle, mode);
        return true;
    }

    return false;
}

bool is_running_under_conhost() {
    if (has_non_empty_env("WT_SESSION")) {
        return false;
    }

    HWND hwnd = GetConsoleWindow();
    if (hwnd && is_console_window_class(hwnd) && IsWindowVisible(hwnd)) {
        return true;
    }

    if (console_handle_supports_vt(STD_OUTPUT_HANDLE) ||
        console_handle_supports_vt(STD_ERROR_HANDLE)) {
        return false;
    }

    return hwnd != nullptr;
}

class StartupTerminalGuard {
public:
    StartupTerminalGuard() {
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output_ || output_ == INVALID_HANDLE_VALUE) {
            return;
        }
        if (!GetConsoleMode(output_, &original_mode_)) {
            return;
        }
        mode_saved_ = true;
        const DWORD vt_mode = original_mode_
                            | ENABLE_PROCESSED_OUTPUT
                            | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(output_, vt_mode)) {
            vt_enabled_ = true;
        }
    }

    ~StartupTerminalGuard() {
        if (mode_saved_) {
            SetConsoleMode(output_, original_mode_);
        }
    }

    bool can_animate() const {
        return vt_enabled_;
    }

private:
    HANDLE output_ = INVALID_HANDLE_VALUE;
    DWORD original_mode_ = 0;
    bool mode_saved_ = false;
    bool vt_enabled_ = false;
};

TerminalSize current_terminal_size() {
    TerminalSize size;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        return size;
    }
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(output, &csbi)) {
        return size;
    }
    size.width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    size.height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (size.width <= 0) size.width = 80;
    if (size.height <= 0) size.height = 24;
    return size;
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

class StartupTerminalGuard {
public:
    bool can_animate() const {
        return isatty(STDOUT_FILENO) != 0;
    }
};

TerminalSize current_terminal_size() {
    TerminalSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) size.width = ws.ws_col;
        if (ws.ws_row > 0) size.height = ws.ws_row;
    }
    return size;
}

void show_conhost_status() {
    std::cout
        << "CONHOST: NO\n"
        << "\nPress Enter to continue..." << std::flush;

    std::string ignored;
    std::getline(std::cin, ignored);
}

#endif

std::vector<std::string> make_rotating_a_frame(double radians, int width, int height) {
    const std::vector<std::string> letter = {
        "     #####     ",
        "    #######    ",
        "   #########   ",
        "  ####   ####  ",
        " ####     #### ",
        " ############# ",
        "###############",
        "####       ####",
        "####       ####",
        "####       ####",
        "####       ####",
    };

    const int canvas_width = std::max(20, width);
    const int canvas_height = std::max(12, height);
    std::vector<std::string> canvas(
        static_cast<size_t>(canvas_height),
        std::string(static_cast<size_t>(canvas_width), ' '));
    std::vector<double> depth(
        static_cast<size_t>(canvas_width * canvas_height),
        -1.0e9);

    constexpr int extrusion = 6;
    const double letter_width = static_cast<double>(letter.front().size());
    const double letter_height = static_cast<double>(letter.size());
    const double center_x = (letter_width - 1.0) / 2.0;
    const double center_y = (letter_height - 1.0) / 2.0;
    const double center_z = static_cast<double>(extrusion - 1) / 2.0;
    const double focal = 38.0;
    const double x_scale = std::max(1.4, std::min(2.6, width / 34.0));
    const double y_scale = std::max(0.9, std::min(1.45, height / 18.0));
    const double cos_a = std::cos(radians);
    const double sin_a = std::sin(radians);
    const int origin_col = canvas_width / 2;
    const int origin_row = canvas_height / 2;

    for (size_t row = 0; row < letter.size(); ++row) {
        for (size_t col = 0; col < letter[row].size(); ++col) {
            if (letter[row][col] != '#') {
                continue;
            }
            for (int z = 0; z < extrusion; ++z) {
                const double x = static_cast<double>(col) - center_x;
                const double y = static_cast<double>(row) - center_y;
                const double local_z = static_cast<double>(z) - center_z;
                const double rotated_x = x * cos_a + local_z * sin_a;
                const double rotated_z = -x * sin_a + local_z * cos_a;
                const double perspective = focal / (focal + rotated_z);
                const int draw_col = origin_col + static_cast<int>(std::round(rotated_x * x_scale * perspective));
                const int draw_row = origin_row + static_cast<int>(std::round(y * y_scale * perspective));
                if (draw_row < 0 || draw_row >= canvas_height ||
                    draw_col < 0 || draw_col >= canvas_width) {
                    continue;
                }

                const size_t index = static_cast<size_t>(draw_row * canvas_width + draw_col);
                if (perspective <= depth[index]) {
                    continue;
                }

                depth[index] = perspective;
                char shade = '@';
                if (z <= 1) {
                    shade = '#';
                } else if (z <= 3) {
                    shade = 'O';
                }
                canvas[static_cast<size_t>(draw_row)][static_cast<size_t>(draw_col)] = shade;
                if (draw_col + 1 < canvas_width) {
                    const size_t right_index = index + 1;
                    if (perspective > depth[right_index]) {
                        depth[right_index] = perspective;
                        canvas[static_cast<size_t>(draw_row)][static_cast<size_t>(draw_col + 1)] = shade;
                    }
                }
            }
        }
    }

    return canvas;
}

std::vector<std::string> crop_to_content(const std::vector<std::string>& canvas) {
    int min_row = static_cast<int>(canvas.size());
    int max_row = -1;
    int min_col = canvas.empty() ? 0 : static_cast<int>(canvas.front().size());
    int max_col = -1;

    for (int row = 0; row < static_cast<int>(canvas.size()); ++row) {
        for (int col = 0; col < static_cast<int>(canvas[row].size()); ++col) {
            if (canvas[static_cast<size_t>(row)][static_cast<size_t>(col)] == ' ') {
                continue;
            }
            min_row = std::min(min_row, row);
            max_row = std::max(max_row, row);
            min_col = std::min(min_col, col);
            max_col = std::max(max_col, col);
        }
    }

    if (max_row < min_row || max_col < min_col) {
        return {};
    }

    std::vector<std::string> result;
    result.reserve(static_cast<size_t>(max_row - min_row + 1));
    for (int row = min_row; row <= max_row; ++row) {
        result.push_back(canvas[static_cast<size_t>(row)].substr(
            static_cast<size_t>(min_col),
            static_cast<size_t>(max_col - min_col + 1)));
    }
    return result;
}

void render_centered_frame(const std::vector<std::string>& frame,
                           const TerminalSize& size) {
    const int frame_height = static_cast<int>(frame.size());
    int frame_width = 0;
    for (const auto& line : frame) {
        frame_width = std::max(frame_width, static_cast<int>(line.size()));
    }
    const int top = std::max(1, (size.height - frame_height) / 2 + 1);
    const int left = std::max(1, (size.width - frame_width) / 2 + 1);

    std::cout << "\033[H\033[2J";
    for (int row = 0; row < frame_height; ++row) {
        std::cout << "\033[" << (top + row) << ';' << left << 'H'
                  << frame[static_cast<size_t>(row)];
    }
    std::cout << std::flush;
}

void play_startup_a_animation() {
    StartupTerminalGuard terminal;
    if (!terminal.can_animate()) {
        return;
    }

    constexpr int rotations = 3;
    constexpr int frames_per_rotation = 32;
    constexpr int frame_delay_ms = 35;

    std::cout << "\033[?25l" << std::flush;
    for (int frame = 0; frame < rotations * frames_per_rotation; ++frame) {
        const TerminalSize size = current_terminal_size();
        const double radians = (2.0 * kPi * frame) / frames_per_rotation;
        render_centered_frame(
            crop_to_content(make_rotating_a_frame(radians, size.width, size.height)),
            size);
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_delay_ms));
    }
    std::cout << "\033[H\033[2J\033[?25h" << std::flush;
}

} // namespace

int main() {
    using namespace ftxui;

    show_conhost_status();
    play_startup_a_animation();

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
