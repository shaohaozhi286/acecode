#pragma once

#include "../permissions.hpp"

#include <string>
#include <vector>

namespace acecode {

struct ModePickerOption {
    PermissionMode mode = PermissionMode::Default;
    std::string name;
    std::string description;
    bool is_current = false;
};

std::vector<ModePickerOption> build_mode_picker_options(PermissionMode current_mode);

} // namespace acecode
