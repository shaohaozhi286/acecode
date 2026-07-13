#include "mode_picker.hpp"

#include <array>

namespace acecode {

std::vector<ModePickerOption> build_mode_picker_options(PermissionMode current_mode) {
    static constexpr std::array<PermissionMode, 4> kModes = {
        PermissionMode::Default,
        PermissionMode::AcceptEdits,
        PermissionMode::Plan,
        PermissionMode::Yolo,
    };

    std::vector<ModePickerOption> options;
    options.reserve(kModes.size());
    for (const PermissionMode mode : kModes) {
        options.push_back({
            mode,
            PermissionManager::mode_name(mode),
            PermissionManager::mode_description(mode),
            mode == current_mode,
        });
    }
    return options;
}

} // namespace acecode
