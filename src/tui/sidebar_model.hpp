#pragma once

#include "../tui_state.hpp"

#include <string>
#include <vector>

namespace acecode::tui {

struct SidebarFileChange {
    std::string file;
    std::string display_file;
    int additions = 0;
    int deletions = 0;
};

std::vector<SidebarFileChange> collect_sidebar_file_changes(
    const std::vector<TuiState::Message>& messages,
    const std::string& workspace_cwd = {});

} // namespace acecode::tui
