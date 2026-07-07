#include "lsp_tool.hpp"

#include "../lsp/lsp_service.hpp"
#include "../lsp/lsp_uri.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

namespace acecode {
namespace {

namespace fs = std::filesystem;

constexpr auto kRequestTimeout = std::chrono::milliseconds(15000);
constexpr int kWorkspaceSymbolLimit = 10;

// workspaceSymbol 的符号种类白名单(对齐 opencode):Class / Method /
// Enum / Interface / Function / Variable / Constant / Struct。
const std::set<int> kWorkspaceSymbolKinds = {5, 6, 10, 11, 12, 13, 14, 23};

struct LspToolArgs {
    std::string operation;
    std::string file_path;
    long long line = 1;      // 1-based(与编辑器显示一致)
    long long character = 1; // 1-based
    std::string query;
    std::string error;
};

LspToolArgs parse_args(const std::string& arguments_json) {
    LspToolArgs args;
    nlohmann::json j = nlohmann::json::parse(arguments_json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        args.error = "Invalid tool arguments: expected a JSON object";
        return args;
    }
    if (!j.contains("operation") || !j["operation"].is_string()) {
        args.error = "Missing required parameter: operation";
        return args;
    }
    args.operation = j["operation"].get<std::string>();
    if (!j.contains("file_path") || !j["file_path"].is_string()) {
        args.error = "Missing required parameter: file_path";
        return args;
    }
    args.file_path = j["file_path"].get<std::string>();
    if (j.contains("line") && j["line"].is_number_integer())
        args.line = j["line"].get<long long>();
    if (j.contains("character") && j["character"].is_number_integer())
        args.character = j["character"].get<long long>();
    if (j.contains("query") && j["query"].is_string())
        args.query = j["query"].get<std::string>();
    if (args.line < 1 || args.character < 1) {
        args.error = "line/character are 1-based and must be >= 1";
        return args;
    }
    return args;
}

// 各 client 的结果聚合成单个数组:数组结果摊平,对象结果(hover)整体收纳。
nlohmann::json flatten_results(const std::vector<nlohmann::json>& results) {
    nlohmann::json combined = nlohmann::json::array();
    for (const auto& result : results) {
        if (result.is_array()) {
            for (const auto& item : result) {
                if (!item.is_null()) combined.push_back(item);
            }
        } else if (!result.is_null()) {
            combined.push_back(result);
        }
    }
    return combined;
}

ToolResult execute_lsp(const std::string& arguments_json, const ToolContext& ctx) {
    LspToolArgs args = parse_args(arguments_json);
    if (!args.error.empty()) return ToolResult{args.error, false};

    static const std::set<std::string> kOperations = {
        "goToDefinition", "findReferences", "hover", "documentSymbol",
        "workspaceSymbol", "goToImplementation", "prepareCallHierarchy",
        "incomingCalls", "outgoingCalls",
    };
    if (kOperations.find(args.operation) == kOperations.end()) {
        return ToolResult{"Unknown operation: " + args.operation, false};
    }

    if (!lsp::is_initialized() || !lsp::service().enabled()) {
        return ToolResult{"LSP integration is disabled.", false};
    }
    auto& svc = lsp::service();

    // 相对路径按会话 cwd 解析(与文件工具一致)。
    std::string base = ctx.cwd.empty() ? current_path_utf8() : ctx.cwd;
    fs::path resolved = path_from_utf8(args.file_path);
    if (!resolved.is_absolute()) resolved = path_from_utf8(base) / resolved;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(resolved, ec);
    const std::string file = path_to_utf8(ec ? resolved : canonical);

    if (!fs::is_regular_file(path_from_utf8(file), ec) || ec) {
        return ToolResult{"File not found: " + file, false};
    }
    if (!svc.has_server_for(file)) {
        return ToolResult{"No LSP server available for this file type.", false};
    }

    lsp::AbortProbe should_abort;
    if (ctx.abort_flag) {
        const std::atomic<bool>* flag = ctx.abort_flag;
        should_abort = [flag] { return flag->load(); };
    }

    const std::string uri = lsp::path_to_file_uri(file);
    const nlohmann::json position = {
        {"line", args.line - 1},
        {"character", args.character - 1},
    };
    const nlohmann::json text_document = {{"uri", uri}};
    const nlohmann::json loc_params = {
        {"textDocument", text_document},
        {"position", position},
    };

    std::vector<nlohmann::json> results;
    if (args.operation == "goToDefinition") {
        results = svc.request_for_file(file, "textDocument/definition", loc_params,
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "findReferences") {
        nlohmann::json params = loc_params;
        params["context"] = {{"includeDeclaration", true}};
        results = svc.request_for_file(file, "textDocument/references", params,
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "hover") {
        results = svc.request_for_file(file, "textDocument/hover", loc_params,
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "documentSymbol") {
        results = svc.request_for_file(file, "textDocument/documentSymbol",
                                       {{"textDocument", text_document}},
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "workspaceSymbol") {
        results = svc.request_for_file(file, "workspace/symbol",
                                       {{"query", args.query}},
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "goToImplementation") {
        results = svc.request_for_file(file, "textDocument/implementation", loc_params,
                                       kRequestTimeout, should_abort);
    } else if (args.operation == "prepareCallHierarchy") {
        results = svc.request_for_file(file, "textDocument/prepareCallHierarchy",
                                       loc_params, kRequestTimeout, should_abort);
    } else if (args.operation == "incomingCalls") {
        results = svc.call_hierarchy_for_file(file, loc_params,
                                              "callHierarchy/incomingCalls",
                                              kRequestTimeout, should_abort);
    } else if (args.operation == "outgoingCalls") {
        results = svc.call_hierarchy_for_file(file, loc_params,
                                              "callHierarchy/outgoingCalls",
                                              kRequestTimeout, should_abort);
    }

    nlohmann::json combined = flatten_results(results);

    if (args.operation == "workspaceSymbol") {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& symbol : combined) {
            if (!symbol.is_object()) continue;
            const int kind = symbol.contains("kind") && symbol["kind"].is_number_integer()
                ? symbol["kind"].get<int>() : 0;
            if (kWorkspaceSymbolKinds.count(kind) == 0) continue;
            filtered.push_back(symbol);
            if (static_cast<int>(filtered.size()) >= kWorkspaceSymbolLimit) break;
        }
        combined = std::move(filtered);
    }

    if (combined.empty()) {
        return ToolResult{"No results found for " + args.operation, true};
    }
    return ToolResult{combined.dump(2), true};
}

} // namespace

ToolImpl create_lsp_tool() {
    ToolDef def;
    def.name = "lsp";
    def.description =
        "Query Language Server Protocol (LSP) servers for code intelligence.\n"
        "Operations:\n"
        "- goToDefinition: find where the symbol at a position is defined\n"
        "- findReferences: find all references to the symbol at a position\n"
        "- hover: type/documentation info for the symbol at a position\n"
        "- documentSymbol: all symbols (functions, classes, ...) in the file\n"
        "- workspaceSymbol: project-wide symbol search (uses `query`; the file "
        "only selects which language server handles the search)\n"
        "- goToImplementation: implementations of the interface/virtual at a position\n"
        "- prepareCallHierarchy: call-hierarchy item at a position\n"
        "- incomingCalls: functions calling the function at a position\n"
        "- outgoingCalls: functions called by the function at a position\n"
        "line/character are 1-based, exactly as shown in editors. Positions must "
        "point at a symbol name. Results are raw LSP JSON (0-based positions). "
        "Requires a language server installed for the file type.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"operation", {
                {"type", "string"},
                {"enum", nlohmann::json::array({
                    "goToDefinition", "findReferences", "hover", "documentSymbol",
                    "workspaceSymbol", "goToImplementation", "prepareCallHierarchy",
                    "incomingCalls", "outgoingCalls"})},
                {"description", "The LSP operation to perform"},
            }},
            {"file_path", {
                {"type", "string"},
                {"description", "Absolute or CWD-relative path to the file"},
            }},
            {"line", {
                {"type", "integer"},
                {"description", "1-based line number (as shown in editors). "
                                "Required for position-based operations."},
            }},
            {"character", {
                {"type", "integer"},
                {"description", "1-based character offset (as shown in editors). "
                                "Required for position-based operations."},
            }},
            {"query", {
                {"type", "string"},
                {"description", "Search query for workspaceSymbol. Empty lists all symbols."},
            }},
        }},
        {"required", nlohmann::json::array({"operation", "file_path"})},
    });

    return ToolImpl{def, execute_lsp, /*is_read_only=*/true};
}

} // namespace acecode
