#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int delay_ms = 0;
    int call_delay_ms = 0;  // 仅延迟 tools/call 响应,模拟慢工具(abort 测试用)
    bool no_tools = false;
    bool fail_initialize = false;
    std::string tool_name = "echo";
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--delay-ms" && i + 1 < argc) {
            opts.delay_ms = std::atoi(argv[++i]);
        } else if (arg == "--call-delay-ms" && i + 1 < argc) {
            opts.call_delay_ms = std::atoi(argv[++i]);
        } else if (arg == "--no-tools") {
            opts.no_tools = true;
        } else if (arg == "--fail-initialize") {
            opts.fail_initialize = true;
        } else if (arg == "--tool" && i + 1 < argc) {
            opts.tool_name = argv[++i];
        }
    }
    return opts;
}

void maybe_delay(const Options& opts) {
    if (opts.delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.delay_ms));
    }
}

void write_response(const nlohmann::json& response) {
    std::cout << response.dump() << '\n';
    std::cout.flush();
}

nlohmann::json tool_schema(const std::string& name) {
    return nlohmann::json{
        {"name", name},
        {"description", "Test MCP tool " + name},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"text", {
                    {"type", "string"}
                }}
            }}
        }}
    };
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parse_options(argc, argv);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(line);
        } catch (...) {
            continue;
        }

        const std::string method = request.value("method", "");
        const bool has_id = request.contains("id") && !request["id"].is_null();
        if (!has_id) {
            continue;
        }

        if (method == "initialize") {
            maybe_delay(opts);
            if (opts.fail_initialize) {
                write_response({
                    {"jsonrpc", "2.0"},
                    {"id", request["id"]},
                    {"error", {
                        {"code", -32000},
                        {"message", "test initialize failure"}
                    }}
                });
            } else {
                write_response({
                    {"jsonrpc", "2.0"},
                    {"id", request["id"]},
                    {"result", {
                        {"protocolVersion", "2024-11-05"},
                        {"capabilities", {
                            {"tools", nlohmann::json::object()}
                        }},
                        {"serverInfo", {
                            {"name", "acecode-test-mcp"},
                            {"version", "1.0.0"}
                        }}
                    }}
                });
            }
        } else if (method == "tools/list") {
            maybe_delay(opts);
            nlohmann::json tools = nlohmann::json::array();
            if (!opts.no_tools) {
                tools.push_back(tool_schema(opts.tool_name));
            }
            write_response({
                {"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result", {
                    {"tools", tools}
                }}
            });
        } else if (method == "tools/call") {
            if (opts.call_delay_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(opts.call_delay_ms));
            }
            const auto params = request.value("params", nlohmann::json::object());
            const auto args = params.value("arguments", nlohmann::json::object());
            const std::string text = args.value("text", "ok");
            write_response({
                {"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result", {
                    {"content", nlohmann::json::array({
                        {
                            {"type", "text"},
                            {"text", text}
                        }
                    })}
                }}
            });
        } else if (method == "ping") {
            write_response({
                {"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"result", nlohmann::json::object()}
            });
        } else {
            write_response({
                {"jsonrpc", "2.0"},
                {"id", request["id"]},
                {"error", {
                    {"code", -32601},
                    {"message", "method not found"}
                }}
            });
        }
    }
    return 0;
}
