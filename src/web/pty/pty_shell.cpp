// resolve_console_shell / pty_backend_kind_name:平台无关纯逻辑,进
// acecode_testable 供单测直接覆盖(spawn 实现在 pty_backend_{win,posix}.cpp)。

#include "pty_backend.hpp"

#include "../../utils/encoding.hpp"

namespace acecode {

const char* pty_backend_kind_name(PtyBackendKind kind) {
    switch (kind) {
        case PtyBackendKind::ConPty:   return "conpty";
        case PtyBackendKind::Winpty:   return "winpty";
        case PtyBackendKind::Pipe:     return "pipe";
        case PtyBackendKind::PosixPty: return "posix";
    }
    return "unknown";
}

std::string resolve_console_shell(const std::string& configured) {
    if (!configured.empty()) return configured;
#ifdef _WIN32
    std::string comspec = getenv_utf8("COMSPEC");
    return comspec.empty() ? "cmd.exe" : comspec;
#else
    std::string shell = getenv_utf8("SHELL");
    return shell.empty() ? "/bin/sh" : shell;
#endif
}

} // namespace acecode
