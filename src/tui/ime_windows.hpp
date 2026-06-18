#pragma once
// Windows IME composition window positioning. Win32-only.
// Extracted from main.cpp lines 1262-1468.

#ifdef _WIN32

#include <string>

namespace acecode { namespace tui {

// Position the IME composition and candidate windows near the text input caret.
void update_ime_composition_window(const std::string& input_text,
                                   bool show_bottom_bar,
                                   bool confirm_pending,
                                   const std::string& confirm_tool_name);

}} // namespace acecode::tui

#endif // _WIN32
