#pragma once

#include <string>

namespace acecode {

struct InteractiveCliOptions {
    bool dangerous_mode = false;
    bool run_configure_cmd = false;
    bool validate_models_registry_cmd = false;
    bool resume_latest = false;
    bool resume_picker_on_startup = false;
    bool force_alt_screen = false;
    std::string resume_session_id;

    bool direct_resume_requested() const {
        return resume_latest || !resume_session_id.empty();
    }
};

InteractiveCliOptions parse_interactive_cli_options(int argc, char* argv[]);

} // namespace acecode
