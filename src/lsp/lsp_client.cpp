#include "lsp_client.hpp"

#include "lsp_frame.hpp"
#include "lsp_uri.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace acecode::lsp {
namespace {

constexpr auto kPullRequestTimeout = std::chrono::milliseconds(3000);
constexpr auto kPushDebounce = std::chrono::milliseconds(150);
constexpr auto kAbortPollSlice = std::chrono::milliseconds(50);
// touch 与 wait 之间到达的 push(无 version 字段时)也算新鲜的宽限窗。
constexpr auto kFreshnessGrace = std::chrono::milliseconds(500);

std::int64_t current_process_id() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

std::optional<std::string> read_file_utf8(const std::string& utf8_path) {
    std::ifstream in(path_from_utf8(utf8_path), std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string language_id_for(const std::string& utf8_path) {
    const auto dot = utf8_path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string{} : utf8_path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // 常见扩展 → LSP languageId。未覆盖的走 plaintext,server 通常仍按
    // 文件名/内容自行识别。
    static const std::map<std::string, std::string> kMap = {
        {".c", "c"},           {".h", "c"},
        {".cc", "cpp"},        {".cpp", "cpp"},      {".cxx", "cpp"},
        {".hpp", "cpp"},       {".hh", "cpp"},       {".hxx", "cpp"},
        {".m", "objective-c"}, {".mm", "objective-cpp"},
        {".ts", "typescript"}, {".tsx", "typescriptreact"},
        {".mts", "typescript"},{".cts", "typescript"},
        {".js", "javascript"}, {".jsx", "javascriptreact"},
        {".mjs", "javascript"},{".cjs", "javascript"},
        {".py", "python"},     {".pyi", "python"},
        {".go", "go"},
        {".rs", "rust"},
        {".java", "java"},
        {".rb", "ruby"},
        {".php", "php"},
        {".cs", "csharp"},
        {".swift", "swift"},
        {".kt", "kotlin"},     {".kts", "kotlin"},
        {".lua", "lua"},
        {".sh", "shellscript"},{".bash", "shellscript"},
        {".json", "json"},     {".jsonc", "jsonc"},
        {".yaml", "yaml"},     {".yml", "yaml"},
        {".toml", "toml"},
        {".md", "markdown"},
        {".html", "html"},     {".css", "css"},
        {".vue", "vue"},       {".svelte", "svelte"},
        {".zig", "zig"},
        {".dart", "dart"},
    };
    auto it = kMap.find(ext);
    return it == kMap.end() ? "plaintext" : it->second;
}

// UTF-8 文本的 UTF-16 code unit 计数(LSP position.character 的单位)。
std::size_t utf16_length(const std::string& utf8) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) { i += 1; count += 1; }
        else if ((c >> 5) == 0x6) { i += 2; count += 1; }
        else if ((c >> 4) == 0xE) { i += 3; count += 1; }
        else if ((c >> 3) == 0x1E) { i += 4; count += 2; } // 补充平面 → 代理对
        else { i += 1; count += 1; } // 非法字节,按 1 计
    }
    return count;
}

// 全文的 LSP end 位置(didChange incremental 模式下做整篇替换用)。
nlohmann::json end_position_of(const std::string& text) {
    std::size_t line = 0;
    std::size_t last_line_start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            last_line_start = i + 1;
        }
    }
    std::string last_line = text.substr(last_line_start);
    if (!last_line.empty() && last_line.back() == '\r') last_line.pop_back();
    return {{"line", line}, {"character", utf16_length(last_line)}};
}

std::string dedupe_key(const nlohmann::json& diag) {
    nlohmann::json key = nlohmann::json::object();
    if (diag.contains("severity")) key["severity"] = diag["severity"];
    if (diag.contains("message")) key["message"] = diag["message"];
    if (diag.contains("source")) key["source"] = diag["source"];
    if (diag.contains("code")) key["code"] = diag["code"];
    if (diag.contains("range")) key["range"] = diag["range"];
    return key.dump();
}

std::vector<nlohmann::json> dedupe_diagnostics(std::vector<nlohmann::json> items) {
    std::set<std::string> seen;
    std::vector<nlohmann::json> out;
    out.reserve(items.size());
    for (auto& item : items) {
        if (seen.insert(dedupe_key(item)).second) out.push_back(std::move(item));
    }
    return out;
}

// workspace/configuration 的 section 取值:按 '.' 逐级下钻 initialization。
nlohmann::json configuration_value(const nlohmann::json& settings,
                                   const std::string& section) {
    if (section.empty()) return settings.is_null() ? nlohmann::json() : settings;
    const nlohmann::json* cursor = &settings;
    std::size_t pos = 0;
    while (pos <= section.size()) {
        std::size_t next = section.find('.', pos);
        if (next == std::string::npos) next = section.size();
        const std::string key = section.substr(pos, next - pos);
        if (!cursor->is_object() || !cursor->contains(key)) return nullptr;
        cursor = &(*cursor)[key];
        pos = next + 1;
    }
    return *cursor;
}

} // namespace

std::unique_ptr<LspClient> LspClient::create(CreateOptions options, std::string* error) {
    std::unique_ptr<LspClient> client(new LspClient());
    client->server_id_ = options.server_id;
    client->root_ = options.root;
    client->initialization_ = options.initialization;

    if (!client->process_.start(options.spawn, error)) {
        return nullptr;
    }
    client->running_.store(true);
    client->reader_ = std::thread(&LspClient::read_loop, client.get());

    std::string handshake_error;
    {
        // 握手复用通用 request 通道,把超时限制在 options.initialize_timeout。
        nlohmann::json capabilities = {
            {"window", {{"workDoneProgress", true}}},
            {"workspace", {
                {"configuration", true},
                {"didChangeWatchedFiles", {{"dynamicRegistration", true}}},
                {"diagnostics", {{"refreshSupport", false}}},
            }},
            {"textDocument", {
                {"synchronization", {{"didOpen", true}, {"didChange", true}}},
                {"diagnostic", {
                    {"dynamicRegistration", true},
                    {"relatedDocumentSupport", true},
                }},
                {"publishDiagnostics", {{"versionSupport", true}}},
            }},
        };
        const std::string root_uri = path_to_file_uri(options.root);
        nlohmann::json params = {
            {"processId", current_process_id()},
            {"rootUri", root_uri},
            {"workspaceFolders", nlohmann::json::array({
                nlohmann::json{{"name", "workspace"}, {"uri", root_uri}},
            })},
            {"capabilities", capabilities},
            {"clientInfo", {{"name", "acecode"}}},
        };
        if (options.initialization.is_object()) {
            params["initializationOptions"] = options.initialization;
        }

        auto result = client->request("initialize", params, options.initialize_timeout);
        if (!result.has_value()) {
            handshake_error = "initialize handshake failed or timed out";
        } else {
            const nlohmann::json caps = result->value("capabilities", nlohmann::json::object());
            {
                std::lock_guard<std::mutex> lk(client->state_mu_);
                if (caps.contains("textDocumentSync")) {
                    const auto& sync = caps["textDocumentSync"];
                    if (sync.is_number_integer()) {
                        client->sync_kind_ = sync.get<int>();
                    } else if (sync.is_object() && sync.contains("change") &&
                               sync["change"].is_number_integer()) {
                        client->sync_kind_ = sync["change"].get<int>();
                    }
                }
                client->static_pull_provider_ =
                    caps.contains("diagnosticProvider") && !caps["diagnosticProvider"].is_null();
            }
            client->notify("initialized", nlohmann::json::object());
            if (options.initialization.is_object() && !options.initialization.empty()) {
                client->notify("workspace/didChangeConfiguration",
                               {{"settings", options.initialization}});
            }
        }
    }

    if (!handshake_error.empty()) {
        if (error) *error = handshake_error;
        // 没握上手的 server 不值得走 LSP shutdown 协议(它可能根本不响应,
        // 白等两个超时窗);置 running_=false 让 shutdown 跳过优雅段直接强杀。
        client->running_.store(false);
        client->shutdown();
        return nullptr;
    }
    return client;
}

LspClient::~LspClient() {
    shutdown();
}

int LspClient::open_file_count() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return static_cast<int>(files_.size());
}

std::optional<std::int64_t> LspClient::touch_file(const std::string& utf8_path) {
    if (!running_.load()) return std::nullopt;
    std::lock_guard<std::mutex> touch_lk(touch_mu_);

    auto text = read_file_utf8(utf8_path);
    if (!text.has_value()) return std::nullopt;

    const std::string key = normalize_path_key(utf8_path);
    const std::string uri = path_to_file_uri(utf8_path);

    std::optional<OpenFile> existing;
    int sync_kind = 1;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        auto it = files_.find(key);
        if (it != files_.end()) existing = it->second;
        sync_kind = sync_kind_;
    }

    if (existing.has_value()) {
        // 不清诊断缓存:部分 server(clangd)内容未变时不重发诊断,清了会
        // 让 no-op touch 丢失既有错误;由 server 的下一次 push/pull 自然覆盖。
        notify("workspace/didChangeWatchedFiles",
               {{"changes", nlohmann::json::array({
                     nlohmann::json{{"uri", uri}, {"type", 2}}, // Changed
                 })}});
        const std::int64_t next_version = existing->version + 1;
        nlohmann::json content_changes;
        if (sync_kind == 2) { // Incremental:用覆盖全文的 range 替换表达全量
            content_changes = nlohmann::json::array({
                nlohmann::json{
                    {"range", {
                        {"start", {{"line", 0}, {"character", 0}}},
                        {"end", end_position_of(existing->text)},
                    }},
                    {"text", *text},
                },
            });
        } else {
            content_changes = nlohmann::json::array({
                nlohmann::json{{"text", *text}},
            });
        }
        notify("textDocument/didChange",
               {{"textDocument", {{"uri", uri}, {"version", next_version}}},
                {"contentChanges", content_changes}});
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            files_[key] = OpenFile{next_version, std::move(*text)};
        }
        return next_version;
    }

    notify("workspace/didChangeWatchedFiles",
           {{"changes", nlohmann::json::array({
                 nlohmann::json{{"uri", uri}, {"type", 1}}, // Created
             })}});
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        push_diags_.erase(key);
        pull_diags_.erase(key);
    }
    notify("textDocument/didOpen",
           {{"textDocument", {
                 {"uri", uri},
                 {"languageId", language_id_for(utf8_path)},
                 {"version", 0},
                 {"text", *text},
             }}});
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        files_[key] = OpenFile{0, std::move(*text)};
    }
    return 0;
}

std::vector<nlohmann::json> LspClient::diagnostics_for(const std::string& utf8_path) const {
    const std::string key = normalize_path_key(utf8_path);
    std::vector<nlohmann::json> merged;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        auto push_it = push_diags_.find(key);
        if (push_it != push_diags_.end()) {
            merged.insert(merged.end(), push_it->second.begin(), push_it->second.end());
        }
        auto pull_it = pull_diags_.find(key);
        if (pull_it != pull_diags_.end()) {
            merged.insert(merged.end(), pull_it->second.begin(), pull_it->second.end());
        }
    }
    return dedupe_diagnostics(std::move(merged));
}

std::map<std::string, std::vector<nlohmann::json>> LspClient::all_diagnostics() const {
    std::set<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        for (const auto& [key, _] : push_diags_) keys.insert(key);
        for (const auto& [key, _] : pull_diags_) keys.insert(key);
    }
    std::map<std::string, std::vector<nlohmann::json>> out;
    for (const auto& key : keys) out[key] = diagnostics_for(key);
    return out;
}

bool LspClient::pull_supported() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return static_pull_provider_ || dynamic_pull_registered_;
}

bool LspClient::pull_diagnostics_once(const std::string& utf8_path) {
    const std::string key = normalize_path_key(utf8_path);
    const std::string uri = path_to_file_uri(utf8_path);

    std::vector<std::string> identifiers;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        identifiers.assign(pull_identifiers_.begin(), pull_identifiers_.end());
    }
    // 无 identifier 的裸请求 + 每个注册 identifier 各一发,结果合并。
    std::vector<nlohmann::json> requests;
    {
        nlohmann::json base = {{"textDocument", {{"uri", uri}}}};
        requests.push_back(base);
        for (const auto& identifier : identifiers) {
            nlohmann::json with_id = base;
            with_id["identifier"] = identifier;
            requests.push_back(std::move(with_id));
        }
    }

    bool matched = false;
    for (const auto& params : requests) {
        auto report = request("textDocument/diagnostic", params, kPullRequestTimeout);
        if (!report.has_value() || !report->is_object()) continue;

        auto store = [&](const std::string& target_key, const nlohmann::json& items) {
            if (!items.is_array()) return;
            std::lock_guard<std::mutex> lk(state_mu_);
            auto& bucket = pull_diags_[target_key];
            for (const auto& item : items) bucket.push_back(item);
            bucket = dedupe_diagnostics(std::move(bucket));
        };

        if (report->contains("items")) {
            store(key, (*report)["items"]);
            matched = true;
        }
        if (report->contains("relatedDocuments") && (*report)["relatedDocuments"].is_object()) {
            for (const auto& [rel_uri, rel_report] : (*report)["relatedDocuments"].items()) {
                auto rel_path = file_uri_to_path(rel_uri);
                if (!rel_path.has_value() || !rel_report.is_object()) continue;
                const std::string rel_key = normalize_path_key(*rel_path);
                if (rel_report.contains("items")) {
                    store(rel_key, rel_report["items"]);
                    if (rel_key == key) matched = true;
                }
            }
        }
    }
    if (matched) diag_cv_.notify_all();
    return matched;
}

void LspClient::wait_for_diagnostics(const std::string& utf8_path,
                                     std::int64_t version,
                                     std::chrono::milliseconds timeout,
                                     const AbortProbe& should_abort) {
    const std::string key = normalize_path_key(utf8_path);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + timeout;
    const auto grace_start = start - kFreshnessGrace;
    bool pulled = false;

    while (running_.load()) {
        if (should_abort && should_abort()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;

        std::optional<PublishStamp> stamp;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            auto it = published_.find(key);
            if (it != published_.end()) stamp = it->second;
        }
        if (stamp.has_value()) {
            const bool fresh = stamp->version.has_value()
                ? *stamp->version >= version
                : stamp->at >= grace_start;
            if (fresh) {
                // debounce:等 push 静默满 150ms 再返回,吸收 server 的
                // 「先空后满」两段式推送。
                const auto quiet = now - stamp->at;
                if (quiet >= kPushDebounce) return;
                std::unique_lock<std::mutex> lk(state_mu_);
                diag_cv_.wait_for(lk, std::min<std::chrono::milliseconds>(
                    kAbortPollSlice,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        kPushDebounce - quiet)));
                continue;
            }
        }

        if (!pulled && pull_supported()) {
            pulled = true;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds(0)) return;
            if (pull_diagnostics_once(utf8_path)) return;
            continue;
        }

        std::unique_lock<std::mutex> lk(state_mu_);
        diag_cv_.wait_for(lk, kAbortPollSlice);
    }
}

std::optional<nlohmann::json> LspClient::request(const std::string& method,
                                                 const nlohmann::json& params,
                                                 std::chrono::milliseconds timeout) {
    if (!running_.load()) return std::nullopt;

    const std::int64_t id = next_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        responses_[id] = PendingResponse{};
    }

    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params.is_null() ? nlohmann::json::object() : params},
    };
    std::string write_error;
    if (!write_frame(message, &write_error)) {
        std::lock_guard<std::mutex> lk(state_mu_);
        responses_.erase(id);
        LOG_WARN("[lsp] " + server_id_ + " write failed for " + method + ": " + write_error);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lk(state_mu_);
    const bool ready = responses_cv_.wait_for(lk, timeout, [&] {
        auto it = responses_.find(id);
        return it == responses_.end() || it->second.done;
    });
    if (!ready) {
        responses_.erase(id);
        return std::nullopt;
    }
    auto it = responses_.find(id);
    if (it == responses_.end()) return std::nullopt;
    PendingResponse pending = std::move(it->second);
    responses_.erase(it);
    lk.unlock();

    if (!pending.error.is_null() && !pending.error.empty()) {
        LOG_DEBUG("[lsp] " + server_id_ + " " + method + " error: " + pending.error.dump());
        return std::nullopt;
    }
    return pending.result;
}

bool LspClient::notify(const std::string& method, const nlohmann::json& params) {
    if (!running_.load()) return false;
    nlohmann::json message = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params.is_null() ? nlohmann::json::object() : params},
    };
    std::string write_error;
    if (!write_frame(message, &write_error)) {
        LOG_WARN("[lsp] " + server_id_ + " notify " + method + " failed: " + write_error);
        return false;
    }
    return true;
}

void LspClient::shutdown() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (shutdown_done_) return;
        shutdown_done_ = true;
    }

    if (running_.load()) {
        // 尽力而为的协议级退出;server 不配合时下方 terminate 兜底。
        request("shutdown", nlohmann::json(), std::chrono::milliseconds(1500));
        notify("exit", nlohmann::json::object());
        process_.close_stdin();
        if (!process_.wait_exit(1500)) {
            process_.terminate();
        }
    }
    running_.store(false);
    process_.terminate(); // 幂等:关句柄让 reader 的阻塞 read 解除
    if (reader_.joinable()) reader_.join();
    fail_all_pending("LSP client shut down");
}

bool LspClient::write_frame(const nlohmann::json& message, std::string* error) {
    const std::string frame = encode_frame(message);
    std::lock_guard<std::mutex> lk(write_mu_);
    return process_.write_stdin(frame.data(), frame.size(), error);
}

void LspClient::fail_all_pending(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        for (auto& [id, pending] : responses_) {
            if (!pending.done) {
                pending.done = true;
                pending.error = {{"code", -32000}, {"message", reason}};
            }
        }
    }
    responses_cv_.notify_all();
    diag_cv_.notify_all();
}

void LspClient::read_loop() {
    LspFrameParser parser;
    char chunk[8192];
    while (running_.load()) {
        const long n = process_.read_stdout(chunk, sizeof(chunk));
        if (n <= 0) break;
        const bool ok = parser.feed(chunk, static_cast<std::size_t>(n),
                                    [this](nlohmann::json&& message) {
                                        handle_message(std::move(message));
                                    });
        if (!ok) {
            LOG_WARN("[lsp] " + server_id_ + " stream framing broken; disconnecting");
            break;
        }
    }
    running_.store(false);
    fail_all_pending("LSP server connection closed");
}

void LspClient::handle_message(nlohmann::json&& message) {
    if (!message.is_object()) return;

    // server → client 请求(有 id + method,无 result/error)
    if (message.contains("id") && message.contains("method") &&
        !message.contains("result") && !message.contains("error")) {
        handle_server_request(message);
        return;
    }

    // 应答
    if (message.contains("id") &&
        (message.contains("result") || message.contains("error"))) {
        if (!message["id"].is_number_integer()) return;
        const std::int64_t id = message["id"].get<std::int64_t>();
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            auto it = responses_.find(id);
            if (it != responses_.end()) {
                it->second.done = true;
                if (message.contains("result")) it->second.result = message["result"];
                if (message.contains("error")) it->second.error = message["error"];
            }
        }
        responses_cv_.notify_all();
        return;
    }

    // 通知
    if (!message.contains("method") || !message["method"].is_string()) return;
    const std::string method = message["method"].get<std::string>();
    const nlohmann::json params = message.value("params", nlohmann::json::object());

    if (method == "textDocument/publishDiagnostics") {
        if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string()) return;
        auto path = file_uri_to_path(params["uri"].get<std::string>());
        if (!path.has_value()) return;
        const std::string key = normalize_path_key(*path);
        PublishStamp stamp;
        stamp.at = std::chrono::steady_clock::now();
        if (params.contains("version") && params["version"].is_number_integer()) {
            stamp.version = params["version"].get<std::int64_t>();
        }
        std::vector<nlohmann::json> items;
        if (params.contains("diagnostics") && params["diagnostics"].is_array()) {
            for (const auto& item : params["diagnostics"]) items.push_back(item);
        }
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            published_[key] = stamp;
            push_diags_[key] = std::move(items);
        }
        diag_cv_.notify_all();
        return;
    }
    // 其余通知(logMessage / progress / ...)静默忽略。
}

void LspClient::handle_server_request(const nlohmann::json& message) {
    const std::string method =
        message.contains("method") && message["method"].is_string()
            ? message["method"].get<std::string>()
            : std::string{};
    const nlohmann::json params = message.value("params", nlohmann::json::object());

    nlohmann::json response = {
        {"jsonrpc", "2.0"},
        {"id", message.value("id", nlohmann::json())},
    };

    if (method == "workspace/configuration") {
        nlohmann::json result = nlohmann::json::array();
        if (params.is_object() && params.contains("items") && params["items"].is_array()) {
            for (const auto& item : params["items"]) {
                const std::string section =
                    item.is_object() && item.contains("section") && item["section"].is_string()
                        ? item["section"].get<std::string>()
                        : std::string{};
                result.push_back(configuration_value(initialization_, section));
            }
        }
        response["result"] = result;
    } else if (method == "window/workDoneProgress/create" ||
               method == "workspace/diagnostic/refresh") {
        response["result"] = nullptr;
    } else if (method == "client/registerCapability") {
        bool changed = false;
        if (params.is_object() && params.contains("registrations") &&
            params["registrations"].is_array()) {
            std::lock_guard<std::mutex> lk(state_mu_);
            for (const auto& reg : params["registrations"]) {
                if (!reg.is_object()) continue;
                if (reg.value("method", std::string{}) != "textDocument/diagnostic") continue;
                dynamic_pull_registered_ = true;
                changed = true;
                if (reg.contains("registerOptions") && reg["registerOptions"].is_object()) {
                    const auto& opts = reg["registerOptions"];
                    if (opts.contains("identifier") && opts["identifier"].is_string()) {
                        pull_identifiers_.insert(opts["identifier"].get<std::string>());
                    }
                }
            }
        }
        if (changed) diag_cv_.notify_all();
        response["result"] = nullptr;
    } else if (method == "client/unregisterCapability") {
        response["result"] = nullptr;
    } else if (method == "workspace/workspaceFolders") {
        response["result"] = nlohmann::json::array({
            nlohmann::json{{"name", "workspace"}, {"uri", path_to_file_uri(root_)}},
        });
    } else {
        response["error"] = {{"code", -32601},
                             {"message", "method not supported by acecode LSP client"}};
    }

    std::string ignored;
    write_frame(response, &ignored);
}

} // namespace acecode::lsp
