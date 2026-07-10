#include "cli/interactive_options.hpp"

#include <cstdlib>
#include <string>

namespace acecode {

bool parse_question_policy_value(const std::string& value,
                                 std::string& policy,
                                 int& timeout_seconds,
                                 std::string& error) {
    policy.clear();
    timeout_seconds = 0;
    error.clear();

    if (value == "ask" || value == "deny" || value == "timeout") {
        policy = value;
        return true;
    }

    const std::string prefix = "timeout:";
    if (value.rfind(prefix, 0) == 0) {
        const std::string secs = value.substr(prefix.size());
        if (secs.empty() ||
            secs.find_first_not_of("0123456789") != std::string::npos) {
            error = "invalid --question-policy timeout seconds: \"" + secs +
                    "\" (expected an integer in [5, 3600])";
            return false;
        }
        // 全数字且限长,strtol 不会溢出到未定义行为;超长直接判越界。
        if (secs.size() > 4) {
            error = "--question-policy timeout seconds out of range [5, 3600]: " + secs;
            return false;
        }
        const long parsed = std::strtol(secs.c_str(), nullptr, 10);
        if (parsed < 5 || parsed > 3600) {
            error = "--question-policy timeout seconds out of range [5, 3600]: " + secs;
            return false;
        }
        policy = "timeout";
        timeout_seconds = static_cast<int>(parsed);
        return true;
    }

    error = "invalid --question-policy value: \"" + value +
            "\" (expected ask | deny | timeout[:seconds])";
    return false;
}

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
        } else if (arg == "--question-policy" || arg == "-question-policy") {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                cli.question_policy_error =
                    "--question-policy requires a value: ask | deny | timeout[:seconds]";
                continue;
            }
            parse_question_policy_value(argv[++i], cli.question_policy,
                                        cli.question_timeout_seconds,
                                        cli.question_policy_error);
        }
    }
    return cli;
}

} // namespace acecode
