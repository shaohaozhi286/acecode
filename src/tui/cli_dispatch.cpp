#include "tui/cli_dispatch.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "version.hpp"
#include "config/config.hpp"
#include "provider/models_dev_registry.hpp"
#include "commands/configure.hpp"
#include "upgrade/upgrade.hpp"
#include "upgrade/apply.hpp"
#include "upgrade/manifest.hpp"
#include "daemon/cli.hpp"
#include "tui/terminal_utils.hpp"

#ifdef _WIN32
#include "daemon/service_win.hpp"
#endif

namespace acecode { namespace cli {

using namespace acecode;

void configure_process_environment() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetEnvironmentVariableA("NoDefaultCurrentDirectoryInExePath", "1");
#endif
}

std::vector<std::string> argv_tail(int argc, char* argv[], int start) {
    std::vector<std::string> tokens;
    for (int i = start; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    return tokens;
}

std::string executable_path_from_argv(int argc, char* argv[]) {
    return (argc > 0 && argv[0]) ? std::string(argv[0]) : std::string();
}

bool is_version_command_arg(const std::string& arg) {
    return arg == "version" || arg == "-version" || arg == "--version" ||
           arg == "/version";
}

std::optional<int> dispatch_non_tui_command(int argc, char* argv[]) {
    const std::string exe_path = executable_path_from_argv(argc, argv);

    if (argc >= 2 && is_version_command_arg(argv[1] ? std::string(argv[1]) : std::string())) {
        std::cout << "acecode v" ACECODE_VERSION << "\n";
        return 0;
    }

    if (argc >= 2 && (std::string(argv[1]) == "upgrade" ||
                      std::string(argv[1]) == "update")) {
        bool force_update = false;
        std::optional<std::string> server_override;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i] ? std::string(argv[i]) : std::string();
            if (arg == "--force") {
                force_update = true;
                continue;
            }
            constexpr const char* kServerPrefix = "--server=";
            if (arg.rfind(kServerPrefix, 0) == 0) {
                server_override = arg.substr(std::char_traits<char>::length(kServerPrefix));
                continue;
            }
            if (arg == "--server") {
                std::cerr << "acecode " << argv[1] << ": missing value for --server\n"
                          << "usage: acecode " << argv[1]
                          << " [--force] [--server=<url>]\n";
                return 64;
            }
            std::cerr << "acecode " << argv[1] << ": unknown option: " << arg << "\n"
                      << "usage: acecode " << argv[1]
                      << " [--force] [--server=<url>]\n";
            return 64;
        }
        AppConfig config = load_config();
        if (server_override.has_value()) {
            std::string server_error;
            if (!upgrade::apply_upgrade_server_override(
                    config, *server_override, &server_error)) {
                std::cerr << "acecode " << argv[1] << ": " << server_error << "\n"
                          << "usage: acecode " << argv[1]
                          << " [--force] [--server=<url>]\n";
                return 64;
            }
            try {
                save_config(config);
            } catch (const std::exception& e) {
                std::cerr << "acecode " << argv[1]
                          << ": failed to save update server: " << e.what() << "\n";
                return 1;
            }
        }
        return upgrade::run_upgrade_command(
            config, exe_path, ACECODE_VERSION, std::cout, std::cerr, force_update);
    }

    if (argc >= 2 && std::string(argv[1]) == "--apply-update") {
        return upgrade::run_apply_update_command(
            argv_tail(argc, argv, 2), std::cout, std::cerr,
            upgrade::current_target());
    }

    if (argc >= 2 && std::string(argv[1]) == "daemon") {
        return daemon::cli::run(argv_tail(argc, argv, 2), exe_path);
    }

    if (argc >= 2 && std::string(argv[1]) == "service") {
#ifdef _WIN32
        return daemon::service_win::run_cli(argv_tail(argc, argv, 2), exe_path);
#else
        std::cerr << "acecode: native `service` subcommand is Windows-only;\n"
                     "         on Linux/macOS use `acecode daemon --foreground`\n"
                     "         under systemd / launchd (see README for sample units).\n";
        return 65;
#endif
    }

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--service-main") {
#ifdef _WIN32
            return daemon::service_win::run_service_main_dispatcher();
#else
            std::cerr << "--service-main is Windows-only\n";
            return 64;
#endif
        }
    }

#ifdef _WIN32
    if (argc == 1) {
        AppConfig cfg_probe = load_config();
        if (cfg_probe.daemon.auto_start_on_double_click) {
            DWORD procs[2] = {0, 0};
            DWORD n = ::GetConsoleProcessList(procs, 2);
            if (n == 1) {
                std::vector<std::string> tokens = {"start"};
                return daemon::cli::run(tokens, exe_path);
            }
        }
    }
#endif

    return std::nullopt;
}

int validate_models_registry_command(const std::string& argv0_dir) {
    AppConfig config = load_config();
    acecode::tui::seed_default_skills_if_first_initialization(argv0_dir);
    acecode::initialize_registry(config, argv0_dir);
    const auto& src = acecode::current_registry_source();
    auto registry = acecode::current_registry();
    if (!registry || registry->empty()) {
        std::cerr << "models.dev registry not found or empty\n";
        return 1;
    }
    size_t actual_models = 0;
    for (auto it = registry->begin(); it != registry->end(); ++it) {
        if (!it->is_object()) continue;
        auto m = it->find("models");
        if (m == it->end()) continue;
        if (m->is_object()) actual_models += m->size();
        else if (m->is_array()) actual_models += m->size();
    }
    std::cout << "models.dev registry OK: " << registry->size() << " providers, "
              << actual_models << " models, source=" << src.path_or_url << "\n";
    if (src.manifest && src.manifest->is_object()) {
        const auto& m = *src.manifest;
        if (m.contains("model_count") && m["model_count"].is_number_integer()) {
            size_t expected = static_cast<size_t>(m["model_count"].get<int>());
            if (expected != actual_models) {
                std::cerr << "MANIFEST.json model_count=" << expected
                          << " disagrees with actual " << actual_models << "\n";
                return 1;
            }
        }
    }
    return 0;
}

std::optional<int> run_pre_tui_command(const InteractiveCliOptions& cli,
                                       const std::string& argv0_dir) {
    if (cli.run_configure_cmd) {
        AppConfig config = load_config();
        acecode::tui::seed_default_skills_if_first_initialization(argv0_dir);
        acecode::initialize_registry(config, argv0_dir);
        return acecode::run_configure(config);
    }

    if (cli.validate_models_registry_cmd) {
        return validate_models_registry_command(argv0_dir);
    }

    return std::nullopt;
}

}} // namespace acecode::cli
