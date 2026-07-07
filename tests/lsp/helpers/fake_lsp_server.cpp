// 测试辅助:最小 LSP server(stdio + Content-Length 分帧),让
// LspClient 集成测试走真实的进程/管道/握手路径,不依赖外部 node /
// 真实 clangd。独立 main(),不进 gtest 发现;只链 nlohmann_json。
//
// 行为(由命令行开关控制):
//   (默认)        initialize 正常应答;didOpen/didChange 后立即
//                  publishDiagnostics(1 条 ERROR,带 version 回显);
//                  hover / definition 固定应答;shutdown/exit 干净退出
//   --slow-init N  收到 initialize 后先 sleep N 毫秒再应答(握手超时用例)
//   --no-diagnostics  didOpen/didChange 后不推诊断(等待超时/abort 用例)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using nlohmann::json;

void write_frame(const json& message) {
    const std::string body = message.dump();
    std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::fwrite(frame.data(), 1, frame.size(), stdout);
    std::fflush(stdout);
}

// 阻塞读一条完整消息;流结束返回 false。
bool read_frame(json& out) {
    // 读头部直到 \r\n\r\n
    std::string header;
    int c;
    while ((c = std::fgetc(stdin)) != EOF) {
        header.push_back(static_cast<char>(c));
        if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    if (c == EOF) return false;

    std::size_t content_length = 0;
    std::size_t pos = header.find("Content-Length:");
    if (pos == std::string::npos) pos = header.find("content-length:");
    if (pos == std::string::npos) return false;
    content_length = std::strtoull(header.c_str() + pos + 15, nullptr, 10);

    std::string body(content_length, '\0');
    std::size_t got = std::fread(body.data(), 1, content_length, stdin);
    if (got != content_length) return false;
    out = json::parse(body, nullptr, false);
    return !out.is_discarded();
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    int slow_init_ms = 0;
    bool publish_diagnostics = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--slow-init") == 0 && i + 1 < argc) {
            slow_init_ms = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-diagnostics") == 0) {
            publish_diagnostics = false;
        }
    }

    json message;
    while (read_frame(message)) {
        const std::string method = message.value("method", std::string{});
        const json id = message.value("id", json());
        const json params = message.value("params", json::object());

        if (method == "initialize") {
            if (slow_init_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(slow_init_ms));
            }
            write_frame({{"jsonrpc", "2.0"},
                         {"id", id},
                         {"result", {{"capabilities", {{"textDocumentSync", 1}}}}}});
        } else if (method == "shutdown") {
            write_frame({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
        } else if (method == "exit") {
            return 0;
        } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
            if (publish_diagnostics && params.contains("textDocument")) {
                const json& doc = params["textDocument"];
                json diag = {
                    {"severity", 1},
                    {"range", {{"start", {{"line", 0}, {"character", 0}}},
                               {"end", {{"line", 0}, {"character", 1}}}}},
                    {"message", "fake error"},
                };
                json publish = {{"jsonrpc", "2.0"},
                                {"method", "textDocument/publishDiagnostics"},
                                {"params", {{"uri", doc.value("uri", "")},
                                            {"diagnostics", json::array({diag})}}}};
                if (doc.contains("version")) {
                    publish["params"]["version"] = doc["version"];
                }
                write_frame(publish);
            }
        } else if (method == "textDocument/hover") {
            write_frame({{"jsonrpc", "2.0"},
                         {"id", id},
                         {"result", {{"contents", "fake hover"}}}});
        } else if (method == "textDocument/definition") {
            write_frame({{"jsonrpc", "2.0"},
                         {"id", id},
                         {"result", json::array({{
                             {"uri", "file:///fake/def.cpp"},
                             {"range", {{"start", {{"line", 4}, {"character", 2}}},
                                        {"end", {{"line", 4}, {"character", 8}}}}},
                         }})}});
        } else if (!id.is_null()) {
            // 其余请求一律 MethodNotFound —— 客户端应静默吞掉。
            write_frame({{"jsonrpc", "2.0"},
                         {"id", id},
                         {"error", {{"code", -32601}, {"message", "not implemented"}}}});
        }
        // 通知(initialized / didChangeWatchedFiles / ...)静默忽略。
    }
    return 0;
}
