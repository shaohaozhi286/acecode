#pragma once

#include <string>
#include <string_view>

namespace acecode {

// Set the terminal/window title. Windows uses the console title API to avoid
// leaking OSC escape bytes in legacy cmd.exe; POSIX terminals use OSC 2.
// Empty text clears the title.
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
