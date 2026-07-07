#include "lsp_diagnostics.hpp"

#include "lsp_service.hpp"

#include <algorithm>
#include <chrono>

namespace acecode::lsp {
namespace {

constexpr auto kEditWaitTimeout = std::chrono::milliseconds(5000);

int severity_of(const nlohmann::json& diagnostic) {
    if (diagnostic.is_object() && diagnostic.contains("severity") &&
        diagnostic["severity"].is_number_integer()) {
        return diagnostic["severity"].get<int>();
    }
    return 1; // LSP 允许省略 severity,惯例视为 Error
}

} // namespace

std::string pretty_diagnostic(const nlohmann::json& diagnostic) {
    const char* severity_label = "ERROR";
    switch (severity_of(diagnostic)) {
        case 2: severity_label = "WARN"; break;
        case 3: severity_label = "INFO"; break;
        case 4: severity_label = "HINT"; break;
        default: break;
    }
    long long line = 0;
    long long character = 0;
    if (diagnostic.is_object() && diagnostic.contains("range") &&
        diagnostic["range"].is_object()) {
        const auto& start = diagnostic["range"].value("start", nlohmann::json::object());
        if (start.contains("line") && start["line"].is_number_integer())
            line = start["line"].get<long long>();
        if (start.contains("character") && start["character"].is_number_integer())
            character = start["character"].get<long long>();
    }
    const std::string message =
        diagnostic.is_object() ? diagnostic.value("message", std::string{}) : std::string{};
    return std::string(severity_label) + " [" + std::to_string(line + 1) + ":" +
           std::to_string(character + 1) + "] " + message;
}

std::string report_block(const std::string& display_path,
                         const std::vector<nlohmann::json>& diagnostics) {
    std::vector<const nlohmann::json*> errors;
    for (const auto& item : diagnostics) {
        if (severity_of(item) == 1) errors.push_back(&item);
    }
    if (errors.empty()) return {};

    std::string body;
    const int limit = std::min<int>(kMaxDiagnosticsPerFile, static_cast<int>(errors.size()));
    for (int i = 0; i < limit; ++i) {
        if (i) body += "\n";
        body += pretty_diagnostic(*errors[i]);
    }
    const int more = static_cast<int>(errors.size()) - kMaxDiagnosticsPerFile;
    if (more > 0) body += "\n... and " + std::to_string(more) + " more";

    return "<diagnostics file=\"" + display_path + "\">\n" + body + "\n</diagnostics>";
}

void append_diagnostics_block(std::string& output,
                              const std::string& utf8_path,
                              const std::atomic<bool>* abort_flag) {
    if (!is_initialized()) return;
    auto& svc = service();
    if (!svc.enabled()) return;
    // 廉价前置:无匹配 server(含未安装/broken)时零延迟返回。
    if (!svc.has_server_for(utf8_path)) return;

    AbortProbe should_abort;
    if (abort_flag) {
        should_abort = [abort_flag] { return abort_flag->load(); };
    }
    const auto diagnostics =
        svc.collect_diagnostics_after_write(utf8_path, kEditWaitTimeout, should_abort);
    const std::string block = report_block(utf8_path, diagnostics);
    if (block.empty()) return;
    output += "\n\nLSP errors detected in this file, please fix:\n" + block;
}

} // namespace acecode::lsp
