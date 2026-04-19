#pragma once

#include <string>
#include <string_view>

namespace acecode {

// Write `ESC ] 2 ; <text> ESC \` to stdout to set the terminal/window title.
// Empty text emits the same sequence with an empty body, which most terminals
// interpret as "let the next program (e.g. shell) repaint the title".
void set_terminal_title(std::string_view text);

// Convenience: set_terminal_title("").
void clear_terminal_title();

// Validate and clamp a user-supplied title in place.
//
// Rejects (returns false, sets error_out): any C0 control byte except that the
// caller has already excluded — specifically rejects ESC (0x1B), BEL (0x07),
// NUL (0x00), CR (0x0D), LF (0x0A), and HT (0x09). Tabs/newlines are rejected
// because OSC 2 is a single-line construct.
//
// On success (returns true): if the byte length exceeds 256, truncates to a
// UTF-8 character boundary <= 256 bytes. error_out is set to a short note when
// truncation occurred ("truncated"), empty otherwise.
bool sanitize_title(std::string& inout, std::string& error_out);

} // namespace acecode
