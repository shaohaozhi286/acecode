#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

constexpr const char* kCliVersion = "0.1.0";
constexpr const char* kDaemonVersion = "0.1.0";
constexpr const char* kProtocolVersion = "0.1";
constexpr const char* kHost = "127.0.0.1";
constexpr int kDefaultPort = 52007;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

void configure_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

class SocketRuntime {
  public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        ok_ = true;
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        if (ok_) WSACleanup();
#endif
    }

    bool ok() const { return ok_; }

  private:
    bool ok_ = false;
};

json success(json data = json::object()) {
    return json{{"ok", true}, {"data", std::move(data)}};
}

json failure(std::string code, std::string message) {
    return json{{"ok", false}, {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}

void print_json(const json& value) {
    std::cout << value.dump() << std::endl;
}

std::string read_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

std::optional<std::string> find_arg(int argc, char** argv, const std::string& name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) return std::string(argv[i + 1]);
    }
    return std::nullopt;
}

bool has_arg(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == name) return true;
    }
    return false;
}

int parse_port(int argc, char** argv) {
    auto value = find_arg(argc, argv, "--port");
    if (!value) return kDefaultPort;
    try {
        int port = std::stoi(*value);
        if (port < 1 || port > 65535) return kDefaultPort;
        return port;
    } catch (...) {
        return kDefaultPort;
    }
}

json default_capabilities(bool extension_connected) {
    return json{
        {"cdp", extension_connected},
        {"network", extension_connected},
        {"pdf", extension_connected},
        {"upload", extension_connected},
        {"os_pointer", false},
        {"operation_overlay", extension_connected},
    };
}

json stopped_status(int port) {
    return json{
        {"running", false},
        {"extension_connected", false},
        {"version", nullptr},
        {"extension_version", nullptr},
        {"protocol_version", nullptr},
        {"cli_protocol_version", kProtocolVersion},
        {"version_compatible", true},
        {"cli_version", kCliVersion},
        {"port", port},
        {"capabilities", default_capabilities(false)},
    };
}

std::filesystem::path normalize_bridge_path(std::string input) {
    std::replace(input.begin(), input.end(), '\\', '/');

    std::filesystem::path p;
    if (input.rfind("/tmp/", 0) == 0) {
        p = std::filesystem::temp_directory_path() / input.substr(5);
    } else if (input == "/tmp") {
        p = std::filesystem::temp_directory_path();
    } else {
        p = std::filesystem::path(input);
    }

    if (p.is_relative()) {
        p = std::filesystem::absolute(p);
    }

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return canonical;
    return p.lexically_normal();
}

void normalize_path_fields(json& value) {
    if (value.is_object()) {
        for (auto& item : value.items()) {
            if ((item.key() == "path" || item.key() == "file_path") && item.value().is_string()) {
                item.value() = normalize_bridge_path(item.value().get<std::string>()).u8string();
            } else {
                normalize_path_fields(item.value());
            }
        }
        return;
    }

    if (value.is_array()) {
        for (auto& item : value) normalize_path_fields(item);
    }
}

std::optional<std::vector<unsigned char>> decode_base64(std::string input) {
    auto comma = input.find(',');
    if (comma != std::string::npos && input.substr(0, comma).find("base64") != std::string::npos) {
        input = input.substr(comma + 1);
    }

    static const int table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<unsigned char> out;
    int val = 0;
    int valb = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch)) continue;
        int decoded = table[ch];
        if (decoded == -2) break;
        if (decoded < 0) return std::nullopt;
        val = (val << 6) + decoded;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::optional<json> materialize_binary_payloads(json& value) {
    if (value.is_object()) {
        if (value.contains("path") && value["path"].is_string() &&
            value.contains("data") && value["data"].is_string()) {
            auto bytes = decode_base64(value["data"].get<std::string>());
            if (!bytes) {
                return failure("invalid_cli_response", "bridge returned invalid base64 data");
            }
            std::filesystem::path path = normalize_bridge_path(value["path"].get<std::string>());
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) {
                return failure("file_write_failed", "failed to write bridge output file: " + path.u8string());
            }
            out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
            out.close();
            if (!out) {
                return failure("file_write_failed", "failed to write bridge output file: " + path.u8string());
            }
            value["path"] = path.u8string();
            value["sizeBytes"] = bytes->size();
            value.erase("data");
        }
        for (auto& item : value.items()) {
            if (auto error = materialize_binary_payloads(item.value())) return error;
        }
    } else if (value.is_array()) {
        for (auto& item : value) {
            if (auto error = materialize_binary_payloads(item)) return error;
        }
    }
    return std::nullopt;
}

bool send_all(socket_t s, const std::string& data) {
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
#ifdef _WIN32
        int sent = send(s, ptr, static_cast<int>(remaining), 0);
#else
        ssize_t sent = send(s, ptr, remaining, 0);
#endif
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

struct HttpResponse {
    bool transport_ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

socket_t connect_loopback(int port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) return kInvalidSocket;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, kHost, &addr.sin_addr) != 1) {
        close_socket(s);
        return kInvalidSocket;
    }

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return kInvalidSocket;
    }
    return s;
}

HttpResponse http_request(const std::string& method, const std::string& path, const std::string& body, int port) {
    socket_t s = connect_loopback(port);
    if (s == kInvalidSocket) {
        HttpResponse response;
        response.transport_ok = false;
        response.error = "connect_failed";
        return response;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << kHost << ":" << port << "\r\n";
    req << "Connection: close\r\n";
    req << "Accept: application/json\r\n";
    req << "X-Ace-Browser-Cli: 1\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json; charset=utf-8\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    req << body;

    if (!send_all(s, req.str())) {
        close_socket(s);
        HttpResponse response;
        response.transport_ok = false;
        response.error = "send_failed";
        return response;
    }

    std::string raw;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
        int n = recv(s, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        ssize_t n = recv(s, buffer, sizeof(buffer), 0);
#endif
        if (n <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(n));
    }
    close_socket(s);

    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        HttpResponse response;
        response.transport_ok = false;
        response.error = "invalid_http_response";
        return response;
    }

    std::istringstream status_line(raw.substr(0, raw.find("\r\n")));
    std::string http_version;
    int status = 0;
    status_line >> http_version >> status;
    HttpResponse response;
    response.transport_ok = true;
    response.status = status;
    response.body = raw.substr(header_end + 4);
    return response;
}

std::optional<json> parse_json(const std::string& text) {
    auto parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

json normalize_daemon_envelope(const std::string& body) {
    auto parsed = parse_json(body);
    if (!parsed || !parsed->is_object() || !parsed->contains("ok") || !(*parsed)["ok"].is_boolean()) {
        return failure("invalid_cli_response", "daemon returned an invalid JSON envelope");
    }
    if ((*parsed)["ok"].get<bool>()) {
        if (!parsed->contains("data")) (*parsed)["data"] = json::object();
        normalize_path_fields((*parsed)["data"]);
        if (auto error = materialize_binary_payloads((*parsed)["data"])) {
            return *error;
        }
        normalize_path_fields((*parsed)["data"]);
    } else if (!parsed->contains("error") || !(*parsed)["error"].is_object()) {
        return failure("invalid_cli_response", "daemon returned an invalid error envelope");
    }
    return *parsed;
}

int status_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    HttpResponse response = http_request("GET", "/status", "", port);
    if (!response.transport_ok) {
        print_json(success(stopped_status(port)));
        return 0;
    }
    if (response.status != 200) {
        print_json(failure("daemon_error", "daemon status endpoint returned HTTP " + std::to_string(response.status)));
        return 0;
    }
    json envelope = normalize_daemon_envelope(response.body);
    if (envelope.value("ok", false) && envelope["data"].is_object()) {
        envelope["data"]["cli_version"] = kCliVersion;
        envelope["data"]["port"] = port;
    }
    print_json(envelope);
    return 0;
}

json command_envelope_from_stdin(int port, std::string input) {
    auto parsed = parse_json(input);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "command --json expects a JSON object on stdin");
    }
    if (!parsed->contains("session") || !(*parsed)["session"].is_string()) {
        return failure("invalid_request", "command request requires string field: session");
    }
    if (!parsed->contains("action") || !(*parsed)["action"].is_string()) {
        return failure("invalid_request", "command request requires string field: action");
    }
    if (!parsed->contains("args")) {
        (*parsed)["args"] = json::object();
    }

    HttpResponse response = http_request("POST", "/command", parsed->dump(), port);
    if (!response.transport_ok) {
        return failure("daemon_not_running", "ace-browser-cli daemon is not running on 127.0.0.1:" + std::to_string(port));
    }
    if (response.status != 200) {
        return failure("daemon_error", "daemon command endpoint returned HTTP " + std::to_string(response.status));
    }
    return normalize_daemon_envelope(response.body);
}

int command_command(int argc, char** argv) {
    print_json(command_envelope_from_stdin(parse_port(argc, argv), read_stdin()));
    return 0;
}

int screenshot_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    auto session = find_arg(argc, argv, "--session").value_or("acecode-default");
    auto output = find_arg(argc, argv, "--output");
    if (!output) {
        print_json(failure("invalid_request", "screenshot requires --output <path>"));
        return 0;
    }
    json request = {
        {"session", session},
        {"action", "screenshot"},
        {"args", {{"output", normalize_bridge_path(*output).u8string()}}},
    };
    print_json(command_envelope_from_stdin(port, request.dump()));
    return 0;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::optional<HttpRequest> read_http_request(socket_t client) {
    std::string raw;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
#ifdef _WIN32
        int n = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        ssize_t n = recv(client, buffer, sizeof(buffer), 0);
#endif
        if (n <= 0) return std::nullopt;
        raw.append(buffer, static_cast<std::size_t>(n));
        header_end = raw.find("\r\n\r\n");
        if (raw.size() > 1024 * 1024) return std::nullopt;
    }

    std::string headers_text = raw.substr(0, header_end);
    std::string body = raw.substr(header_end + 4);
    std::istringstream hs(headers_text);
    std::string first;
    std::getline(hs, first);
    if (!first.empty() && first.back() == '\r') first.pop_back();
    std::istringstream fs(first);

    HttpRequest req;
    fs >> req.method >> req.path;
    if (req.method.empty() || req.path.empty()) return std::nullopt;

    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower_copy(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        req.headers[key] = value;
    }

    std::size_t content_length = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        try {
            content_length = static_cast<std::size_t>(std::stoul(it->second));
        } catch (...) {
            return std::nullopt;
        }
    }

    while (body.size() < content_length) {
#ifdef _WIN32
        int n = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        ssize_t n = recv(client, buffer, sizeof(buffer), 0);
#endif
        if (n <= 0) return std::nullopt;
        body.append(buffer, static_cast<std::size_t>(n));
    }
    if (body.size() > content_length) body.resize(content_length);
    req.body = std::move(body);
    return req;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string extension_cors_origin(const HttpRequest& req) {
    auto it = req.headers.find("origin");
    if (it == req.headers.end()) return {};
    const std::string& origin = it->second;
    return starts_with(origin, "chrome-extension://") ? origin : std::string{};
}

void append_extension_cors_headers(std::ostringstream& resp, const std::string& origin) {
    if (origin.empty()) return;
    resp << "Access-Control-Allow-Origin: " << origin << "\r\n";
    resp << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    resp << "Access-Control-Allow-Headers: Content-Type, X-Ace-Browser-Bridge\r\n";
    resp << "Access-Control-Max-Age: 600\r\n";
    resp << "Vary: Origin\r\n";
}

std::string http_json_response(int status, const json& body, const std::string& cors_origin = {}) {
    std::string payload = body.dump();
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " OK\r\n";
    resp << "Content-Type: application/json; charset=utf-8\r\n";
    append_extension_cors_headers(resp, cors_origin);
    resp << "Content-Length: " << payload.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << payload;
    return resp.str();
}

std::string http_empty_response(int status, const std::string& cors_origin = {}) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " OK\r\n";
    append_extension_cors_headers(resp, cors_origin);
    resp << "Content-Length: 0\r\n";
    resp << "Connection: close\r\n\r\n";
    return resp.str();
}

bool header_equals(const HttpRequest& req, const std::string& key, const std::string& value) {
    auto it = req.headers.find(lower_copy(key));
    return it != req.headers.end() && it->second == value;
}

struct DaemonState {
    struct PendingAction {
        std::string id;
        json request;
        std::optional<json> result;
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool extension_connected = false;
    std::string extension_version;
    std::string protocol_version;
    bool version_compatible = true;
    std::string version_error;
    std::string browser;
    json capabilities = default_capabilities(false);
    std::chrono::steady_clock::time_point last_hello{};
    uint64_t next_action_id = 1;
    std::deque<std::shared_ptr<PendingAction>> queued_actions;
    std::unordered_map<std::string, std::shared_ptr<PendingAction>> pending_actions;
};

json daemon_status_payload(const DaemonState& state, int port) {
    return json{
        {"running", true},
        {"extension_connected", state.extension_connected},
        {"version", kDaemonVersion},
        {"extension_version", state.extension_connected ? json(state.extension_version) : json(nullptr)},
        {"protocol_version", state.extension_connected ? json(state.protocol_version) : json(nullptr)},
        {"cli_protocol_version", kProtocolVersion},
        {"version_compatible", state.version_compatible},
        {"version_error", state.version_error.empty() ? json(nullptr) : json(state.version_error)},
        {"cli_version", kCliVersion},
        {"port", port},
        {"browser", state.browser.empty() ? json(nullptr) : json(state.browser)},
        {"capabilities", state.extension_connected ? state.capabilities : default_capabilities(false)},
    };
}

bool compatible_protocol_version(const std::string& version) {
    return version == kProtocolVersion;
}

json handle_plugin_hello(DaemonState& state, const std::string& body) {
    auto parsed = parse_json(body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "plugin hello expects a JSON object");
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    state.extension_connected = true;
    state.extension_version = parsed->value("extension_version", "unknown");
    state.protocol_version = parsed->value("protocol_version", "");
    if (!compatible_protocol_version(state.protocol_version)) {
        state.version_compatible = false;
        state.version_error = "ace-browser-bridge protocol version " +
            (state.protocol_version.empty() ? std::string("<missing>") : state.protocol_version) +
            " is not compatible with ace-browser-cli protocol " + kProtocolVersion;
    } else {
        state.version_compatible = true;
        state.version_error.clear();
    }
    state.browser = parsed->value("browser", "");
    if (parsed->contains("capabilities") && (*parsed)["capabilities"].is_object()) {
        state.capabilities = (*parsed)["capabilities"];
    } else {
        state.capabilities = default_capabilities(true);
    }
    state.last_hello = std::chrono::steady_clock::now();
    state.cv.notify_all();
    return success({{"success", true}});
}

json handle_command(DaemonState& state, const std::string& body) {
    auto parsed = parse_json(body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "command endpoint expects a JSON object");
    }
    if (!parsed->contains("session") || !(*parsed)["session"].is_string()) {
        return failure("invalid_request", "command request requires string field: session");
    }
    if (!parsed->contains("action") || !(*parsed)["action"].is_string()) {
        return failure("invalid_request", "command request requires string field: action");
    }
    if (!parsed->contains("args")) {
        (*parsed)["args"] = json::object();
    }

    std::shared_ptr<DaemonState::PendingAction> pending;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.extension_connected) {
            return failure("extension_not_connected", "ace-browser-bridge browser plugin is not connected");
        }
        if (!state.version_compatible) {
            return failure("version_mismatch", state.version_error.empty()
                ? "ace-browser-cli and ace-browser-bridge protocol versions are not compatible"
                : state.version_error);
        }
        pending = std::make_shared<DaemonState::PendingAction>();
        pending->id = "act_" + std::to_string(state.next_action_id++);
        pending->request = *parsed;
        pending->request["id"] = pending->id;
        state.queued_actions.push_back(pending);
        state.pending_actions[pending->id] = pending;
    }
    state.cv.notify_all();

    std::unique_lock<std::mutex> lock(state.mutex);
    bool ready = state.cv.wait_for(lock, std::chrono::seconds(30), [&]() {
        return pending->result.has_value();
    });
    state.pending_actions.erase(pending->id);
    if (!ready || !pending->result) {
        return failure("bridge_timeout", "timed out waiting for browser plugin action result");
    }
    json result = *pending->result;
    normalize_path_fields(result);
    return result;
}

json handle_plugin_poll(DaemonState& state, const std::string& body) {
    auto parsed = parse_json(body.empty() ? "{}" : body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "plugin poll expects a JSON object");
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.extension_connected = true;
        state.last_hello = std::chrono::steady_clock::now();
    }

    std::unique_lock<std::mutex> lock(state.mutex);
    state.cv.wait_for(lock, std::chrono::seconds(20), [&]() {
        return !state.queued_actions.empty();
    });
    if (!state.extension_connected) {
        return failure("extension_not_connected", "ace-browser-bridge browser plugin is not connected");
    }
    if (state.queued_actions.empty()) {
        return success({{"action", nullptr}});
    }
    auto pending = state.queued_actions.front();
    state.queued_actions.pop_front();
    return success({{"action", pending->request}});
}

json handle_plugin_result(DaemonState& state, const std::string& body) {
    auto parsed = parse_json(body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "plugin result expects a JSON object");
    }
    std::string id = parsed->value("id", "");
    if (id.empty() || !parsed->contains("result") || !(*parsed)["result"].is_object()) {
        return failure("invalid_request", "plugin result requires id and result object");
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.pending_actions.find(id);
    if (it == state.pending_actions.end()) {
        return failure("unknown_action", "plugin returned result for unknown action id");
    }
    it->second->result = (*parsed)["result"];
    state.cv.notify_all();
    return success({{"success", true}});
}

std::string route_request(const HttpRequest& req, DaemonState& state, int port, std::atomic<bool>& running) {
    const std::string cors_origin = extension_cors_origin(req);
    if (req.method == "OPTIONS") {
        return http_empty_response(204, cors_origin);
    }
    const bool cli_request = header_equals(req, "x-ace-browser-cli", "1");
    const bool plugin_request = header_equals(req, "x-ace-browser-bridge", "extension");
    if (req.method == "GET" && req.path == "/status") {
        if (!cli_request) return http_json_response(403, failure("unauthorized", "status requires ace-browser-cli"));
        return http_json_response(200, success(daemon_status_payload(state, port)));
    }
    if (req.method == "POST" && req.path == "/plugin/hello") {
        if (!plugin_request) return http_json_response(403, failure("unauthorized", "plugin hello requires ace-browser-bridge extension"), cors_origin);
        return http_json_response(200, handle_plugin_hello(state, req.body), cors_origin);
    }
    if (req.method == "POST" && req.path == "/plugin/poll") {
        if (!plugin_request) return http_json_response(403, failure("unauthorized", "plugin poll requires ace-browser-bridge extension"), cors_origin);
        return http_json_response(200, handle_plugin_poll(state, req.body), cors_origin);
    }
    if (req.method == "POST" && req.path == "/plugin/result") {
        if (!plugin_request) return http_json_response(403, failure("unauthorized", "plugin result requires ace-browser-bridge extension"), cors_origin);
        return http_json_response(200, handle_plugin_result(state, req.body), cors_origin);
    }
    if (req.method == "POST" && req.path == "/command") {
        if (!cli_request) return http_json_response(403, failure("unauthorized", "command requires ace-browser-cli"));
        return http_json_response(200, handle_command(state, req.body));
    }
    if (req.method == "POST" && req.path == "/shutdown") {
        if (!cli_request) return http_json_response(403, failure("unauthorized", "shutdown requires ace-browser-cli"));
        running = false;
        return http_json_response(200, success({{"success", true}}));
    }
    return http_json_response(404, failure("not_found", "unknown daemon endpoint"));
}

int serve_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        print_json(failure("socket_error", "failed to create listen socket"));
        return 1;
    }

    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(listener);
        print_json(failure("bind_failed", "failed to bind 127.0.0.1:" + std::to_string(port)));
        return 1;
    }
    if (listen(listener, 16) != 0) {
        close_socket(listener);
        print_json(failure("listen_failed", "failed to listen on 127.0.0.1:" + std::to_string(port)));
        return 1;
    }

    if (has_arg(argc, argv, "--json")) {
        print_json(success({{"running", true}, {"port", port}, {"version", kDaemonVersion}}));
    }

    DaemonState state;
    std::atomic<bool> running{true};
    while (running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int selected = select(static_cast<int>(listener + 1), &read_set, nullptr, nullptr, &timeout);
        if (selected <= 0 || !FD_ISSET(listener, &read_set)) {
            continue;
        }
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) continue;
        std::thread([client, &state, port, &running]() {
            auto req = read_http_request(client);
            std::string response;
            if (!req) {
                response = http_json_response(400, failure("invalid_http_request", "failed to parse HTTP request"));
            } else {
                response = route_request(*req, state, port, running);
            }
            send_all(client, response);
            close_socket(client);
        }).detach();
    }
    state.cv.notify_all();
    close_socket(listener);
    return 0;
}

int shutdown_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    HttpResponse response = http_request("POST", "/shutdown", "{}", port);
    if (!response.transport_ok) {
        print_json(failure("daemon_not_running", "ace-browser-cli daemon is not running on 127.0.0.1:" + std::to_string(port)));
        return 0;
    }
    print_json(normalize_daemon_envelope(response.body));
    return 0;
}

void print_help() {
    std::cout
        << "ace-browser-cli commands:\n"
        << "  status --json [--port 52007]\n"
        << "  command --json [--port 52007]     # reads {session,action,args} from stdin\n"
        << "  screenshot --json --session <name> --output <path> [--port 52007]\n"
        << "  serve --json [--port 52007]\n"
        << "  shutdown --json [--port 52007]\n";
}

}  // namespace

int main(int argc, char** argv) {
    configure_utf8_console();
    SocketRuntime sockets;
    if (!sockets.ok()) {
        print_json(failure("socket_error", "failed to initialize socket runtime"));
        return 1;
    }

    if (argc < 2 || has_arg(argc, argv, "--help") || has_arg(argc, argv, "-h")) {
        print_help();
        return argc < 2 ? 1 : 0;
    }

    std::string command = argv[1];
    if (command == "status") return status_command(argc, argv);
    if (command == "command") return command_command(argc, argv);
    if (command == "screenshot") return screenshot_command(argc, argv);
    if (command == "serve") return serve_command(argc, argv);
    if (command == "shutdown") return shutdown_command(argc, argv);

    print_json(failure("unknown_command", "unknown command: " + command));
    return 1;
}
