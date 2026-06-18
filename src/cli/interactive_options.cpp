#include "cli/interactive_options.hpp"

#include <string>

namespace acecode {

InteractiveCliOptions parse_interactive_cli_options(int argc, char* argv[]) {
    InteractiveCliOptions cli;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-dangerous" || arg == "--dangerous" ||
            arg == "-yolo" || arg == "--yolo") {
            cli.dangerous_mode = true;
        } else if (arg == "configure") {
            cli.run_configure_cmd = true;
        } else if (arg == "--validate-models-registry") {
            cli.validate_models_registry_cmd = true;
        } else if (arg == "-alt-screen" || arg == "--alt-screen") {
            cli.force_alt_screen = true;
        } else if (arg == "-r") {
            cli.resume_picker_on_startup = true;
        } else if (arg == "--resume") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cli.resume_session_id = argv[++i];
            } else {
                cli.resume_latest = true;
            }
        }
    }
    return cli;
}

} // namespace acecode
