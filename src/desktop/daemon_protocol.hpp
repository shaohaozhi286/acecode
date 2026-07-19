#pragma once

namespace acecode::desktop {

// Increment only when the native Desktop shell and its managed daemon can no
// longer safely share lifecycle/runtime semantics. ACECode application versions
// may differ while this protocol remains compatible.
inline constexpr int kDesktopDaemonProtocolVersion = 1;
inline constexpr const char* kDesktopManagedRuntimeKind = "acecode-desktop";

} // namespace acecode::desktop
