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
        } else if (arg == "-w" || arg == "--worktree") {
            cli.worktree_enabled = true;
            // 允许 "#123" 这样的 PR 引用作值,所以只把 "--xxx" 视为下一个
            // flag;单独的 "-" 开头短参数极少作为 worktree 名,一并排除。
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cli.worktree_name = argv[++i];
            }
        } else if (arg.rfind("--worktree=", 0) == 0) {
            cli.worktree_enabled = true;
            cli.worktree_name = arg.substr(std::string("--worktree=").size());
        }
    }
    return cli;
}

} // namespace acecode
