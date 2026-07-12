#include "window_background.hpp"

namespace acecode::desktop {

namespace {

std::optional<int> hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return std::nullopt;
}

std::optional<std::uint8_t> hex_byte(std::string_view two_digits) {
    auto high = hex_digit_value(two_digits[0]);
    auto low = hex_digit_value(two_digits[1]);
    if (!high || !low) return std::nullopt;
    return static_cast<std::uint8_t>(*high * 16 + *low);
}

char hex_digit_upper(std::uint8_t value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + (value - 10));
}

void append_hex_byte_upper(std::string& out, std::uint8_t value) {
    out.push_back(hex_digit_upper(static_cast<std::uint8_t>(value >> 4)));
    out.push_back(hex_digit_upper(static_cast<std::uint8_t>(value & 0x0F)));
}

} // namespace

std::optional<WindowBackgroundColor> parse_window_background_color(std::string_view text) {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6) return std::nullopt;

    auto r = hex_byte(text.substr(0, 2));
    auto g = hex_byte(text.substr(2, 2));
    auto b = hex_byte(text.substr(4, 2));
    if (!r || !g || !b) return std::nullopt;
    return WindowBackgroundColor{*r, *g, *b};
}

std::string webview2_default_background_env_value(WindowBackgroundColor color) {
    std::string out = "FF";
    append_hex_byte_upper(out, color.r);
    append_hex_byte_upper(out, color.g);
    append_hex_byte_upper(out, color.b);
    return out;
}

} // namespace acecode::desktop
