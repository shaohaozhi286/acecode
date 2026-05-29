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
#include <string_view>
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
#include <shellapi.h>
#else
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

constexpr const char* kHostVersion = "0.1.0";
constexpr const char* kDaemonVersion = "0.1.0";
constexpr const char* kProtocolVersion = "0.1";
constexpr const char* kHost = "127.0.0.1";
constexpr int kDefaultPort = 52007;
constexpr int kExtensionFreshnessMs = 30000;
constexpr int kEnsureReadyDefaultTimeoutMs = 15000;

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

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7F) {
            ++i;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if (c1 < 0x80 || c1 > 0xBF) return false;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 < 0xA0 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF) return false;
            i += 3;
        } else if (c >= 0xE1 && c <= 0xEC) {
            if (i + 2 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF) return false;
            i += 3;
        } else if (c == 0xED) {
            if (i + 2 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 < 0x80 || c1 > 0x9F || c2 < 0x80 || c2 > 0xBF) return false;
            i += 3;
        } else if (c >= 0xEE && c <= 0xEF) {
            if (i + 2 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF) return false;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            if (c1 < 0x90 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF) return false;
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            if (c1 < 0x80 || c1 > 0xBF || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF) return false;
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= s.size()) return false;
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            if (c1 < 0x80 || c1 > 0x8F || c2 < 0x80 || c2 > 0xBF || c3 < 0x80 || c3 > 0xBF) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

std::string safe_error_message(const char* message) {
    std::string text = message ? message : "unknown error";
    return is_valid_utf8(text) ? text : "non-UTF-8 exception message";
}

struct Utf8Argv {
    std::vector<std::string> args;
    std::vector<char*> pointers;

    void refresh() {
        pointers.clear();
        pointers.reserve(args.size());
        for (auto& arg : args) pointers.push_back(arg.data());
    }

    int argc() const {
        return static_cast<int>(pointers.size());
    }

    char** argv() {
        return pointers.empty() ? nullptr : pointers.data();
    }
};

#ifdef _WIN32
std::optional<std::string> wide_to_utf8(const wchar_t* value) {
    if (!value) return std::string();
    int length = lstrlenW(value);
    if (length == 0) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                     value, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::nullopt;
    std::string out(static_cast<std::size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      value, length, out.data(), needed, nullptr, nullptr);
    if (written != needed) return std::nullopt;
    return out;
}
#endif

std::optional<Utf8Argv> make_utf8_argv(int argc, char** argv, std::string& error) {
    Utf8Argv out;
#ifdef _WIN32
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) {
        error = "failed to read Windows command line";
        return std::nullopt;
    }
    out.args.reserve(static_cast<std::size_t>(wide_argc));
    for (int i = 0; i < wide_argc; ++i) {
        auto converted = wide_to_utf8(wide_argv[i]);
        if (!converted) {
            LocalFree(wide_argv);
            error = "failed to convert Windows command line argument to UTF-8 at index " + std::to_string(i);
            return std::nullopt;
        }
        out.args.push_back(std::move(*converted));
    }
    LocalFree(wide_argv);
#else
    out.args.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    for (int i = 0; i < argc; ++i) {
        std::string value = argv[i] ? std::string(argv[i]) : std::string();
        if (!is_valid_utf8(value)) {
            error = "CLI arguments must be valid UTF-8; invalid argument index " + std::to_string(i);
            return std::nullopt;
        }
        out.args.push_back(std::move(value));
    }
#endif
    out.refresh();
    return out;
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
    const std::string prefix = name + "=";
    for (int i = 1; i < argc; ++i) {
        std::string current = argv[i];
        if (current == name && i + 1 < argc) return std::string(argv[i + 1]);
        if (current.rfind(prefix, 0) == 0) return current.substr(prefix.size());
    }
    return std::nullopt;
}

bool has_arg(int argc, char** argv, const std::string& name) {
    for (int i = 1; i < argc; ++i) {
        std::string current = argv[i];
        if (current == name || current.rfind(name + "=", 0) == 0) return true;
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

std::string arg_or_default(int argc, char** argv, const std::string& name, std::string fallback) {
    auto value = find_arg(argc, argv, name);
    return value ? *value : std::move(fallback);
}

std::optional<int> find_int_arg(int argc, char** argv, const std::string& name) {
    auto value = find_arg(argc, argv, name);
    if (!value) return std::nullopt;
    try {
        return std::stoi(*value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> find_number_arg(int argc, char** argv, const std::string& name) {
    auto value = find_arg(argc, argv, name);
    if (!value) return std::nullopt;
    try {
        return std::stod(*value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> find_bool_arg(int argc, char** argv, const std::string& name) {
    const std::string prefix = name + "=";
    for (int i = 1; i < argc; ++i) {
        std::string current = argv[i];
        if (current == name) return true;
        if (current.rfind(prefix, 0) == 0) {
            std::string value = lower_copy(current.substr(prefix.size()));
            if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
            if (value == "0" || value == "false" || value == "no" || value == "off") return false;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void put_string_arg(json& target, int argc, char** argv,
                    const std::string& cli_name, const std::string& json_name) {
    auto value = find_arg(argc, argv, cli_name);
    if (value) target[json_name] = *value;
}

void put_bool_flag(json& target, int argc, char** argv,
                   const std::string& cli_name, const std::string& json_name) {
    if (auto value = find_bool_arg(argc, argv, cli_name)) target[json_name] = *value;
}

void put_int_arg(json& target, int argc, char** argv,
                 const std::string& cli_name, const std::string& json_name) {
    if (auto value = find_int_arg(argc, argv, cli_name)) target[json_name] = *value;
}

void put_number_arg(json& target, int argc, char** argv,
                    const std::string& cli_name, const std::string& json_name) {
    if (auto value = find_number_arg(argc, argv, cli_name)) target[json_name] = *value;
}

json default_capabilities(bool extension_connected) {
    return json{
        {"cdp", extension_connected},
        {"devtools", extension_connected},
        {"raw_cdp", extension_connected},
        {"console", extension_connected},
        {"network", extension_connected},
        {"emulation", extension_connected},
        {"performance", extension_connected},
        {"heap_snapshot", extension_connected},
        {"pdf", extension_connected},
        {"upload", extension_connected},
        {"os_pointer", false},
        {"operation_overlay", extension_connected},
        {"input_block", extension_connected},
    };
}

json stopped_status(int port) {
    return json{
        {"running", false},
        {"ready", false},
        {"extension_connected", false},
        {"extension_stale", false},
        {"extension_last_seen_ms", nullptr},
        {"version", nullptr},
        {"extension_version", nullptr},
        {"protocol_version", nullptr},
        {"host_protocol_version", kProtocolVersion},
        {"version_compatible", true},
        {"host_version", kHostVersion},
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
                return failure("invalid_host_response", "bridge returned invalid base64 data");
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

std::optional<json> write_bytes_file(const std::filesystem::path& path,
                                     const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return failure("file_write_failed", "failed to write bridge output file: " + path.u8string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        return failure("file_write_failed", "failed to write bridge output file: " + path.u8string());
    }
    return std::nullopt;
}

std::optional<json> write_text_file(const std::filesystem::path& path,
                                    const std::string& text) {
    std::vector<unsigned char> bytes(text.begin(), text.end());
    return write_bytes_file(path, bytes);
}

std::vector<std::string> split_csv(const std::string& input) {
    std::vector<std::string> out;
    std::string current;
    std::istringstream ss(input);
    while (std::getline(ss, current, ',')) {
        auto begin = current.find_first_not_of(" \t\r\n");
        auto end = current.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        out.push_back(current.substr(begin, end - begin + 1));
    }
    return out;
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
    req << "X-Ace-Browser-Host: 1\r\n";
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

std::optional<json> parse_args_json(int argc, char** argv) {
    auto value = find_arg(argc, argv, "--args-json");
    if (!value) return std::nullopt;
    auto parsed = parse_json(*value);
    if (!parsed || !parsed->is_object()) return json();
    return *parsed;
}

json normalize_daemon_envelope(const std::string& body) {
    auto parsed = parse_json(body);
    if (!parsed || !parsed->is_object() || !parsed->contains("ok") || !(*parsed)["ok"].is_boolean()) {
        return failure("invalid_host_response", "daemon returned an invalid JSON envelope");
    }
    if ((*parsed)["ok"].get<bool>()) {
        if (!parsed->contains("data")) (*parsed)["data"] = json::object();
        normalize_path_fields((*parsed)["data"]);
        if (auto error = materialize_binary_payloads((*parsed)["data"])) {
            return *error;
        }
        normalize_path_fields((*parsed)["data"]);
    } else if (!parsed->contains("error") || !(*parsed)["error"].is_object()) {
        return failure("invalid_host_response", "daemon returned an invalid error envelope");
    }
    return *parsed;
}

json status_envelope(int port) {
    HttpResponse response = http_request("GET", "/status", "", port);
    if (!response.transport_ok) {
        return success(stopped_status(port));
    }
    if (response.status != 200) {
        return failure("daemon_error", "daemon status endpoint returned HTTP " + std::to_string(response.status));
    }
    json envelope = normalize_daemon_envelope(response.body);
    if (envelope.value("ok", false) && envelope["data"].is_object()) {
        envelope["data"]["host_version"] = kHostVersion;
        envelope["data"]["port"] = port;
    }
    return envelope;
}

#ifdef _WIN32
std::wstring quote_windows_arg(const std::filesystem::path& path) {
    std::wstring arg = path.wstring();
    if (arg.empty()) return L"\"\"";
    bool needs_quotes = false;
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return arg;

    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::optional<std::wstring> utf8_to_wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     value.data(), static_cast<int>(value.size()),
                                     nullptr, 0);
    if (needed <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      value.data(), static_cast<int>(value.size()),
                                      out.data(), needed);
    if (written != needed) return std::nullopt;
    return out;
}
#endif

std::filesystem::path current_executable_path(char** argv) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) break;
        if (n < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), n));
        if (buffer.size() >= 32768) break;
        buffer.resize(buffer.size() * 2);
    }
#endif
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(argv[0], ec);
    if (!ec) return p;
    return std::filesystem::path(argv[0]);
}

bool start_detached_serve(char** argv, int port, std::string& error) {
    std::filesystem::path exe = current_executable_path(argv);
#ifdef _WIN32
    std::wstring command_line = quote_windows_arg(exe) + L" serve --json --port " +
        std::to_wstring(port);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = exe.parent_path().wstring();
    BOOL ok = CreateProcessW(nullptr,
                             mutable_command.data(),
                             nullptr,
                             nullptr,
                             FALSE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                             nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(),
                             &si,
                             &pi);
    if (!ok) {
        error = "failed to start ace-browser-host daemon";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        std::string port_s = std::to_string(port);
        execl(exe.string().c_str(), exe.string().c_str(), "serve", "--json", "--port",
              port_s.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

bool launch_browser_url(const std::string& url, std::string& error) {
#ifdef _WIN32
    auto wide_url = utf8_to_wide(url);
    if (!wide_url) {
        error = "browser launch URL is not valid UTF-8";
        return false;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wide_url->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    auto code = reinterpret_cast<intptr_t>(result);
    if (code > 32) return true;
    error = "ShellExecuteW failed: " + std::to_string(code);
    return false;
#else
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

int status_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    print_json(status_envelope(port));
    return 0;
}

int start_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    json before = status_envelope(port);
    if (before.value("ok", false) && before["data"].is_object() &&
        before["data"].value("running", false)) {
        before["data"]["start_attempted"] = false;
        before["data"]["already_running"] = true;
        print_json(before);
        return 0;
    }

    std::string error;
    bool started = start_detached_serve(argv, port, error);
    if (!started) {
        json out = before;
        if (out.value("ok", false) && out["data"].is_object()) {
            out["data"]["start_attempted"] = true;
            out["data"]["start_error"] = error.empty() ? "failed to start ace-browser-host daemon" : error;
            print_json(out);
        } else {
            print_json(failure("start_failed", error.empty() ? "failed to start ace-browser-host daemon" : error));
        }
        return 0;
    }

    json latest = before;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt < 5 ? 100 : 250));
        latest = status_envelope(port);
        if (latest.value("ok", false) && latest["data"].is_object()) {
            latest["data"]["start_attempted"] = true;
            latest["data"]["started"] = true;
            if (latest["data"].value("running", false)) {
                print_json(latest);
                return 0;
            }
        }
    }
    if (latest.value("ok", false) && latest["data"].is_object()) {
        latest["data"]["start_attempted"] = true;
        latest["data"]["started"] = true;
        latest["data"]["start_error"] = "ace-browser-host did not report running after start";
    }
    print_json(latest);
    return 0;
}

bool status_envelope_ready(const json& envelope) {
    if (!envelope.value("ok", false) || !envelope.contains("data") || !envelope["data"].is_object()) {
        return false;
    }
    const auto& data = envelope["data"];
    return data.value("running", false) &&
           data.value("extension_connected", false) &&
           !data.value("extension_stale", false) &&
           data.value("version_compatible", true);
}

std::string readiness_error_from_status(const json& envelope) {
    if (!envelope.value("ok", false)) {
        if (envelope.contains("error") && envelope["error"].is_object() &&
            envelope["error"].contains("code") && envelope["error"]["code"].is_string()) {
            return envelope["error"]["code"].get<std::string>();
        }
        return "status_error";
    }
    if (!envelope.contains("data") || !envelope["data"].is_object()) return "invalid_status";
    const auto& data = envelope["data"];
    if (!data.value("running", false)) return "daemon_not_running";
    if (!data.value("version_compatible", true)) return "version_mismatch";
    if (data.value("extension_stale", false)) return "extension_stale";
    if (!data.value("extension_connected", false)) return "extension_not_connected";
    return "not_ready";
}

void annotate_readiness(json& envelope,
                        bool host_start_attempted,
                        const std::string& host_start_error,
                        bool browser_launch_attempted,
                        const std::string& browser_launch_error,
                        const std::string& wake_url,
                        const std::string& ready_error) {
    if (!envelope.value("ok", false) || !envelope.contains("data") || !envelope["data"].is_object()) return;
    auto& data = envelope["data"];
    const bool ready = status_envelope_ready(envelope);
    data["ready"] = ready;
    data["host_start_attempted"] = host_start_attempted;
    data["host_start_error"] = host_start_error.empty() ? json(nullptr) : json(host_start_error);
    data["browser_launch_attempted"] = browser_launch_attempted;
    data["browser_launch_error"] = browser_launch_error.empty() ? json(nullptr) : json(browser_launch_error);
    data["wake_url"] = wake_url;
    data["ready_error"] = ready ? json(nullptr) : json(ready_error.empty() ? readiness_error_from_status(envelope) : ready_error);
    data["diagnostics"] = {
        {"host_running", data.value("running", false)},
        {"extension_connected", data.value("extension_connected", false)},
        {"extension_stale", data.value("extension_stale", false)},
        {"version_compatible", data.value("version_compatible", true)},
        {"host_start_attempted", host_start_attempted},
        {"browser_launch_attempted", browser_launch_attempted},
        {"ready_error", data["ready_error"]},
    };
}

int ensure_ready_command(int argc, char** argv) {
    const int port = parse_port(argc, argv);
    int timeout_ms = find_int_arg(argc, argv, "--timeout-ms").value_or(kEnsureReadyDefaultTimeoutMs);
    timeout_ms = std::max(1000, std::min(120000, timeout_ms));
    const bool launch_browser = !has_arg(argc, argv, "--no-launch-browser");
    const std::string wake_url = arg_or_default(
        argc, argv, "--wake-url", "http://127.0.0.1:" + std::to_string(port) + "/wake");

    bool host_start_attempted = false;
    bool browser_launch_attempted = false;
    std::string host_start_error;
    std::string browser_launch_error;
    json latest = status_envelope(port);

    if (latest.value("ok", false) && latest["data"].is_object() &&
        !latest["data"].value("running", false)) {
        host_start_attempted = true;
        if (!start_detached_serve(argv, port, host_start_error)) {
            annotate_readiness(latest, host_start_attempted, host_start_error,
                               browser_launch_attempted, browser_launch_error,
                               wake_url, "daemon_start_failed");
            print_json(latest);
            return 0;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool browser_launch_checked = false;
    while (std::chrono::steady_clock::now() <= deadline) {
        latest = status_envelope(port);
        if (!latest.value("ok", false)) {
            print_json(latest);
            return 0;
        }
        if (status_envelope_ready(latest)) {
            annotate_readiness(latest, host_start_attempted, host_start_error,
                               browser_launch_attempted, browser_launch_error,
                               wake_url, {});
            print_json(latest);
            return 0;
        }

        if (latest["data"].is_object() &&
            latest["data"].value("running", false) &&
            launch_browser && !browser_launch_checked) {
            browser_launch_checked = true;
            browser_launch_attempted = true;
            if (!launch_browser_url(wake_url, browser_launch_error)) {
                annotate_readiness(latest, host_start_attempted, host_start_error,
                                   browser_launch_attempted, browser_launch_error,
                                   wake_url, "browser_launch_failed");
                print_json(latest);
                return 0;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    latest = status_envelope(port);
    if (latest.value("ok", false)) {
        annotate_readiness(latest, host_start_attempted, host_start_error,
                           browser_launch_attempted, browser_launch_error,
                           wake_url, "readiness_timeout");
    }
    print_json(latest);
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
        return failure("daemon_not_running", "ace-browser-host daemon is not running on 127.0.0.1:" + std::to_string(port));
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

json command_envelope(int port, const std::string& session,
                      const std::string& action, json args) {
    json request = {
        {"session", session.empty() ? "acecode-default" : session},
        {"action", action},
        {"args", args.is_null() ? json::object() : std::move(args)},
    };
    return command_envelope_from_stdin(port, request.dump());
}

bool merge_args_json_or_print(int argc, char** argv, json& args) {
    auto extra = parse_args_json(argc, argv);
    if (!extra) return true;
    if (!extra->is_object()) {
        print_json(failure("invalid_request", "--args-json must be a JSON object"));
        return false;
    }
    for (auto& item : extra->items()) args[item.key()] = item.value();
    return true;
}

int command_alias_command(int argc, char** argv, const std::string& action, json args) {
    if (!merge_args_json_or_print(argc, argv, args)) return 0;
    print_json(command_envelope(parse_port(argc, argv),
                                arg_or_default(argc, argv, "--session", "acecode-default"),
                                action,
                                std::move(args)));
    return 0;
}

int open_command(int argc, char** argv) {
    auto url = find_arg(argc, argv, "--url");
    if (!url || url->empty()) {
        print_json(failure("invalid_request", "open requires --url <url>"));
        return 0;
    }
    json args;
    args["url"] = *url;
    args["newTab"] = find_bool_arg(argc, argv, "--new-tab").value_or(false) ||
                     find_bool_arg(argc, argv, "--newTab").value_or(false);
    put_string_arg(args, argc, argv, "--group-title", "group_title");
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    return command_alias_command(argc, argv, "navigate", std::move(args));
}

int find_tab_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--url", "url");
    put_bool_flag(args, argc, argv, "--active", "active");
    put_int_arg(args, argc, argv, "--tab-id", "tab_id");
    if (!args.contains("url") && !args.contains("active") && !args.contains("tab_id") &&
        !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "find-tab requires --url, --tab-id, --active, or --args-json"));
        return 0;
    }
    return command_alias_command(argc, argv, "find_tab", std::move(args));
}

int navigate_command(int argc, char** argv) {
    json args;
    std::string op = arg_or_default(argc, argv, "--operation", "");
    auto url = find_arg(argc, argv, "--url");
    if (op.empty() && url) op = "goto";
    if (op.empty()) {
        print_json(failure("invalid_request", "navigate requires --operation <goto|back|forward|reload> or --url <url>"));
        return 0;
    }
    if (op == "goto") {
        if (!url || url->empty()) {
            print_json(failure("invalid_request", "navigate --operation goto requires --url <url>"));
            return 0;
        }
        args["url"] = *url;
        args["newTab"] = false;
    } else {
        args["operation"] = op;
    }
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    return command_alias_command(argc, argv, "navigate", std::move(args));
}

int read_page_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--since-snapshot-id", "since_snapshot_id");
    return command_alias_command(argc, argv, "snapshot", std::move(args));
}

int wait_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--condition", "condition");
    put_string_arg(args, argc, argv, "--target", "target");
    put_string_arg(args, argc, argv, "--text", "text");
    put_string_arg(args, argc, argv, "--url", "url");
    put_string_arg(args, argc, argv, "--request-id", "request_id");
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    if (!args.contains("condition") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "wait requires --condition <condition>"));
        return 0;
    }
    return command_alias_command(argc, argv, "wait", std::move(args));
}

int block_input_command(int argc, char** argv) {
    json args;
    put_int_arg(args, argc, argv, "--watchdog-ms", "watchdog_ms");
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    put_string_arg(args, argc, argv, "--message", "message");
    return command_alias_command(argc, argv, "block_input", std::move(args));
}

int unblock_input_command(int argc, char** argv) {
    return command_alias_command(argc, argv, "unblock_input", json::object());
}

void put_interaction_options(json& args, int argc, char** argv) {
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--speed", "speed");
    put_int_arg(args, argc, argv, "--duration-ms", "duration_ms");
    put_int_arg(args, argc, argv, "--hold-ms", "hold_ms");
    put_number_arg(args, argc, argv, "--jitter", "jitter");
    put_bool_flag(args, argc, argv, "--debug-visualization", "debug_visualization");
    put_int_arg(args, argc, argv, "--debug-duration-ms", "debug_duration_ms");
    put_string_arg(args, argc, argv, "--snapshot-id", "snapshot_id");
}

int click_command(int argc, char** argv) {
    json args;
    auto target = find_arg(argc, argv, "--target");
    if (target) args["selector"] = *target;
    put_number_arg(args, argc, argv, "--x", "x");
    put_number_arg(args, argc, argv, "--y", "y");
    put_string_arg(args, argc, argv, "--button", "button");
    put_interaction_options(args, argc, argv);
    if (!args.contains("selector") && !(args.contains("x") && args.contains("y")) &&
        !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "click requires --target <ref|selector> or --x/--y"));
        return 0;
    }
    return command_alias_command(argc, argv, "click", std::move(args));
}

int fill_command(int argc, char** argv) {
    auto target = find_arg(argc, argv, "--target");
    auto value = find_arg(argc, argv, "--value");
    if ((!target || target->empty() || !value) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "fill requires --target <ref|selector> and --value <text>"));
        return 0;
    }
    json args;
    if (target) args["selector"] = *target;
    if (value) args["value"] = *value;
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--snapshot-id", "snapshot_id");
    return command_alias_command(argc, argv, "fill", std::move(args));
}

int type_command(int argc, char** argv) {
    auto target = find_arg(argc, argv, "--target");
    if ((!target || target->empty()) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "type requires --target <ref|selector>"));
        return 0;
    }
    json args;
    if (target) args["selector"] = *target;
    put_string_arg(args, argc, argv, "--text", "text");
    put_bool_flag(args, argc, argv, "--clear", "clear");
    put_bool_flag(args, argc, argv, "--submit", "submit");
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--speed", "speed");
    return command_alias_command(argc, argv, "type", std::move(args));
}

int hover_command(int argc, char** argv) {
    auto target = find_arg(argc, argv, "--target");
    if ((!target || target->empty()) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "hover requires --target <ref|selector>"));
        return 0;
    }
    json args;
    if (target) args["selector"] = *target;
    put_interaction_options(args, argc, argv);
    return command_alias_command(argc, argv, "hover", std::move(args));
}

int drag_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--from", "from");
    put_string_arg(args, argc, argv, "--to", "to");
    if (auto offset = find_arg(argc, argv, "--offset")) {
        auto comma = offset->find(',');
        if (comma != std::string::npos) {
            try {
                args["offset"] = json::array({
                    std::stod(offset->substr(0, comma)),
                    std::stod(offset->substr(comma + 1)),
                });
            } catch (...) {
                print_json(failure("invalid_request", "--offset must be <x>,<y>"));
                return 0;
            }
        }
    }
    put_interaction_options(args, argc, argv);
    if (!args.contains("from") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "drag requires --from <ref|selector>"));
        return 0;
    }
    return command_alias_command(argc, argv, "drag", std::move(args));
}

int scroll_command(int argc, char** argv) {
    json args;
    if (auto target = find_arg(argc, argv, "--target")) args["selector"] = *target;
    put_number_arg(args, argc, argv, "--delta-x", "delta_x");
    put_number_arg(args, argc, argv, "--delta-y", "delta_y");
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--speed", "speed");
    if (!args.contains("delta_y") && !args.contains("delta_x") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "scroll requires --delta-y or --delta-x"));
        return 0;
    }
    return command_alias_command(argc, argv, "scroll", std::move(args));
}

int evaluate_command(int argc, char** argv) {
    auto code = find_arg(argc, argv, "--code");
    if ((!code || code->empty()) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "evaluate requires --code <javascript>"));
        return 0;
    }
    json args;
    if (code) args["code"] = *code;
    return command_alias_command(argc, argv, "evaluate", std::move(args));
}

int network_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--cmd", "cmd");
    put_string_arg(args, argc, argv, "--filter", "filter");
    put_string_arg(args, argc, argv, "--request-id", "requestId");
    if (!args.contains("cmd") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "network requires --cmd <start|stop|list|detail>"));
        return 0;
    }
    return command_alias_command(argc, argv, "network", std::move(args));
}

void put_csv_arg(json& args, int argc, char** argv,
                 const std::string& cli_name, const std::string& json_name) {
    auto value = find_arg(argc, argv, cli_name);
    if (!value) return;
    args[json_name] = split_csv(*value);
}

void put_json_object_arg(json& args, int argc, char** argv,
                         const std::string& cli_name, const std::string& json_name) {
    auto value = find_arg(argc, argv, cli_name);
    if (!value) return;
    auto parsed = parse_json(*value);
    if (!parsed || !parsed->is_object()) {
        args[json_name] = "__ACE_INVALID_JSON_OBJECT__";
        return;
    }
    args[json_name] = std::move(*parsed);
}

std::optional<json> materialize_devtools_output(json& envelope, const json& args) {
    if (!envelope.value("ok", false) || !envelope.contains("data") || !envelope["data"].is_object()) {
        return std::nullopt;
    }
    json& data = envelope["data"];
    const std::string cmd = args.value("cmd", "");
    const std::string output = args.value("output", "");
    if (cmd == "performance-stop" && !output.empty() && data.contains("trace")) {
        const std::string text = data["trace"].dump();
        std::filesystem::path path = normalize_bridge_path(output);
        if (auto error = write_text_file(path, text)) return error;
        data["path"] = path.u8string();
        data["output"] = path.u8string();
        data["sizeBytes"] = text.size();
        data["size_bytes"] = text.size();
        data.erase("trace");
    }
    if (cmd == "heap-snapshot" && !output.empty() && data.contains("snapshot") && data["snapshot"].is_string()) {
        const std::string text = data["snapshot"].get<std::string>();
        std::filesystem::path path = normalize_bridge_path(output);
        if (auto error = write_text_file(path, text)) return error;
        data["path"] = path.u8string();
        data["output"] = path.u8string();
        data["sizeBytes"] = text.size();
        data["size_bytes"] = text.size();
        data.erase("snapshot");
    }
    if (cmd == "network-detail") {
        const std::string request_file = args.value("request_file", "");
        if (!request_file.empty() && data.contains("postData") && data["postData"].is_string()) {
            const std::string text = data["postData"].get<std::string>();
            std::filesystem::path path = normalize_bridge_path(request_file);
            if (auto error = write_text_file(path, text)) return error;
            data["requestFile"] = path.u8string();
            data["request_file"] = path.u8string();
            data["requestBodySizeBytes"] = text.size();
            data["request_body_size_bytes"] = text.size();
        }

        const std::string response_file = args.value("response_file", "");
        if (!response_file.empty()) {
            json* body = nullptr;
            if (data.contains("responseBody")) body = &data["responseBody"];
            else if (data.contains("response_body")) body = &data["response_body"];
            else if (data.contains("body")) body = &data["body"];
            if (body) {
                std::filesystem::path path = normalize_bridge_path(response_file);
                std::size_t size = 0;
                if (body->is_object() && body->value("base64Encoded", false) && body->contains("body") && (*body)["body"].is_string()) {
                    auto bytes = decode_base64((*body)["body"].get<std::string>());
                    if (!bytes) return failure("invalid_host_response", "bridge returned invalid base64 response body");
                    if (auto error = write_bytes_file(path, *bytes)) return error;
                    size = bytes->size();
                } else {
                    std::string text = body->is_string() ? body->get<std::string>() : body->dump();
                    if (auto error = write_text_file(path, text)) return error;
                    size = text.size();
                }
                data["responseFile"] = path.u8string();
                data["response_file"] = path.u8string();
                data["responseBodySizeBytes"] = size;
                data["response_body_size_bytes"] = size;
                data.erase("responseBody");
                data.erase("response_body");
                data.erase("body");
            }
        }
    }
    return std::nullopt;
}

int devtools_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--cmd", "cmd");
    put_string_arg(args, argc, argv, "--filter", "filter");
    put_string_arg(args, argc, argv, "--request-id", "requestId");
    put_string_arg(args, argc, argv, "--resource-type", "resource_type");
    put_int_arg(args, argc, argv, "--id", "id");
    put_int_arg(args, argc, argv, "--page-size", "page_size");
    put_int_arg(args, argc, argv, "--page-idx", "page_idx");
    put_csv_arg(args, argc, argv, "--types", "types");
    put_bool_flag(args, argc, argv, "--preserve", "preserve");
    put_string_arg(args, argc, argv, "--viewport", "viewport");
    put_int_arg(args, argc, argv, "--width", "width");
    put_int_arg(args, argc, argv, "--height", "height");
    put_number_arg(args, argc, argv, "--device-scale-factor", "device_scale_factor");
    put_bool_flag(args, argc, argv, "--mobile", "mobile");
    put_bool_flag(args, argc, argv, "--touch", "touch");
    put_bool_flag(args, argc, argv, "--reset", "reset");
    put_bool_flag(args, argc, argv, "--clear", "clear");
    put_string_arg(args, argc, argv, "--network-conditions", "network_conditions");
    put_number_arg(args, argc, argv, "--cpu-throttling-rate", "cpu_throttling_rate");
    put_string_arg(args, argc, argv, "--geolocation", "geolocation");
    put_number_arg(args, argc, argv, "--accuracy", "accuracy");
    put_string_arg(args, argc, argv, "--user-agent", "user_agent");
    put_string_arg(args, argc, argv, "--color-scheme", "color_scheme");
    put_json_object_arg(args, argc, argv, "--extra-http-headers", "extra_http_headers");
    put_bool_flag(args, argc, argv, "--reload", "reload");
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    put_string_arg(args, argc, argv, "--categories", "categories");
    put_string_arg(args, argc, argv, "--output", "output");
    put_string_arg(args, argc, argv, "--request-file", "request_file");
    put_string_arg(args, argc, argv, "--response-file", "response_file");

    if (args.value("extra_http_headers", json()).is_string() &&
        args["extra_http_headers"].get<std::string>() == "__ACE_INVALID_JSON_OBJECT__") {
        print_json(failure("invalid_request", "--extra-http-headers must be a JSON object"));
        return 0;
    }
    if (!args.contains("cmd") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "devtools requires --cmd <command>"));
        return 0;
    }
    if (!merge_args_json_or_print(argc, argv, args)) return 0;
    json envelope = command_envelope(parse_port(argc, argv),
                                     arg_or_default(argc, argv, "--session", "acecode-default"),
                                     "devtools",
                                     args);
    if (auto error = materialize_devtools_output(envelope, args)) {
        print_json(*error);
        return 0;
    }
    print_json(envelope);
    return 0;
}

int cdp_command(int argc, char** argv) {
    auto method = find_arg(argc, argv, "--method");
    if ((!method || method->empty()) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "cdp requires --method <CDP.method>"));
        return 0;
    }
    json args;
    if (method) args["method"] = *method;
    if (auto params_text = find_arg(argc, argv, "--params")) {
        auto parsed = parse_json(*params_text);
        if (!parsed || !parsed->is_object()) {
            print_json(failure("invalid_request", "--params must be a JSON object"));
            return 0;
        }
        args["params"] = std::move(*parsed);
    }
    if (!merge_args_json_or_print(argc, argv, args)) return 0;
    print_json(command_envelope(parse_port(argc, argv),
                                arg_or_default(argc, argv, "--session", "acecode-default"),
                                "raw_cdp",
                                std::move(args)));
    return 0;
}

int save_pdf_command(int argc, char** argv) {
    json args;
    put_string_arg(args, argc, argv, "--paper-format", "paper_format");
    put_bool_flag(args, argc, argv, "--landscape", "landscape");
    put_number_arg(args, argc, argv, "--scale", "scale");
    put_bool_flag(args, argc, argv, "--print-background", "print_background");
    put_string_arg(args, argc, argv, "--file-name", "file_name");
    return command_alias_command(argc, argv, "save_as_pdf", std::move(args));
}

int list_tabs_command(int argc, char** argv) {
    return command_alias_command(argc, argv, "list_tabs", json::object());
}

int close_session_command(int argc, char** argv) {
    return command_alias_command(argc, argv, "close_session", json::object());
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

std::string http_html_response(int status, const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " OK\r\n";
    resp << "Content-Type: text/html; charset=utf-8\r\n";
    resp << "Cache-Control: no-store\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << body;
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
    bool shutting_down = false;
    std::deque<std::shared_ptr<PendingAction>> queued_actions;
    std::unordered_map<std::string, std::shared_ptr<PendingAction>> pending_actions;
};

long long extension_last_seen_ms_locked(const DaemonState& state) {
    if (state.last_hello == std::chrono::steady_clock::time_point{}) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state.last_hello).count();
}

bool extension_fresh_locked(const DaemonState& state) {
    long long age_ms = extension_last_seen_ms_locked(state);
    return state.extension_connected && age_ms >= 0 && age_ms <= kExtensionFreshnessMs;
}

json daemon_status_payload(DaemonState& state, int port) {
    std::lock_guard<std::mutex> lock(state.mutex);
    const long long last_seen_ms = extension_last_seen_ms_locked(state);
    const bool has_seen = last_seen_ms >= 0;
    const bool fresh = extension_fresh_locked(state);
    const bool stale = state.extension_connected && !fresh;
    return json{
        {"running", true},
        {"ready", fresh && state.version_compatible},
        {"extension_connected", fresh},
        {"extension_stale", stale},
        {"extension_last_seen_ms", has_seen ? json(last_seen_ms) : json(nullptr)},
        {"extension_freshness_ms", kExtensionFreshnessMs},
        {"version", kDaemonVersion},
        {"extension_version", has_seen ? json(state.extension_version) : json(nullptr)},
        {"protocol_version", has_seen ? json(state.protocol_version) : json(nullptr)},
        {"host_protocol_version", kProtocolVersion},
        {"version_compatible", state.version_compatible},
        {"version_error", state.version_error.empty() ? json(nullptr) : json(state.version_error)},
        {"host_version", kHostVersion},
        {"port", port},
        {"browser", state.browser.empty() ? json(nullptr) : json(state.browser)},
        {"capabilities", fresh ? state.capabilities : default_capabilities(false)},
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
            " is not compatible with ace-browser-host protocol " + kProtocolVersion;
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
        if (state.shutting_down) {
            return failure("daemon_shutting_down", "ace-browser-host daemon is shutting down");
        }
        if (!state.extension_connected) {
            return failure("extension_not_connected", "ace-browser-bridge browser plugin is not connected");
        }
        if (!extension_fresh_locked(state)) {
            return failure("extension_stale", "ace-browser-bridge browser plugin connection is stale");
        }
        if (!state.version_compatible) {
            return failure("version_mismatch", state.version_error.empty()
                ? "ace-browser-host and ace-browser-bridge protocol versions are not compatible"
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
        return pending->result.has_value() || state.shutting_down;
    });
    state.pending_actions.erase(pending->id);
    if (state.shutting_down && !pending->result) {
        state.queued_actions.erase(
            std::remove(state.queued_actions.begin(), state.queued_actions.end(), pending),
            state.queued_actions.end());
        return failure("daemon_shutting_down", "ace-browser-host daemon is shutting down");
    }
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
        if (state.shutting_down) {
            return success({{"action", nullptr}});
        }
        state.extension_connected = true;
        state.last_hello = std::chrono::steady_clock::now();
    }

    std::unique_lock<std::mutex> lock(state.mutex);
    state.cv.wait_for(lock, std::chrono::seconds(20), [&]() {
        return !state.queued_actions.empty() || state.shutting_down;
    });
    if (state.shutting_down) {
        return success({{"action", nullptr}});
    }
    if (!state.extension_connected || !extension_fresh_locked(state)) {
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
    if (req.method == "GET" && req.path == "/wake") {
        std::ostringstream html;
        html << "<!doctype html><html><head><meta charset=\"utf-8\">"
             << "<title>ACE Browser Bridge Wake</title>"
             << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
             << "<style>body{font:14px/1.5 -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;"
             << "margin:32px;color:#0f172a}code{background:#f1f5f9;padding:2px 5px;"
             << "border-radius:4px}</style></head><body>"
             << "<h1>ACE Browser Bridge</h1>"
             << "<p>This local page wakes the browser extension for ACECode.</p>"
             << "<p>Daemon: <code>127.0.0.1:" << port << "</code></p>"
             << "</body></html>";
        return http_html_response(200, html.str());
    }
    const bool host_request = header_equals(req, "x-ace-browser-host", "1") ||
                              header_equals(req, "x-ace-browser-cli", "1");
    const bool plugin_request = header_equals(req, "x-ace-browser-bridge", "extension");
    if (req.method == "GET" && req.path == "/status") {
        if (!host_request) return http_json_response(403, failure("unauthorized", "status requires ace-browser-host"));
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
        if (!host_request) return http_json_response(403, failure("unauthorized", "command requires ace-browser-host"));
        return http_json_response(200, handle_command(state, req.body));
    }
    if (req.method == "POST" && req.path == "/shutdown") {
        if (!host_request) return http_json_response(403, failure("unauthorized", "shutdown requires ace-browser-host"));
        running = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.shutting_down = true;
        }
        state.cv.notify_all();
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

    auto state = std::make_shared<DaemonState>();
    auto running = std::make_shared<std::atomic<bool>>(true);
    while (running->load()) {
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
        std::thread([client, state, port, running]() {
            auto req = read_http_request(client);
            std::string response;
            if (!req) {
                response = http_json_response(400, failure("invalid_http_request", "failed to parse HTTP request"));
            } else {
                response = route_request(*req, *state, port, *running);
            }
            send_all(client, response);
            close_socket(client);
        }).detach();
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->shutting_down = true;
    }
    state->cv.notify_all();
    close_socket(listener);
    return 0;
}

int shutdown_command(int argc, char** argv) {
    int port = parse_port(argc, argv);
    HttpResponse response = http_request("POST", "/shutdown", "{}", port);
    if (!response.transport_ok) {
        print_json(failure("daemon_not_running", "ace-browser-host daemon is not running on 127.0.0.1:" + std::to_string(port)));
        return 0;
    }
    print_json(normalize_daemon_envelope(response.body));
    return 0;
}

void print_help() {
    std::cout
        << "ace-browser-host commands:\n"
        << "  start --json [--port 52007]\n"
        << "  ensure-ready --json [--port 52007] [--timeout-ms <ms>] [--no-launch-browser]\n"
        << "  status --json [--port 52007]\n"
        << "  command --json [--port 52007]     # reads {session,action,args} from stdin\n"
        << "  open --json --url <url> [--session <name>] [--new-tab] [--timeout-ms <ms>]\n"
        << "  find-tab --json (--url <text>|--tab-id <id>|--active) [--session <name>]\n"
        << "  navigate --json --operation <goto|back|forward|reload> [--url <url>] [--session <name>] [--timeout-ms <ms>]\n"
        << "  read-page --json [--session <name>] [--mode summary|elements|focused|changed]\n"
        << "  wait --json --condition <condition> [--target <ref>] [--timeout-ms <ms>]\n"
        << "  block-input --json [--session <name>] [--watchdog-ms <ms>] [--message <text>]\n"
        << "  unblock-input --json [--session <name>]\n"
        << "  click|fill|type|hover|drag|scroll --json [--session <name>] ...\n"
        << "  evaluate --json --code <javascript> [--session <name>]\n"
        << "  network --json --cmd <start|stop|list|detail> [--filter <text>] [--request-id <id>]\n"
        << "  devtools --json --cmd <console-start|console-list|network-start|network-detail|emulate|performance-start|performance-stop|heap-snapshot> [--session <name>]\n"
        << "  cdp --json --method <CDP.method> [--params <json>] [--session <name>]\n"
        << "  screenshot --json --session <name> --output <path> [--port 52007]\n"
        << "  save-pdf --json [--session <name>] [--file-name <name>]\n"
        << "  list-tabs --json [--session <name>]\n"
        << "  close-session --json [--session <name>]\n"
        << "  serve --json [--port 52007]\n"
        << "  shutdown --json [--port 52007]\n";
}

int main_impl(int argc, char** argv) {
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
    if (command == "start") return start_command(argc, argv);
    if (command == "ensure-ready") return ensure_ready_command(argc, argv);
    if (command == "status") return status_command(argc, argv);
    if (command == "command") return command_command(argc, argv);
    if (command == "open") return open_command(argc, argv);
    if (command == "find-tab") return find_tab_command(argc, argv);
    if (command == "navigate") return navigate_command(argc, argv);
    if (command == "read-page") return read_page_command(argc, argv);
    if (command == "wait") return wait_command(argc, argv);
    if (command == "block-input") return block_input_command(argc, argv);
    if (command == "unblock-input") return unblock_input_command(argc, argv);
    if (command == "click") return click_command(argc, argv);
    if (command == "fill") return fill_command(argc, argv);
    if (command == "type") return type_command(argc, argv);
    if (command == "hover") return hover_command(argc, argv);
    if (command == "drag") return drag_command(argc, argv);
    if (command == "scroll") return scroll_command(argc, argv);
    if (command == "evaluate") return evaluate_command(argc, argv);
    if (command == "network") return network_command(argc, argv);
    if (command == "devtools") return devtools_command(argc, argv);
    if (command == "cdp") return cdp_command(argc, argv);
    if (command == "screenshot") return screenshot_command(argc, argv);
    if (command == "save-pdf") return save_pdf_command(argc, argv);
    if (command == "list-tabs") return list_tabs_command(argc, argv);
    if (command == "close-session") return close_session_command(argc, argv);
    if (command == "serve") return serve_command(argc, argv);
    if (command == "shutdown") return shutdown_command(argc, argv);

    print_json(failure("unknown_command", "unknown command: " + command));
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        configure_utf8_console();
        std::string encoding_error;
        auto utf8_argv = make_utf8_argv(argc, argv, encoding_error);
        if (!utf8_argv) {
            print_json(failure("invalid_encoding", encoding_error));
            return 1;
        }
        return main_impl(utf8_argv->argc(), utf8_argv->argv());
    } catch (const nlohmann::json::exception& e) {
        std::string code = e.id == 316 ? "invalid_encoding" : "json_error";
        print_json(failure(code, safe_error_message(e.what())));
        return 1;
    } catch (const std::exception& e) {
        print_json(failure("internal_error", safe_error_message(e.what())));
        return 1;
    } catch (...) {
        print_json(failure("internal_error", "unknown internal error"));
        return 1;
    }
}
