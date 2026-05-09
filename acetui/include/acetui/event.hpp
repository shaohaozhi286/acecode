// acetui/event.hpp — 终端事件值类型。
//
// phase 1 只填实 Key + Resize;Paste / Mouse 类型保留占位,phase 2 落地
// bracketed paste 与 SGR mouse 解析时再实做产生路径。

#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace acetui {

// 修饰键位标志。可按位或合并。
enum class Modifier : std::uint8_t {
    None  = 0,
    Ctrl  = 1u << 0,
    Alt   = 1u << 1,
    Shift = 1u << 2,
};

inline Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<std::uint8_t>(a) |
                                 static_cast<std::uint8_t>(b));
}
inline Modifier operator&(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<std::uint8_t>(a) &
                                 static_cast<std::uint8_t>(b));
}
inline bool has_mod(Modifier mods, Modifier flag) {
    return (static_cast<std::uint8_t>(mods) &
            static_cast<std::uint8_t>(flag)) != 0;
}

// 约定:用 ASCII 控制码作为特殊键的 codepoint sentinel,可见字符直接
// 用其 Unicode codepoint。方向键 / Home / End / Delete 这些在 ASCII 里
// 没有,占 Unicode Private Use Area (U+E000..)做 sentinel。
namespace key {
constexpr char32_t kBackspace = 0x08;
constexpr char32_t kEnter     = 0x0D;
constexpr char32_t kEsc       = 0x1B;

constexpr char32_t kLeft   = 0xE000;
constexpr char32_t kRight  = 0xE001;
constexpr char32_t kUp     = 0xE002;
constexpr char32_t kDown   = 0xE003;
constexpr char32_t kHome   = 0xE004;
constexpr char32_t kEnd    = 0xE005;
constexpr char32_t kDelete = 0xE006;
}  // namespace key

struct KeyEvent {
    char32_t codepoint = 0;
    Modifier mods      = Modifier::None;
};

struct ResizeEvent {
    int width  = 0;
    int height = 0;
};

// 占位:bracketed paste 攒到 phase 2 才会真正产生这种事件。
struct PasteEvent {
    std::string text;
};

// 占位:SGR mouse 解析在 phase 2 才落地。
struct MouseEvent {
    enum class Kind { Down, Up, Drag, ScrollUp, ScrollDown } kind = Kind::Down;
    int x = 0;  // 1-based
    int y = 0;  // 1-based
    Modifier mods = Modifier::None;
};

using Event = std::variant<KeyEvent, ResizeEvent, PasteEvent, MouseEvent>;

}  // namespace acetui
