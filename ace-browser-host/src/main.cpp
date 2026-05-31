#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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
#include <sys/wait.h>
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
// 一条命令最多等浏览器插件回结果的总时长。
constexpr int kCommandTimeoutMs = 30000;
// 指令被某次 poll 取走后,若超过这个时间还没收到插件 ack,就判定那次投递丢了(接走它的
// service worker 多半已被浏览器回收),把指令重新入队再投一次。loopback 上正常 ack 仅需毫秒级,
// 4 秒足以把"还活着只是慢"和"已经死了"区分开,不会误重投正在执行的慢操作。
constexpr int kRedeliveryAfterMs = 4000;
// 命令级超时上限。长 batch / 长 wait 可在请求里带 command_timeout_ms 把等待从默认 30s 抬高,
// 但不超过这个上限,避免单条命令无限期占住 daemon 线程。
constexpr int kMaxCommandTimeoutMs = 180000;
constexpr int kCommandTimeoutPaddingMs = 5000;
constexpr int kDefaultBatchStepBudgetMs = 5000;

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
    };
}

json direct_cdp_capabilities(bool direct_ready) {
    return json{
        {"cdp", direct_ready},
        {"devtools", false},
        {"raw_cdp", direct_ready},
        {"console", false},
        {"network", false},
        {"emulation", false},
        {"performance", false},
        {"heap_snapshot", false},
        {"pdf", false},
        {"upload", false},
        {"os_pointer", false},
        {"operation_overlay", false},
    };
}

json merge_capabilities(json base, const json& overlay) {
    if (!base.is_object()) base = json::object();
    if (!overlay.is_object()) return base;
    for (const auto& item : overlay.items()) {
        if (item.value().is_boolean()) {
            base[item.key()] = base.value(item.key(), false) || item.value().get<bool>();
        } else if (!base.contains(item.key())) {
            base[item.key()] = item.value();
        }
    }
    return base;
}

json stopped_status(int port) {
    return json{
        {"running", false},
        {"ready", false},
        {"backend", "none"},
        {"direct_cdp_ready", false},
        {"direct_cdp", {
            {"ready", false},
            {"ws_url", nullptr},
            {"debug_port", nullptr},
            {"last_error", nullptr},
        }},
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

std::string encode_base64(std::string_view input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const unsigned int n =
            (static_cast<unsigned char>(input[i]) << 16) |
            (static_cast<unsigned char>(input[i + 1]) << 8) |
            static_cast<unsigned char>(input[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
        i += 3;
    }
    if (i < input.size()) {
        unsigned int n = static_cast<unsigned char>(input[i]) << 16;
        const bool has_second = i + 1 < input.size();
        if (has_second) n |= static_cast<unsigned char>(input[i + 1]) << 8;
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(has_second ? alphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
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

std::optional<std::string> read_text_file(const std::filesystem::path& path,
                                          std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        error = "failed to read bridge input file: " + path.u8string();
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        error = "failed to read bridge input file: " + path.u8string();
        return std::nullopt;
    }
    return ss.str();
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

json direct_ensure_envelope(int port) {
    HttpResponse response = http_request("POST", "/direct/ensure", "{}", port);
    if (!response.transport_ok) {
        return failure("daemon_not_running", "ace-browser-host daemon is not running on 127.0.0.1:" + std::to_string(port));
    }
    if (response.status != 200) {
        return failure("daemon_error", "direct ensure endpoint returned HTTP " + std::to_string(response.status));
    }
    return normalize_daemon_envelope(response.body);
}

#ifdef _WIN32
std::wstring quote_windows_command_arg(std::wstring arg) {
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

std::wstring quote_windows_arg(const std::filesystem::path& path) {
    return quote_windows_command_arg(path.wstring());
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

std::string host_log_truncate(std::string value, std::size_t max_len = 800) {
    if (value.size() <= max_len) return value;
    return value.substr(0, max_len) + "...(" + std::to_string(value.size()) + " bytes)";
}

std::string json_for_log(const json& value, std::size_t max_len = 1200) {
    if (value.is_null()) return "null";
    return host_log_truncate(value.dump(), max_len);
}

#ifdef _WIN32
std::optional<std::wstring> env_w(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return std::nullopt;
    value.resize(written);
    return value;
}
#endif

std::filesystem::path acecode_user_data_dir() {
#ifdef _WIN32
    if (auto profile = env_w(L"USERPROFILE"); profile && !profile->empty()) {
        return std::filesystem::path(*profile) / L".acecode";
    }
    auto drive = env_w(L"HOMEDRIVE");
    auto path = env_w(L"HOMEPATH");
    if (drive && path && !drive->empty() && !path->empty()) {
        return std::filesystem::path(*drive + *path) / L".acecode";
    }
    if (auto appdata = env_w(L"APPDATA"); appdata && !appdata->empty()) {
        return std::filesystem::path(*appdata) / L"acecode";
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".acecode";
    }
#endif
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) return cwd / ".acecode";
    return std::filesystem::path(".acecode");
}

std::filesystem::path acecode_log_dir() {
    return acecode_user_data_dir() / "logs";
}

std::tm local_tm(std::time_t time) {
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    return tm_buf;
}

std::string date_string(const std::tm& tm_buf) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1,
                  tm_buf.tm_mday);
    return std::string(buf);
}

unsigned long current_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::mutex& host_log_mutex() {
    static std::mutex mu;
    return mu;
}

void host_log_line(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lk(host_log_mutex());
    try {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm tm_buf = local_tm(time);

        std::error_code ec;
        const auto dir = acecode_log_dir();
        std::filesystem::create_directories(dir, ec);
        if (ec) return;

        const auto path = dir / ("ace-browser-host-" + date_string(tm_buf) + ".log");
        std::ofstream out(path, std::ios::out | std::ios::app);
        if (!out.is_open()) return;
        out << std::put_time(&tm_buf, "%H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms.count()
            << " [pid=" << current_process_id() << "] "
            << level << " " << message << "\n";
    } catch (...) {
        // Logging must never affect the CLI JSON protocol.
    }
}

void host_log_info(const std::string& message) {
    host_log_line("INF", message);
}

void host_log_warn(const std::string& message) {
    host_log_line("WRN", message);
}

void host_log_error(const std::string& message) {
    host_log_line("ERR", message);
}

std::string envelope_error_code(const json& envelope) {
    if (envelope.contains("error") && envelope["error"].is_object() &&
        envelope["error"].contains("code") && envelope["error"]["code"].is_string()) {
        return envelope["error"]["code"].get<std::string>();
    }
    return {};
}

std::string action_session_from_request(const json& request) {
    return request.value("session", "acecode-default");
}

std::string action_name_from_request(const json& request) {
    return request.value("action", "<missing>");
}

long long elapsed_ms_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

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
    host_log_info("[cli] start_detached_serve requested port=" + std::to_string(port));
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
        DWORD error_code = GetLastError();
        error = "failed to start ace-browser-host daemon";
        host_log_error("[cli] start_detached_serve failed port=" + std::to_string(port) +
                       " win32_error=" + std::to_string(static_cast<unsigned long>(error_code)));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    host_log_info("[cli] start_detached_serve launched port=" + std::to_string(port));
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        host_log_error("[cli] start_detached_serve fork_failed port=" + std::to_string(port));
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
    host_log_info("[cli] start_detached_serve launched port=" + std::to_string(port) +
                  " pid=" + std::to_string(static_cast<long long>(pid)));
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
    host_log_info("[cli] start command port=" + std::to_string(port));
    json before = status_envelope(port);
    if (before.value("ok", false) && before["data"].is_object() &&
        before["data"].value("running", false)) {
        before["data"]["start_attempted"] = false;
        before["data"]["already_running"] = true;
        host_log_info("[cli] start command already_running port=" + std::to_string(port));
        print_json(before);
        return 0;
    }

    std::string error;
    bool started = start_detached_serve(argv, port, error);
    if (!started) {
        host_log_error("[cli] start command failed port=" + std::to_string(port) +
                       " error=" + error);
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
                host_log_info("[cli] start command ready port=" + std::to_string(port));
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
    host_log_warn("[cli] start command timeout waiting for daemon port=" + std::to_string(port));
    print_json(latest);
    return 0;
}

bool status_envelope_ready(const json& envelope) {
    if (!envelope.value("ok", false) || !envelope.contains("data") || !envelope["data"].is_object()) {
        return false;
    }
    const auto& data = envelope["data"];
    if (data.contains("ready") && data["ready"].is_boolean()) {
        return data.value("running", false) &&
               data.value("ready", false) &&
               data.value("version_compatible", true);
    }
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
    if (data.value("direct_cdp_ready", false)) return "not_ready";
    if (data.contains("direct_cdp") && data["direct_cdp"].is_object() &&
        data["direct_cdp"].contains("last_error") && data["direct_cdp"]["last_error"].is_string()) {
        return "direct_cdp_unavailable";
    }
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
        {"backend", data.value("backend", "none")},
        {"direct_cdp_ready", data.value("direct_cdp_ready", false)},
        {"direct_cdp", data.value("direct_cdp", json::object())},
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
    host_log_info("[cli] ensure-ready start port=" + std::to_string(port) +
                  " timeout_ms=" + std::to_string(timeout_ms) +
                  " launch_browser=" + std::string(launch_browser ? "true" : "false"));

    if (latest.value("ok", false) && latest["data"].is_object() &&
        !latest["data"].value("running", false)) {
        host_start_attempted = true;
        if (!start_detached_serve(argv, port, host_start_error)) {
            host_log_error("[cli] ensure-ready daemon_start_failed port=" + std::to_string(port) +
                           " error=" + host_start_error);
            annotate_readiness(latest, host_start_attempted, host_start_error,
                               browser_launch_attempted, browser_launch_error,
                               wake_url, "daemon_start_failed");
            print_json(latest);
            return 0;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool direct_ensure_checked = false;
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
            host_log_info("[cli] ensure-ready ready port=" + std::to_string(port) +
                          " host_start_attempted=" + std::string(host_start_attempted ? "true" : "false") +
                          " browser_launch_attempted=" + std::string(browser_launch_attempted ? "true" : "false"));
            print_json(latest);
            return 0;
        }

        if (latest["data"].is_object() &&
            latest["data"].value("running", false) &&
            launch_browser && !direct_ensure_checked) {
            direct_ensure_checked = true;
            browser_launch_attempted = true;
            json direct = direct_ensure_envelope(port);
            if (direct.value("ok", false) && direct.contains("data") && direct["data"].is_object()) {
                latest = direct;
                if (status_envelope_ready(latest)) {
                    annotate_readiness(latest, host_start_attempted, host_start_error,
                                       browser_launch_attempted, browser_launch_error,
                                       wake_url, {});
                    host_log_info("[cli] ensure-ready direct_cdp_ready port=" + std::to_string(port));
                    print_json(latest);
                    return 0;
                }
            } else if (!direct.value("ok", false)) {
                browser_launch_error = envelope_error_code(direct);
                host_log_warn("[cli] ensure-ready direct_cdp_failed port=" + std::to_string(port) +
                              " error=" + browser_launch_error);
            }
        }

        if (latest["data"].is_object() &&
            latest["data"].value("running", false) &&
            launch_browser && !browser_launch_checked) {
            browser_launch_checked = true;
            browser_launch_attempted = true;
            if (!launch_browser_url(wake_url, browser_launch_error)) {
                host_log_error("[cli] ensure-ready browser_launch_failed port=" + std::to_string(port) +
                               " error=" + browser_launch_error);
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
    host_log_warn("[cli] ensure-ready timeout port=" + std::to_string(port) +
                  " ready_error=" + readiness_error_from_status(latest));
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

    const std::string session = action_session_from_request(*parsed);
    const std::string action = action_name_from_request(*parsed);
    const auto started = std::chrono::steady_clock::now();
    host_log_info("[cli] command send action=" + action +
                  " session=" + session +
                  " port=" + std::to_string(port));
    HttpResponse response = http_request("POST", "/command", parsed->dump(), port);
    if (!response.transport_ok) {
        host_log_warn("[cli] command transport_failed action=" + action +
                      " session=" + session +
                      " port=" + std::to_string(port) +
                      " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return failure("daemon_not_running", "ace-browser-host daemon is not running on 127.0.0.1:" + std::to_string(port));
    }
    if (response.status != 200) {
        host_log_warn("[cli] command http_error action=" + action +
                      " session=" + session +
                      " status=" + std::to_string(response.status) +
                      " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return failure("daemon_error", "daemon command endpoint returned HTTP " + std::to_string(response.status));
    }
    json envelope = normalize_daemon_envelope(response.body);
    const std::string error_code = envelope_error_code(envelope);
    const std::string line = "[cli] command finish action=" + action +
        " session=" + session +
        " ok=" + std::string(envelope.value("ok", false) ? "true" : "false") +
        (error_code.empty() ? "" : " error_code=" + error_code) +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    if (envelope.value("ok", false)) {
        host_log_info(line);
    } else {
        host_log_warn(line);
    }
    return envelope;
}

int command_command(int argc, char** argv) {
    print_json(command_envelope_from_stdin(parse_port(argc, argv), read_stdin()));
    return 0;
}

long long clamp_command_timeout_ms(long long requested) {
    return std::clamp<long long>(requested, kCommandTimeoutMs, kMaxCommandTimeoutMs);
}

std::optional<long long> positive_int_field(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return std::nullopt;
    try {
        long long value = object.value(key, 0LL);
        if (value > 0) return value;
    } catch (...) {
    }
    return std::nullopt;
}

long long action_budget_ms(const json& args) {
    long long budget = kDefaultBatchStepBudgetMs;
    if (!args.is_object()) return budget;
    if (auto timeout = positive_int_field(args, "timeout_ms")) {
        budget = std::max(budget, *timeout);
    }
    if (args.contains("expect") && args["expect"].is_object()) {
        if (auto expect_timeout = positive_int_field(args["expect"], "timeout_ms")) {
            budget = std::max(budget, *expect_timeout);
        }
    }
    return budget;
}

long long batch_steps_raw_budget_ms(const json& steps) {
    if (!steps.is_array() || steps.empty()) return 0;
    long long total = 0;
    for (const auto& step : steps) {
        const json empty_args = json::object();
        const json& args = (step.is_object() && step.contains("args") && step["args"].is_object())
            ? step["args"]
            : empty_args;
        long long attempts = 1;
        if (step.is_object() && step.contains("retry") && step["retry"].is_object()) {
            if (auto retry_attempts = positive_int_field(step["retry"], "attempts")) {
                attempts = std::clamp<long long>(*retry_attempts, 1, 10);
            }
        }
        long long delay_ms = 0;
        if (step.is_object() && step.contains("retry") && step["retry"].is_object()) {
            if (auto retry_delay = positive_int_field(step["retry"], "delay_ms")) {
                delay_ms = std::min<long long>(*retry_delay, 30000);
            }
        }
        total += action_budget_ms(args) * attempts;
        if (attempts > 1) total += delay_ms * (attempts - 1);
        if (total >= kMaxCommandTimeoutMs) return kMaxCommandTimeoutMs;
    }
    return total;
}

long long batch_command_timeout_budget_ms(const json& steps) {
    if (!steps.is_array() || steps.empty()) return kCommandTimeoutMs;
    long long total = kCommandTimeoutPaddingMs + batch_steps_raw_budget_ms(steps);
    return clamp_command_timeout_ms(total);
}

long long batch_command_timeout_budget_for_args(const json& args) {
    if (!args.is_object()) return kCommandTimeoutMs;
    long long total = kCommandTimeoutPaddingMs;
    if (args.contains("steps") && args["steps"].is_array()) {
        total += batch_steps_raw_budget_ms(args["steps"]);
    }
    if (args.contains("finally") && args["finally"].is_array()) {
        total += batch_steps_raw_budget_ms(args["finally"]);
    }
    return clamp_command_timeout_ms(total);
}

std::optional<long long> command_timeout_budget_for_action(const std::string& action,
                                                           const json& args) {
    if (action == "batch") {
        return batch_command_timeout_budget_for_args(args);
    }
    if (!args.is_object()) return std::nullopt;
    long long requested = 0;
    if (auto timeout = positive_int_field(args, "timeout_ms")) {
        requested = std::max(requested, *timeout);
    }
    if (args.contains("expect") && args["expect"].is_object()) {
        if (auto expect_timeout = positive_int_field(args["expect"], "timeout_ms")) {
            requested = std::max(requested, *expect_timeout);
        }
    }
    if (requested <= 0) return std::nullopt;
    return clamp_command_timeout_ms(requested + kCommandTimeoutPaddingMs);
}

json command_request(const std::string& session, const std::string& action, json args) {
    json request = {
        {"session", session.empty() ? "acecode-default" : session},
        {"action", action},
        {"args", args.is_null() ? json::object() : std::move(args)},
    };
    if (auto command_timeout = command_timeout_budget_for_action(action, request["args"])) {
        request["command_timeout_ms"] = *command_timeout;
    }
    return request;
}

json command_envelope(int port, const std::string& session,
                      const std::string& action, json args) {
    json request = command_request(session, action, std::move(args));
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

void put_condition_options(json& args, int argc, char** argv) {
    put_string_arg(args, argc, argv, "--condition", "condition");
    put_string_arg(args, argc, argv, "--target", "target");
    put_string_arg(args, argc, argv, "--target-text", "target_text");
    put_string_arg(args, argc, argv, "--text", "text");
    put_string_arg(args, argc, argv, "--value", "value");
    put_string_arg(args, argc, argv, "--url", "url");
    put_string_arg(args, argc, argv, "--method", "method");
    put_string_arg(args, argc, argv, "--status-class", "status_class");
    put_int_arg(args, argc, argv, "--status", "status");
    put_string_arg(args, argc, argv, "--request-id", "request_id");
    put_string_arg(args, argc, argv, "--request-body-contains", "request_body_contains");
    put_string_arg(args, argc, argv, "--response-body-contains", "response_body_contains");
    put_int_arg(args, argc, argv, "--after-ms", "after_ms");
    put_int_arg(args, argc, argv, "--since-ms", "since_ms");
    put_int_arg(args, argc, argv, "--timeout-ms", "timeout_ms");
    put_int_arg(args, argc, argv, "--quiet-ms", "quiet_ms");
}

int wait_command(int argc, char** argv) {
    json args;
    put_condition_options(args, argc, argv);
    if (!args.contains("condition") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "wait requires --condition <condition>"));
        return 0;
    }
    return command_alias_command(argc, argv, "wait", std::move(args));
}

int assert_command(int argc, char** argv) {
    json args;
    put_condition_options(args, argc, argv);
    if (!args.contains("condition") && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "assert requires --condition <condition>"));
        return 0;
    }
    return command_alias_command(argc, argv, "assert", std::move(args));
}

std::optional<json> parse_batch_steps_payload(const std::string& text,
                                              std::string& error) {
    auto parsed = parse_json(text);
    if (!parsed) {
        error = "batch expects a JSON steps array";
        return std::nullopt;
    }
    if (parsed->is_array()) return *parsed;
    if (parsed->is_object() && parsed->contains("steps") && (*parsed)["steps"].is_array()) {
        return (*parsed)["steps"];
    }
    error = "batch expects a JSON steps array";
    return std::nullopt;
}

std::optional<json> parse_batch_payload(const std::string& text,
                                        std::string& error) {
    auto parsed = parse_json(text);
    if (!parsed) {
        error = "batch expects a JSON steps array or object with steps";
        return std::nullopt;
    }
    if (parsed->is_array()) return json{{"steps", *parsed}};
    if (parsed->is_object() && parsed->contains("steps") && (*parsed)["steps"].is_array()) {
        return *parsed;
    }
    error = "batch expects a JSON steps array or object with steps";
    return std::nullopt;
}

int batch_command(int argc, char** argv) {
    json args;
    put_int_arg(args, argc, argv, "--watchdog-ms", "watchdog_ms");
    put_bool_flag(args, argc, argv, "--lifecycle", "lifecycle");
    if (has_arg(argc, argv, "--no-lifecycle")) args["lifecycle"] = false;

    std::optional<std::string> input;
    if (auto inline_stdin = find_arg(argc, argv, "--stdin-input-json")) {
        input = *inline_stdin;
    } else if (auto inline_input = find_arg(argc, argv, "--input-json")) {
        input = *inline_input;
    } else if (auto steps_file = find_arg(argc, argv, "--steps-file")) {
        std::string error;
        input = read_text_file(normalize_bridge_path(*steps_file), error);
        if (!input) {
            print_json(failure("file_read_failed", error));
            return 0;
        }
    } else if (!has_arg(argc, argv, "--args-json")) {
        input = read_stdin();
        if (input->find_first_not_of(" \t\r\n") == std::string::npos) {
            print_json(failure("invalid_request", "batch requires JSON steps on stdin, --stdin-input-json, --steps-file, or --args-json with steps"));
            return 0;
        }
    }

    if (input) {
        std::string error;
        auto payload = parse_batch_payload(*input, error);
        if (!payload) {
            print_json(failure("invalid_request", error));
            return 0;
        }
        for (auto& item : payload->items()) args[item.key()] = item.value();
    }

    if (!merge_args_json_or_print(argc, argv, args)) return 0;
    if (!args.contains("steps") || !args["steps"].is_array()) {
        print_json(failure("invalid_request", "batch requires a JSON steps array"));
        return 0;
    }
    print_json(command_envelope(parse_port(argc, argv),
                                arg_or_default(argc, argv, "--session", "acecode-default"),
                                "batch",
                                std::move(args)));
    return 0;
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

void put_target_options(json& args, int argc, char** argv) {
    if (auto target = find_arg(argc, argv, "--target")) args["selector"] = *target;
    put_string_arg(args, argc, argv, "--target-text", "target_text");
    put_string_arg(args, argc, argv, "--role", "role");
    put_string_arg(args, argc, argv, "--name", "name");
    put_string_arg(args, argc, argv, "--near-text", "near_text");
    put_int_arg(args, argc, argv, "--nth", "nth");
    put_bool_flag(args, argc, argv, "--exact", "exact");
    auto locator_text = find_arg(argc, argv, "--locator");
    if (!locator_text) locator_text = find_arg(argc, argv, "--locator-json");
    if (locator_text) {
        auto parsed = parse_json(*locator_text);
        if (!parsed || !parsed->is_object()) {
            args["locator"] = "__ACE_INVALID_JSON_OBJECT__";
        } else {
            args["locator"] = std::move(*parsed);
        }
    }
}

bool target_options_valid_or_print(const json& args) {
    if (args.value("locator", json()).is_string() &&
        args["locator"].get<std::string>() == "__ACE_INVALID_JSON_OBJECT__") {
        print_json(failure("invalid_request", "--locator must be a JSON object"));
        return false;
    }
    return true;
}

bool has_target_spec(const json& args) {
    return args.contains("selector") ||
           args.contains("target_text") ||
           args.contains("locator") ||
           args.contains("role") ||
           args.contains("name");
}

int click_command(int argc, char** argv) {
    json args;
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
    put_number_arg(args, argc, argv, "--x", "x");
    put_number_arg(args, argc, argv, "--y", "y");
    put_string_arg(args, argc, argv, "--button", "button");
    put_interaction_options(args, argc, argv);
    if (!has_target_spec(args) && !(args.contains("x") && args.contains("y")) &&
        !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "click requires --target <ref|selector>, --target-text <text>, --locator <json>, role/name, or --x/--y"));
        return 0;
    }
    return command_alias_command(argc, argv, "click", std::move(args));
}

int fill_command(int argc, char** argv) {
    auto value = find_arg(argc, argv, "--value");
    json args;
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
    if (value) args["value"] = *value;
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--snapshot-id", "snapshot_id");
    if ((!has_target_spec(args) || !value) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "fill requires a target locator and --value <text>"));
        return 0;
    }
    return command_alias_command(argc, argv, "fill", std::move(args));
}

int type_command(int argc, char** argv) {
    json args;
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
    put_string_arg(args, argc, argv, "--text", "text");
    put_bool_flag(args, argc, argv, "--clear", "clear");
    put_bool_flag(args, argc, argv, "--submit", "submit");
    put_string_arg(args, argc, argv, "--mode", "mode");
    put_string_arg(args, argc, argv, "--speed", "speed");
    if (!has_target_spec(args) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "type requires --target <ref|selector>, --target-text <text>, --locator <json>, or role/name"));
        return 0;
    }
    return command_alias_command(argc, argv, "type", std::move(args));
}

int hover_command(int argc, char** argv) {
    json args;
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
    put_interaction_options(args, argc, argv);
    if (!has_target_spec(args) && !has_arg(argc, argv, "--args-json")) {
        print_json(failure("invalid_request", "hover requires --target <ref|selector>, --target-text <text>, --locator <json>, or role/name"));
        return 0;
    }
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
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
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

// 解析 list-tabs 的 argv 为请求 args。--all 时让插件枚举浏览器里所有 tab(含点击弹出的 popup 新窗口),
// 而不只是本 session adopt 的那个 tab。提取为独立函数便于单元测试覆盖 flag 解析。
json list_tabs_args(int argc, char** argv) {
    json args = json::object();
    put_bool_flag(args, argc, argv, "--all", "all");
    return args;
}

int list_tabs_command(int argc, char** argv) {
    return command_alias_command(argc, argv, "list_tabs", list_tabs_args(argc, argv));
}

int close_session_command(int argc, char** argv) {
    return command_alias_command(argc, argv, "close_session", json::object());
}

int screenshot_command(int argc, char** argv) {
    auto output = find_arg(argc, argv, "--output");
    if (!output) {
        print_json(failure("invalid_request", "screenshot requires --output <path>"));
        return 0;
    }
    json args;
    args["output"] = normalize_bridge_path(*output).u8string();
    put_target_options(args, argc, argv);
    if (!target_options_valid_or_print(args)) return 0;
    put_string_arg(args, argc, argv, "--attachment-url", "attachment_url");
    put_string_arg(args, argc, argv, "--attachment-ref", "attachment_ref");
    if (!merge_args_json_or_print(argc, argv, args)) return 0;
    print_json(command_envelope(parse_port(argc, argv),
                                arg_or_default(argc, argv, "--session", "acecode-default"),
                                "screenshot",
                                std::move(args)));
    return 0;
}

struct ParsedWebSocketUrl {
    int port = 0;
    std::string path;
};

std::optional<ParsedWebSocketUrl> parse_loopback_ws_url(const std::string& url,
                                                        std::string& error) {
    const std::string prefix = "ws://127.0.0.1:";
    const std::string alt_prefix = "ws://localhost:";
    std::size_t offset = std::string::npos;
    if (url.rfind(prefix, 0) == 0) {
        offset = prefix.size();
    } else if (url.rfind(alt_prefix, 0) == 0) {
        offset = alt_prefix.size();
    } else {
        error = "direct CDP currently requires a ws://127.0.0.1:<port>/... URL";
        return std::nullopt;
    }
    const std::size_t slash = url.find('/', offset);
    if (slash == std::string::npos) {
        error = "CDP WebSocket URL is missing path";
        return std::nullopt;
    }
    int port = 0;
    try {
        port = std::stoi(url.substr(offset, slash - offset));
    } catch (...) {
        error = "CDP WebSocket URL has invalid port";
        return std::nullopt;
    }
    if (port <= 0 || port > 65535) {
        error = "CDP WebSocket URL port out of range";
        return std::nullopt;
    }
    return ParsedWebSocketUrl{port, url.substr(slash)};
}

void set_socket_timeouts(socket_t s, int timeout_ms) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool recv_exact(socket_t s, char* out, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        int n = recv(s, out + offset, static_cast<int>(size - offset), 0);
#else
        ssize_t n = recv(s, out + offset, size - offset, 0);
#endif
        if (n <= 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

std::string random_bytes(std::size_t size) {
    static std::mutex rng_mutex;
    static std::mt19937_64 rng{
        static_cast<std::mt19937_64::result_type>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
            static_cast<long long>(current_process_id()))};
    std::lock_guard<std::mutex> lock(rng_mutex);
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

std::string env_string(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

#ifndef _WIN32
std::optional<std::filesystem::path> find_executable_on_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::istringstream paths(path_env);
    std::string dir;
    while (std::getline(paths, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path p = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return std::nullopt;
}
#endif

std::optional<std::filesystem::path> find_chrome_executable() {
    for (const char* name : {"ACE_BROWSER_CHROME", "CHROME_PATH"}) {
        std::string value = env_string(name);
        if (!value.empty()) {
            std::filesystem::path p = std::filesystem::u8path(value);
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p;
        }
    }
#ifdef _WIN32
    std::vector<std::filesystem::path> candidates;
    if (auto local = env_w(L"LOCALAPPDATA")) {
        candidates.push_back(std::filesystem::path(*local) / L"Google\\Chrome\\Application\\chrome.exe");
        candidates.push_back(std::filesystem::path(*local) / L"BraveSoftware\\Brave-Browser\\Application\\brave.exe");
    }
    candidates.emplace_back(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
    candidates.emplace_back(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    candidates.emplace_back(L"C:\\Program Files\\BraveSoftware\\Brave-Browser\\Application\\brave.exe");
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
#else
#ifdef __APPLE__
    for (const char* path : {
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
             "/Applications/Chromium.app/Contents/MacOS/Chromium",
             "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"}) {
        std::filesystem::path p(path);
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
#endif
    for (const char* name : {"google-chrome", "google-chrome-stable", "chromium-browser", "chromium", "brave-browser", "brave-browser-stable"}) {
        if (auto found = find_executable_on_path(name)) return found;
    }
#endif
    return std::nullopt;
}

std::filesystem::path direct_cdp_profile_dir() {
    std::string configured = env_string("ACE_BROWSER_USER_DATA_DIR");
    if (!configured.empty()) return std::filesystem::u8path(configured);
    return acecode_user_data_dir() / "browser" / "chrome-profile";
}

std::optional<std::pair<int, std::string>> read_devtools_active_port_file(const std::filesystem::path& user_data_dir) {
    std::ifstream in(user_data_dir / "DevToolsActivePort", std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::string port_line;
    std::string path_line;
    std::getline(in, port_line);
    std::getline(in, path_line);
    if (port_line.empty()) return std::nullopt;
    int port = 0;
    try {
        port = std::stoi(port_line);
    } catch (...) {
        return std::nullopt;
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    if (path_line.empty()) path_line = "/devtools/browser";
    return std::make_pair(port, path_line);
}

class DirectCdpBackend {
  public:
    DirectCdpBackend() = default;
    ~DirectCdpBackend() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_ws_locked();
        close_process_locked();
    }

    bool ensure(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_ && verify_connection_locked()) {
            error.clear();
            return true;
        }
        close_ws_locked();
        ready_ = false;

        profile_dir_ = direct_cdp_profile_dir();
        std::error_code ec;
        std::filesystem::create_directories(profile_dir_, ec);
        if (ec) {
            error = "failed to create Chrome profile directory: " + ec.message();
            last_error_ = error;
            return false;
        }

        if (connect_from_devtools_file_locked(error)) {
            if (initialize_default_page_locked(error)) return true;
            close_ws_locked();
        }

        if (!launch_chrome_locked(error)) {
            last_error_ = error;
            return false;
        }
        if (!wait_for_devtools_locked(error)) {
            last_error_ = error;
            return false;
        }
        if (!initialize_default_page_locked(error)) {
            last_error_ = error;
            return false;
        }
        last_error_.clear();
        return true;
    }

    json status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return json{
            {"ready", ready_},
            {"ws_url", ws_url_.empty() ? json(nullptr) : json(ws_url_)},
            {"debug_port", debug_port_ > 0 ? json(debug_port_) : json(nullptr)},
            {"profile_dir", profile_dir_.empty() ? json(nullptr) : json(profile_dir_.u8string())},
            {"chrome_path", chrome_path_.empty() ? json(nullptr) : json(chrome_path_.u8string())},
            {"chrome_pid", chrome_pid_ > 0 ? json(chrome_pid_) : json(nullptr)},
            {"launched_by_host", launched_by_host_},
            {"page_sessions", pages_.size()},
            {"last_error", last_error_.empty() ? json(nullptr) : json(last_error_)},
        };
    }

#ifdef ACE_BROWSER_HOST_NO_MAIN
    void test_mark_ready(const std::string& ws_url = "ws://127.0.0.1:1/devtools/browser/test") {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
        ws_url_ = ws_url;
        debug_port_ = 1;
        profile_dir_ = std::filesystem::temp_directory_path() / "ace-browser-host-test-profile";
        chrome_path_ = std::filesystem::path("chrome-test");
        chrome_pid_ = 1234;
        launched_by_host_ = true;
    }
#endif

    json execute(const std::string& session, const std::string& action, const json& args) {
        std::string error;
        if (!ensure(error)) {
            return failure("direct_cdp_unavailable", error.empty() ? "direct CDP backend is not ready" : error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            if (action == "raw_cdp") return execute_raw_cdp_locked(session, args);
            if (action == "navigate") return execute_navigate_locked(session, args);
            if (action == "evaluate") return execute_evaluate_locked(session, args);
            if (action == "snapshot") return execute_snapshot_locked(session, args);
            if (action == "click") return execute_dom_action_locked(session, action, args);
            if (action == "fill" || action == "type") return execute_dom_action_locked(session, action, args);
            if (action == "list_tabs") return execute_list_tabs_locked();
            if (action == "find_tab") return execute_find_tab_locked(args);
            if (action == "close_session") return execute_close_session_locked(session);
            return failure("direct_action_unsupported", "direct CDP backend does not implement action: " + action);
        } catch (const std::exception& e) {
            last_error_ = e.what();
            return failure("direct_cdp_error", safe_error_message(e.what()));
        }
    }

  private:
    struct PageSession {
        std::string target_id;
        std::string session_id;
        std::string url;
        std::string title;
    };

    std::mutex mutex_;
    socket_t ws_ = kInvalidSocket;
    bool ready_ = false;
    bool launched_by_host_ = false;
    int debug_port_ = 0;
    unsigned long chrome_pid_ = 0;
    uint64_t next_command_id_ = 1;
    std::filesystem::path profile_dir_;
    std::filesystem::path chrome_path_;
    std::string ws_url_;
    std::string last_error_;
    std::unordered_map<std::string, PageSession> pages_;
#ifdef _WIN32
    HANDLE chrome_process_ = nullptr;
#else
    pid_t chrome_process_ = -1;
#endif

    void close_ws_locked() {
        if (ws_ != kInvalidSocket) {
            close_socket(ws_);
            ws_ = kInvalidSocket;
        }
        ready_ = false;
    }

    void close_process_locked() {
#ifdef _WIN32
        if (chrome_process_) {
            DWORD code = 0;
            if (GetExitCodeProcess(chrome_process_, &code) && code == STILL_ACTIVE) {
                TerminateProcess(chrome_process_, 0);
                WaitForSingleObject(chrome_process_, 2000);
            }
            CloseHandle(chrome_process_);
            chrome_process_ = nullptr;
        }
#else
        if (chrome_process_ > 0) {
            kill(chrome_process_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                pid_t r = waitpid(chrome_process_, &status, WNOHANG);
                if (r == chrome_process_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(chrome_process_, SIGKILL);
            int status = 0;
            waitpid(chrome_process_, &status, 0);
            chrome_process_ = -1;
        }
#endif
    }

    bool connect_from_devtools_file_locked(std::string& error) {
        auto active = read_devtools_active_port_file(profile_dir_);
        if (!active) return false;
        const std::string url = "ws://127.0.0.1:" + std::to_string(active->first) + active->second;
        if (!connect_ws_locked(url, error)) {
            std::error_code ec;
            std::filesystem::remove(profile_dir_ / "DevToolsActivePort", ec);
            return false;
        }
        debug_port_ = active->first;
        ws_url_ = url;
        launched_by_host_ = false;
        ready_ = true;
        return true;
    }

    bool launch_chrome_locked(std::string& error) {
        auto chrome = find_chrome_executable();
        if (!chrome) {
            error = "Chrome executable not found; set ACE_BROWSER_CHROME to chrome.exe";
            return false;
        }
        chrome_path_ = *chrome;
        std::error_code ec;
        std::filesystem::remove(profile_dir_ / "DevToolsActivePort", ec);

        std::vector<std::string> args = {
            "--remote-debugging-port=0",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-background-networking",
            "--disable-backgrounding-occluded-windows",
            "--disable-component-update",
            "--disable-default-apps",
            "--disable-popup-blocking",
            "--disable-sync",
            "--disable-features=Translate",
            "--user-data-dir=" + profile_dir_.u8string(),
            "about:blank",
        };
#ifdef _WIN32
        std::wstring command_line = quote_windows_arg(chrome_path_);
        for (const auto& arg : args) {
            auto wide = utf8_to_wide(arg);
            if (!wide) {
                error = "Chrome argument is not valid UTF-8";
                return false;
            }
            command_line += L" " + quote_windows_command_arg(*wide);
        }
        std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr,
                                 mutable_command.data(),
                                 nullptr,
                                 nullptr,
                                 FALSE,
                                 CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                                 nullptr,
                                 nullptr,
                                 &si,
                                 &pi);
        if (!ok) {
            error = "failed to launch Chrome, win32_error=" +
                std::to_string(static_cast<unsigned long>(GetLastError()));
            return false;
        }
        CloseHandle(pi.hThread);
        chrome_process_ = pi.hProcess;
        chrome_pid_ = static_cast<unsigned long>(pi.dwProcessId);
#else
        pid_t pid = fork();
        if (pid < 0) {
            error = "fork failed launching Chrome";
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
            std::vector<std::string> child_args;
            child_args.push_back(chrome_path_.string());
            child_args.insert(child_args.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (auto& item : child_args) argv.push_back(item.data());
            argv.push_back(nullptr);
            execv(chrome_path_.string().c_str(), argv.data());
            _exit(127);
        }
        chrome_process_ = pid;
        chrome_pid_ = static_cast<unsigned long>(pid);
#endif
        launched_by_host_ = true;
        host_log_info("[direct-cdp] launched Chrome pid=" + std::to_string(chrome_pid_) +
                      " profile=" + profile_dir_.u8string());
        return true;
    }

    bool wait_for_devtools_locked(std::string& error) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() <= deadline) {
            if (auto active = read_devtools_active_port_file(profile_dir_)) {
                const std::string url = "ws://127.0.0.1:" + std::to_string(active->first) + active->second;
                if (connect_ws_locked(url, error)) {
                    debug_port_ = active->first;
                    ws_url_ = url;
                    ready_ = true;
                    host_log_info("[direct-cdp] connected ws_url=" + ws_url_);
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        error = "timeout waiting for Chrome DevToolsActivePort";
        return false;
    }

    bool connect_ws_locked(const std::string& url, std::string& error) {
        auto parsed = parse_loopback_ws_url(url, error);
        if (!parsed) return false;
        socket_t s = connect_loopback(parsed->port);
        if (s == kInvalidSocket) {
            error = "failed to connect to Chrome debug port " + std::to_string(parsed->port);
            return false;
        }
        set_socket_timeouts(s, 5000);
        const std::string key = encode_base64(random_bytes(16));
        std::ostringstream req;
        req << "GET " << parsed->path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1:" << parsed->port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!send_all(s, req.str())) {
            close_socket(s);
            error = "failed to send CDP WebSocket handshake";
            return false;
        }
        std::string raw;
        char buffer[1024];
        while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
#ifdef _WIN32
            int n = recv(s, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
            ssize_t n = recv(s, buffer, sizeof(buffer), 0);
#endif
            if (n <= 0) {
                close_socket(s);
                error = "failed to read CDP WebSocket handshake";
                return false;
            }
            raw.append(buffer, static_cast<std::size_t>(n));
        }
        if (raw.find(" 101 ") == std::string::npos && raw.find(" 101\r\n") == std::string::npos) {
            close_socket(s);
            error = "Chrome rejected CDP WebSocket handshake";
            return false;
        }
        ws_ = s;
        return true;
    }

    bool send_ws_frame_locked(unsigned char opcode, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
        const std::size_t len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(0x80 | len));
        } else if (len <= 0xFFFF) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
            }
        }
        const std::string mask = random_bytes(4);
        frame.append(mask);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        return send_all(ws_, frame);
    }

    std::optional<std::string> read_ws_text_locked(std::string& error) {
        std::string fragmented;
        for (;;) {
            char header[2];
            if (!recv_exact(ws_, header, 2)) {
                error = "failed to read CDP WebSocket frame";
                return std::nullopt;
            }
            const unsigned char b0 = static_cast<unsigned char>(header[0]);
            const unsigned char b1 = static_cast<unsigned char>(header[1]);
            const bool fin = (b0 & 0x80) != 0;
            const unsigned char opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t len = b1 & 0x7F;
            if (len == 126) {
                char ext[2];
                if (!recv_exact(ws_, ext, 2)) {
                    error = "failed to read CDP WebSocket frame length";
                    return std::nullopt;
                }
                len = (static_cast<unsigned char>(ext[0]) << 8) | static_cast<unsigned char>(ext[1]);
            } else if (len == 127) {
                char ext[8];
                if (!recv_exact(ws_, ext, 8)) {
                    error = "failed to read CDP WebSocket frame length";
                    return std::nullopt;
                }
                len = 0;
                for (char ch : ext) len = (len << 8) | static_cast<unsigned char>(ch);
            }
            if (len > 64ull * 1024ull * 1024ull) {
                error = "CDP WebSocket frame too large";
                return std::nullopt;
            }
            char mask[4] = {};
            if (masked && !recv_exact(ws_, mask, 4)) {
                error = "failed to read CDP WebSocket mask";
                return std::nullopt;
            }
            std::string payload(static_cast<std::size_t>(len), '\0');
            if (len > 0 && !recv_exact(ws_, payload.data(), payload.size())) {
                error = "failed to read CDP WebSocket payload";
                return std::nullopt;
            }
            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
            }
            if (opcode == 0x8) {
                error = "CDP WebSocket closed";
                return std::nullopt;
            }
            if (opcode == 0x9) {
                send_ws_frame_locked(0xA, payload);
                continue;
            }
            if (opcode == 0xA) continue;
            if (opcode == 0x1 || opcode == 0x0) {
                fragmented += payload;
                if (fin) return fragmented;
                continue;
            }
        }
    }

    json send_command_locked(const std::string& method,
                             const json& params,
                             const std::optional<std::string>& session_id,
                             std::string& error) {
        if (ws_ == kInvalidSocket) {
            error = "CDP WebSocket is not connected";
            return json();
        }
        const uint64_t id = next_command_id_++;
        json request = {
            {"id", id},
            {"method", method},
        };
        if (!params.is_null()) request["params"] = params;
        if (session_id && !session_id->empty()) request["sessionId"] = *session_id;
        if (!send_ws_frame_locked(0x1, request.dump())) {
            error = "failed to send CDP command";
            close_ws_locked();
            return json();
        }
        for (;;) {
            auto text = read_ws_text_locked(error);
            if (!text) {
                close_ws_locked();
                return json();
            }
            auto parsed = parse_json(*text);
            if (!parsed || !parsed->is_object()) continue;
            if (parsed->value("id", 0ull) == id) {
                if (parsed->contains("error")) {
                    error = parsed->dump();
                } else {
                    error.clear();
                }
                return *parsed;
            }
        }
    }

    bool verify_connection_locked() {
        std::string error;
        json response = send_command_locked("Browser.getVersion", json::object(), std::nullopt, error);
        return response.is_object() && response.contains("result") && error.empty();
    }

    bool initialize_default_page_locked(std::string& error) {
        if (!ensure_page_locked("acecode-default", false, error)) return false;
        ready_ = true;
        return true;
    }

    bool ensure_page_locked(const std::string& session,
                            bool force_new,
                            std::string& error) {
        const std::string key = session.empty() ? "acecode-default" : session;
        if (!force_new) {
            auto existing = pages_.find(key);
            if (existing != pages_.end() && !existing->second.session_id.empty()) return true;
        }
        json create = send_command_locked(
            "Target.createTarget",
            json{{"url", "about:blank"}},
            std::nullopt,
            error);
        if (!error.empty() || !create.contains("result") || !create["result"].contains("targetId")) {
            if (error.empty()) error = "Target.createTarget returned no targetId";
            return false;
        }
        const std::string target_id = create["result"]["targetId"].get<std::string>();
        json attach = send_command_locked(
            "Target.attachToTarget",
            json{{"targetId", target_id}, {"flatten", true}},
            std::nullopt,
            error);
        if (!error.empty() || !attach.contains("result") || !attach["result"].contains("sessionId")) {
            if (error.empty()) error = "Target.attachToTarget returned no sessionId";
            return false;
        }
        const std::string session_id = attach["result"]["sessionId"].get<std::string>();
        send_command_locked("Page.enable", json::object(), session_id, error);
        if (!error.empty()) return false;
        send_command_locked("Runtime.enable", json::object(), session_id, error);
        if (!error.empty()) return false;
        error.clear();
        send_command_locked("Runtime.runIfWaitingForDebugger", json::object(), session_id, error);
        error.clear();
        send_command_locked("Network.enable", json::object(), session_id, error);
        error.clear();
        pages_[key] = PageSession{target_id, session_id, "about:blank", ""};
        return true;
    }

    std::optional<PageSession> page_for_session_locked(const std::string& session,
                                                       bool force_new,
                                                       std::string& error) {
        const std::string key = session.empty() ? "acecode-default" : session;
        if (!ensure_page_locked(key, force_new, error)) return std::nullopt;
        return pages_[key];
    }

    static bool method_is_browser_level(const std::string& method) {
        return method.rfind("Browser.", 0) == 0 ||
               method.rfind("Target.", 0) == 0 ||
               method.rfind("SystemInfo.", 0) == 0;
    }

    json execute_raw_cdp_locked(const std::string& session, const json& args) {
        const std::string method = args.value("method", "");
        if (method.empty()) return failure("invalid_request", "raw_cdp requires method");
        const json params = args.contains("params") && args["params"].is_object()
            ? args["params"]
            : json::object();
        std::string error;
        std::optional<std::string> cdp_session;
        if (!method_is_browser_level(method)) {
            auto page = page_for_session_locked(session, false, error);
            if (!page) return failure("direct_cdp_error", error);
            cdp_session = page->session_id;
        }
        json response = send_command_locked(method, params, cdp_session, error);
        if (!error.empty() && response.contains("error")) {
            return failure("cdp_error", response["error"].dump());
        }
        if (!error.empty()) return failure("direct_cdp_error", error);
        json data = {
            {"backend", "direct_cdp"},
            {"method", method},
            {"result", response.value("result", json::object())},
        };
        return success(std::move(data));
    }

    json runtime_evaluate_locked(const PageSession& page,
                                 const std::string& expression,
                                 bool return_by_value,
                                 std::string& error) {
        json response = send_command_locked(
            "Runtime.evaluate",
            json{{"expression", expression}, {"awaitPromise", true}, {"returnByValue", return_by_value}},
            page.session_id,
            error);
        if (!error.empty()) return response;
        if (response.contains("result") && response["result"].contains("exceptionDetails")) {
            error = response["result"]["exceptionDetails"].dump();
        }
        return response;
    }

    json current_page_summary_locked(const PageSession& page) {
        std::string error;
        json url_response = runtime_evaluate_locked(page, "location.href", true, error);
        std::string url;
        if (error.empty()) {
            url = url_response["result"]["result"].value("value", "");
        }
        error.clear();
        json title_response = runtime_evaluate_locked(page, "document.title", true, error);
        std::string title;
        if (error.empty()) {
            title = title_response["result"]["result"].value("value", "");
        }
        return json{{"url", url}, {"title", title}};
    }

    json execute_navigate_locked(const std::string& session, const json& args) {
        std::string error;
        const bool force_new = args.value("newTab", false) || args.value("new_tab", false);
        auto page = page_for_session_locked(session, force_new, error);
        if (!page) return failure("direct_cdp_error", error);
        const std::string url = args.value("url", "");
        const std::string operation = args.value("operation", url.empty() ? "" : "goto");
        if (operation == "goto") {
            if (url.empty()) return failure("invalid_request", "navigate goto requires url");
            json response = send_command_locked("Page.navigate", json{{"url", url}}, page->session_id, error);
            if (!error.empty()) return failure("cdp_error", error);
            (void)response;
        } else if (operation == "reload") {
            send_command_locked("Page.reload", json::object(), page->session_id, error);
            if (!error.empty()) return failure("cdp_error", error);
        } else if (operation == "back" || operation == "forward") {
            const std::string script = operation == "back" ? "history.back(); true" : "history.forward(); true";
            runtime_evaluate_locked(*page, script, true, error);
            if (!error.empty()) return failure("cdp_error", error);
        } else {
            return failure("invalid_request", "unsupported navigate operation: " + operation);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(1500, args.value("timeout_ms", 750))));
        json summary = current_page_summary_locked(*page);
        auto it = pages_.find(session.empty() ? "acecode-default" : session);
        if (it != pages_.end()) {
            it->second.url = summary.value("url", url);
            it->second.title = summary.value("title", "");
        }
        summary["backend"] = "direct_cdp";
        summary["success"] = true;
        return success(std::move(summary));
    }

    json execute_evaluate_locked(const std::string& session, const json& args) {
        const std::string code = args.value("code", "");
        if (code.empty()) return failure("invalid_request", "evaluate requires code");
        std::string error;
        auto page = page_for_session_locked(session, false, error);
        if (!page) return failure("direct_cdp_error", error);
        json response = runtime_evaluate_locked(*page, code, true, error);
        if (!error.empty()) return failure("cdp_error", error);
        json value = response["result"]["result"].value("value", json(nullptr));
        return success({{"backend", "direct_cdp"}, {"value", value}, {"result", value}});
    }

    static std::string snapshot_script(const json& args) {
        (void)args;
        return R"JS((() => {
  const textOf = (el) => (el.innerText || el.value || el.getAttribute('aria-label') || el.getAttribute('title') || '').trim();
  const roleOf = (el) => el.getAttribute('role') || ({A:'link',BUTTON:'button',INPUT:'textbox',TEXTAREA:'textbox',SELECT:'combobox'}[el.tagName] || el.tagName.toLowerCase());
  const visible = (el) => {
    const r = el.getBoundingClientRect();
    const s = getComputedStyle(el);
    return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none';
  };
  const selectorFor = (el) => {
    if (el.id) return '#' + CSS.escape(el.id);
    const name = el.getAttribute('name');
    if (name) return el.tagName.toLowerCase() + '[name="' + CSS.escape(name) + '"]';
    return el.tagName.toLowerCase();
  };
  const nodes = Array.from(document.querySelectorAll('a,button,input,textarea,select,[role],[onclick],[tabindex],summary,label'))
    .filter(visible)
    .slice(0, 250);
  window.__aceBrowserRefs = nodes;
  const elements = nodes.map((el, idx) => {
    const r = el.getBoundingClientRect();
    return {
      ref: '@e' + (idx + 1),
      tag: el.tagName.toLowerCase(),
      role: roleOf(el),
      name: (el.getAttribute('aria-label') || el.getAttribute('title') || textOf(el)).slice(0, 160),
      text: textOf(el).slice(0, 240),
      value: 'value' in el ? el.value : undefined,
      disabled: !!el.disabled || el.getAttribute('aria-disabled') === 'true',
      actionable: true,
      stable_selector: selectorFor(el),
      rect: { x: Math.round(r.x), y: Math.round(r.y), width: Math.round(r.width), height: Math.round(r.height) }
    };
  });
  const active = document.activeElement;
  return {
    url: location.href,
    title: document.title,
    mode: 'summary',
    text: (document.body ? document.body.innerText : '').slice(0, 30000),
    elements,
    focused: active ? { tag: active.tagName.toLowerCase(), text: textOf(active).slice(0, 160) } : null,
    viewport: { width: innerWidth, height: innerHeight, device_scale_factor: devicePixelRatio }
  };
})())JS";
    }

    json execute_snapshot_locked(const std::string& session, const json& args) {
        std::string error;
        auto page = page_for_session_locked(session, false, error);
        if (!page) return failure("direct_cdp_error", error);
        json response = runtime_evaluate_locked(*page, snapshot_script(args), true, error);
        if (!error.empty()) return failure("cdp_error", error);
        json value = response["result"]["result"].value("value", json::object());
        if (!value.is_object()) value = json::object({{"value", value}});
        value["backend"] = "direct_cdp";
        value["session"] = session.empty() ? "acecode-default" : session;
        return success(std::move(value));
    }

    static std::string dom_action_script(const std::string& action, const json& args) {
        json payload = args.is_object() ? args : json::object();
        payload["__action"] = action;
        const std::string literal = payload.dump();
        return "(() => {\n"
               "  const args = " + literal + ";\n"
               "  const textOf = (el) => (el.innerText || el.value || el.getAttribute('aria-label') || el.getAttribute('title') || '').trim();\n"
               "  const norm = (s) => String(s || '').trim().toLowerCase();\n"
               "  const roleOf = (el) => el.getAttribute('role') || ({A:'link',BUTTON:'button',INPUT:'textbox',TEXTAREA:'textbox',SELECT:'combobox'}[el.tagName] || el.tagName.toLowerCase());\n"
               "  const visible = (el) => { const r = el.getBoundingClientRect(); const s = getComputedStyle(el); return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none'; };\n"
               "  function byRef(ref) { const m = /^@e(\\d+)$/.exec(ref || ''); return m && window.__aceBrowserRefs ? window.__aceBrowserRefs[Number(m[1]) - 1] : null; }\n"
               "  function findTarget() {\n"
               "    if (args.selector && String(args.selector).startsWith('@e')) return byRef(args.selector);\n"
               "    if (args.target && String(args.target).startsWith('@e')) return byRef(args.target);\n"
               "    if (args.selector) { try { return document.querySelector(args.selector); } catch (_) {} }\n"
               "    const wantedText = args.target_text || args.name || (args.locator && args.locator.name);\n"
               "    const wantedRole = args.role || (args.locator && args.locator.role);\n"
               "    const nodes = Array.from(document.querySelectorAll('a,button,input,textarea,select,[role],[onclick],[tabindex],summary,label')).filter(visible);\n"
               "    return nodes.find((el) => (!wantedRole || norm(roleOf(el)) === norm(wantedRole)) && (!wantedText || norm(textOf(el)).includes(norm(wantedText))));\n"
               "  }\n"
               "  const el = findTarget();\n"
               "  if (!el) return { ok:false, code:'target_not_found', message:'Direct CDP could not resolve target' };\n"
               "  el.scrollIntoView({ block:'center', inline:'center' });\n"
               "  if (args.__action === 'click') { el.click(); return { ok:true, action:'click', text:textOf(el).slice(0,160) }; }\n"
               "  const value = args.value !== undefined ? String(args.value) : String(args.text || '');\n"
               "  if ('value' in el) { if (args.clear !== false) el.value = ''; el.focus(); el.value = value; el.dispatchEvent(new InputEvent('input', { bubbles:true, inputType:'insertText', data:value })); el.dispatchEvent(new Event('change', { bubbles:true })); }\n"
               "  else { el.textContent = value; el.dispatchEvent(new Event('input', { bubbles:true })); }\n"
               "  if (args.submit) { const form = el.form || el.closest('form'); if (form) form.requestSubmit ? form.requestSubmit() : form.submit(); }\n"
               "  return { ok:true, action:args.__action, value };\n"
               "})()";
    }

    json execute_dom_action_locked(const std::string& session, const std::string& action, const json& args) {
        std::string error;
        auto page = page_for_session_locked(session, false, error);
        if (!page) return failure("direct_cdp_error", error);
        json response = runtime_evaluate_locked(*page, dom_action_script(action, args), true, error);
        if (!error.empty()) return failure("cdp_error", error);
        json value = response["result"]["result"].value("value", json::object());
        if (value.is_object() && value.value("ok", true) == false) {
            return failure(value.value("code", "direct_dom_action_failed"),
                           value.value("message", "direct DOM action failed"));
        }
        return success({{"backend", "direct_cdp"}, {"success", true}, {"result", value}});
    }

    json execute_list_tabs_locked() {
        std::string error;
        json response = send_command_locked("Target.getTargets", json::object(), std::nullopt, error);
        if (!error.empty()) return failure("cdp_error", error);
        json tabs = json::array();
        for (const auto& target : response["result"].value("targetInfos", json::array())) {
            if (!target.is_object()) continue;
            const std::string type = target.value("type", target.value("target_type", ""));
            if (type != "page" && type != "webview") continue;
            tabs.push_back({
                {"target_id", target.value("targetId", "")},
                {"url", target.value("url", "")},
                {"title", target.value("title", "")},
                {"type", type},
            });
        }
        return success({{"backend", "direct_cdp"}, {"tabs", tabs}});
    }

    json execute_find_tab_locked(const json& args) {
        json tabs_envelope = execute_list_tabs_locked();
        if (!tabs_envelope.value("ok", false)) return tabs_envelope;
        const std::string needle = args.value("url", "");
        if (needle.empty()) return success({{"backend", "direct_cdp"}, {"found", false}});
        for (const auto& tab : tabs_envelope["data"]["tabs"]) {
            if (tab.value("url", "").find(needle) != std::string::npos) {
                return success({{"backend", "direct_cdp"}, {"found", true}, {"tab", tab}});
            }
        }
        return success({{"backend", "direct_cdp"}, {"found", false}});
    }

    json execute_close_session_locked(const std::string& session) {
        const std::string key = session.empty() ? "acecode-default" : session;
        auto it = pages_.find(key);
        if (it == pages_.end()) return success({{"backend", "direct_cdp"}, {"closed", false}});
        std::string error;
        send_command_locked("Target.closeTarget", json{{"targetId", it->second.target_id}}, std::nullopt, error);
        pages_.erase(it);
        return success({{"backend", "direct_cdp"}, {"closed", true}});
    }
};

bool direct_cdp_action_supported(const std::string& action) {
    return action == "raw_cdp" ||
           action == "navigate" ||
           action == "evaluate" ||
           action == "snapshot" ||
           action == "click" ||
           action == "fill" ||
           action == "type" ||
           action == "list_tabs" ||
           action == "find_tab" ||
           action == "close_session";
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
        bool in_queue = false;                                  // 是否仍排在 queued_actions 待派发
        bool acked = false;                                     // 插件是否已确认收到本指令(重投判定的关键信号)
        int dispatch_count = 0;                                 // 已派发次数(诊断 / 日志)
        std::chrono::steady_clock::time_point dispatched_at{};  // 最近一次派发时刻(重投窗口起点)
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
    std::unique_ptr<DirectCdpBackend> direct_cdp = std::make_unique<DirectCdpBackend>();
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
    json direct_status = state.direct_cdp ? state.direct_cdp->status() : json{{"ready", false}};
    const bool direct_ready = direct_status.value("ready", false);
    std::lock_guard<std::mutex> lock(state.mutex);
    const long long last_seen_ms = extension_last_seen_ms_locked(state);
    const bool has_seen = last_seen_ms >= 0;
    const bool fresh = extension_fresh_locked(state);
    const bool stale = state.extension_connected && !fresh;
    const bool extension_ready = fresh && state.version_compatible;
    const bool ready = direct_ready || extension_ready;
    const std::string backend = direct_ready ? "direct_cdp" : (extension_ready ? "extension" : "none");
    json capabilities = direct_ready ? direct_cdp_capabilities(true) : default_capabilities(false);
    if (fresh) capabilities = merge_capabilities(std::move(capabilities), state.capabilities);
    return json{
        {"running", true},
        {"ready", ready},
        {"backend", backend},
        {"direct_cdp_ready", direct_ready},
        {"direct_cdp", direct_status},
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
        {"capabilities", capabilities},
        {"queued_actions", state.queued_actions.size()},
        {"pending_actions", state.pending_actions.size()},
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
    std::string extension_version;
    std::string protocol_version;
    std::string browser;
    bool compatible = true;
    std::string version_error;
    {
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
        extension_version = state.extension_version;
        protocol_version = state.protocol_version;
        browser = state.browser;
        compatible = state.version_compatible;
        version_error = state.version_error;
    }
    state.cv.notify_all();
    if (compatible) {
        host_log_info("[daemon] plugin_hello extension_version=" + extension_version +
                      " protocol_version=" + protocol_version +
                      " browser=" + browser);
    } else {
        host_log_warn("[daemon] plugin_hello version_mismatch extension_version=" + extension_version +
                      " protocol_version=" + protocol_version +
                      " error=" + version_error);
    }
    return success({{"success", true}});
}

json handle_direct_ensure(DaemonState& state, int port) {
    std::string error;
    if (!state.direct_cdp || !state.direct_cdp->ensure(error)) {
        host_log_warn("[daemon] direct_cdp ensure failed port=" + std::to_string(port) +
                      " error=" + host_log_truncate(error));
        json status = daemon_status_payload(state, port);
        status["direct_cdp_error"] = error.empty() ? "direct CDP backend is not ready" : error;
        return success(std::move(status));
    }
    host_log_info("[daemon] direct_cdp ensure ready port=" + std::to_string(port));
    return success(daemon_status_payload(state, port));
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

    const std::string session = action_session_from_request(*parsed);
    const std::string action = action_name_from_request(*parsed);
    // 命令等待预算:请求可带 command_timeout_ms(由 host CLI 为 batch / 长 wait 算出),
    // clamp 到 [默认 30s, 上限],缺省回落默认值。让长操作不被固定 30s 过早判 bridge_timeout。
    const long long requested_command_timeout =
        parsed->value("command_timeout_ms", static_cast<long long>(kCommandTimeoutMs));
    const long long command_timeout_ms = std::clamp<long long>(
        requested_command_timeout, kCommandTimeoutMs, kMaxCommandTimeoutMs);
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.shutting_down) {
            host_log_warn("[daemon] command rejected action=" + action +
                          " session=" + session +
                          " reason=daemon_shutting_down");
            return failure("daemon_shutting_down", "ace-browser-host daemon is shutting down");
        }
    }
    if (direct_cdp_action_supported(action) && state.direct_cdp) {
        json direct = state.direct_cdp->execute(session, action, (*parsed)["args"]);
        const std::string direct_error = envelope_error_code(direct);
        if (direct.value("ok", false) || direct_error != "direct_cdp_unavailable") {
            host_log_info("[daemon] command direct_cdp finish action=" + action +
                          " session=" + session +
                          " ok=" + std::string(direct.value("ok", false) ? "true" : "false") +
                          " duration_ms=" + std::to_string(elapsed_ms_since(started)));
            return direct;
        }
        host_log_warn("[daemon] direct_cdp unavailable, trying extension fallback action=" + action +
                      " session=" + session +
                      " error=" + host_log_truncate(direct.dump()));
    }
    std::shared_ptr<DaemonState::PendingAction> pending;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.shutting_down) {
            host_log_warn("[daemon] command rejected action=" + action +
                          " session=" + session +
                          " reason=daemon_shutting_down");
            return failure("daemon_shutting_down", "ace-browser-host daemon is shutting down");
        }
        if (!state.extension_connected) {
            host_log_warn("[daemon] command rejected action=" + action +
                          " session=" + session +
                          " reason=extension_not_connected");
            return failure("extension_not_connected", "ace-browser-bridge browser plugin is not connected");
        }
        if (!extension_fresh_locked(state)) {
            host_log_warn("[daemon] command rejected action=" + action +
                          " session=" + session +
                          " reason=extension_stale last_seen_ms=" +
                          std::to_string(extension_last_seen_ms_locked(state)));
            return failure("extension_stale", "ace-browser-bridge browser plugin connection is stale");
        }
        if (!state.version_compatible) {
            host_log_warn("[daemon] command rejected action=" + action +
                          " session=" + session +
                          " reason=version_mismatch");
            return failure("version_mismatch", state.version_error.empty()
                ? "ace-browser-host and ace-browser-bridge protocol versions are not compatible"
                : state.version_error);
        }
        pending = std::make_shared<DaemonState::PendingAction>();
        pending->id = "act_" + std::to_string(state.next_action_id++);
        pending->request = *parsed;
        pending->request["id"] = pending->id;
        pending->in_queue = true;
        state.queued_actions.push_back(pending);
        state.pending_actions[pending->id] = pending;
        host_log_info("[daemon] command queued id=" + pending->id +
                      " action=" + action +
                      " session=" + session +
                      " queued=" + std::to_string(state.queued_actions.size()) +
                      " pending=" + std::to_string(state.pending_actions.size()));
    }
    state.cv.notify_all();

    std::unique_lock<std::mutex> lock(state.mutex);
    // 等待插件回结果,期间做"未确认即重投":指令一旦被某次 poll 取走(in_queue=false)却迟迟
    // 没回 ack,基本可判定接走它的 service worker 已被浏览器回收、这次投递丢了,于是把指令重新
    // 放回队首让下一次 poll 再取一次,直到拿到结果或总超时为止。这样即便插件在派发瞬间被回收,
    // 指令也不会石沉大海干等满 30 秒。
    const auto overall_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(command_timeout_ms);
    while (!pending->result && !state.shutting_down &&
           std::chrono::steady_clock::now() < overall_deadline) {
        state.cv.wait_for(lock, std::chrono::milliseconds(500), [&]() {
            return pending->result.has_value() || state.shutting_down;
        });
        if (pending->result || state.shutting_down) break;
        const bool dispatched_but_unacked =
            !pending->in_queue && !pending->acked && pending->dispatch_count > 0;
        if (dispatched_but_unacked) {
            const long long since_dispatch =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - pending->dispatched_at).count();
            if (since_dispatch >= kRedeliveryAfterMs) {
                pending->in_queue = true;
                state.queued_actions.push_front(pending);
                host_log_warn("[daemon] command redeliver id=" + pending->id +
                              " action=" + action +
                              " session=" + session +
                              " attempt=" + std::to_string(pending->dispatch_count) +
                              " since_dispatch_ms=" + std::to_string(since_dispatch));
                state.cv.notify_all();
            }
        }
    }
    const bool had_result = pending->result.has_value();
    state.pending_actions.erase(pending->id);
    // 无论成功/超时/关停,都把自己从待派发队列摘掉,避免超时后指令仍被某次 poll 取走、
    // 回了个无人认领的结果(unknown_action)。
    state.queued_actions.erase(
        std::remove(state.queued_actions.begin(), state.queued_actions.end(), pending),
        state.queued_actions.end());
    if (state.shutting_down && !had_result) {
        host_log_warn("[daemon] command finish id=" + pending->id +
                      " action=" + action +
                      " session=" + session +
                      " ok=false error_code=daemon_shutting_down" +
                      " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return failure("daemon_shutting_down", "ace-browser-host daemon is shutting down");
    }
    if (!had_result) {
        host_log_warn("[daemon] command finish id=" + pending->id +
                      " action=" + action +
                      " session=" + session +
                      " ok=false error_code=bridge_timeout" +
                      " duration_ms=" + std::to_string(elapsed_ms_since(started)) +
                      " attempts=" + std::to_string(pending->dispatch_count) +
                      " queued=" + std::to_string(state.queued_actions.size()) +
                      " pending=" + std::to_string(state.pending_actions.size()));
        return failure("bridge_timeout", "timed out waiting for browser plugin action result");
    }
    json result = *pending->result;
    normalize_path_fields(result);
    const std::string error_code = envelope_error_code(result);
    const std::string line = "[daemon] command finish id=" + pending->id +
        " action=" + action +
        " session=" + session +
        " ok=" + std::string(result.value("ok", false) ? "true" : "false") +
        (error_code.empty() ? "" : " error_code=" + error_code) +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    if (result.value("ok", false)) {
        host_log_info(line);
    } else {
        host_log_warn(line);
    }
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
        host_log_warn("[daemon] plugin_poll rejected reason=extension_not_connected");
        return failure("extension_not_connected", "ace-browser-bridge browser plugin is not connected");
    }
    if (state.queued_actions.empty()) {
        return success({{"action", nullptr}});
    }
    auto pending = state.queued_actions.front();
    state.queued_actions.pop_front();
    pending->in_queue = false;
    pending->dispatched_at = std::chrono::steady_clock::now();
    pending->dispatch_count += 1;
    host_log_info("[daemon] plugin_poll dispatch id=" + pending->id +
                  " action=" + action_name_from_request(pending->request) +
                  " session=" + action_session_from_request(pending->request) +
                  " attempt=" + std::to_string(pending->dispatch_count) +
                  " queued=" + std::to_string(state.queued_actions.size()) +
                  " pending=" + std::to_string(state.pending_actions.size()));
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
        host_log_warn("[daemon] plugin_result unknown_action id=" + id);
        return failure("unknown_action", "plugin returned result for unknown action id");
    }
    const std::string action = action_name_from_request(it->second->request);
    const std::string session = action_session_from_request(it->second->request);
    const json result = (*parsed)["result"];
    const std::string error_code = envelope_error_code(result);
    const std::string line = "[daemon] plugin_result id=" + id +
        " action=" + action +
        " session=" + session +
        " ok=" + std::string(result.value("ok", false) ? "true" : "false") +
        (error_code.empty() ? "" : " error_code=" + error_code);
    if (result.value("ok", false)) {
        host_log_info(line);
    } else {
        host_log_warn(line);
    }
    it->second->result = (*parsed)["result"];
    state.cv.notify_all();
    return success({{"success", true}});
}

json handle_plugin_ack(DaemonState& state, const std::string& body) {
    auto parsed = parse_json(body.empty() ? "{}" : body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "plugin ack expects a JSON object");
    }
    const std::string id = parsed->value("id", "");
    if (id.empty()) {
        return failure("invalid_request", "plugin ack requires id");
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    // ack 顺带当一次插件心跳:刷新 last_hello,避免长操作期间被判 stale。
    state.extension_connected = true;
    state.last_hello = std::chrono::steady_clock::now();
    auto it = state.pending_actions.find(id);
    if (it == state.pending_actions.end()) {
        // 指令可能已超时清理或已完成;ack 落空不是错误路径,确认收下即可。
        return success({{"success", true}, {"known", false}});
    }
    it->second->acked = true;
    host_log_info("[daemon] plugin_ack id=" + id +
                  " action=" + action_name_from_request(it->second->request) +
                  " session=" + action_session_from_request(it->second->request));
    // 唤醒 handle_command 的等待循环,让它立刻看到 acked=true、不再触发重投。
    state.cv.notify_all();
    return success({{"success", true}, {"known", true}});
}

json handle_plugin_log(const std::string& body) {
    auto parsed = parse_json(body.empty() ? "{}" : body);
    if (!parsed || !parsed->is_object()) {
        return failure("invalid_request", "plugin log expects a JSON object");
    }
    const std::string level = parsed->value("level", "info");
    const std::string message = host_log_truncate(parsed->value("message", "plugin_event"), 240);
    const json data = parsed->contains("data") ? (*parsed)["data"] : json::object();
    const std::string line = "[extension] " + message + " data=" + json_for_log(data);
    if (level == "error" || level == "err") {
        host_log_error(line);
    } else if (level == "warn" || level == "warning") {
        host_log_warn(line);
    } else {
        host_log_info(line);
    }
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
    if (req.method == "POST" && req.path == "/plugin/ack") {
        if (!plugin_request) return http_json_response(403, failure("unauthorized", "plugin ack requires ace-browser-bridge extension"), cors_origin);
        return http_json_response(200, handle_plugin_ack(state, req.body), cors_origin);
    }
    if (req.method == "POST" && req.path == "/plugin/log") {
        if (!plugin_request) return http_json_response(403, failure("unauthorized", "plugin log requires ace-browser-bridge extension"), cors_origin);
        return http_json_response(200, handle_plugin_log(req.body), cors_origin);
    }
    if (req.method == "POST" && req.path == "/direct/ensure") {
        if (!host_request) return http_json_response(403, failure("unauthorized", "direct ensure requires ace-browser-host"));
        return http_json_response(200, handle_direct_ensure(state, port));
    }
    if (req.method == "POST" && req.path == "/command") {
        if (!host_request) return http_json_response(403, failure("unauthorized", "command requires ace-browser-host"));
        return http_json_response(200, handle_command(state, req.body));
    }
    if (req.method == "POST" && req.path == "/shutdown") {
        if (!host_request) return http_json_response(403, failure("unauthorized", "shutdown requires ace-browser-host"));
        running = false;
        host_log_info("[daemon] shutdown requested port=" + std::to_string(port));
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
    host_log_info("[daemon] serve starting port=" + std::to_string(port));
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        host_log_error("[daemon] serve socket_error port=" + std::to_string(port));
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
        host_log_error("[daemon] serve bind_failed port=" + std::to_string(port));
        print_json(failure("bind_failed", "failed to bind 127.0.0.1:" + std::to_string(port)));
        return 1;
    }
    if (listen(listener, 16) != 0) {
        close_socket(listener);
        host_log_error("[daemon] serve listen_failed port=" + std::to_string(port));
        print_json(failure("listen_failed", "failed to listen on 127.0.0.1:" + std::to_string(port)));
        return 1;
    }

    host_log_info("[daemon] serve listening port=" + std::to_string(port));
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
                host_log_warn("[daemon] invalid_http_request");
                response = http_json_response(400, failure("invalid_http_request", "failed to parse HTTP request"));
            } else {
                try {
                    response = route_request(*req, *state, port, *running);
                } catch (const std::exception& e) {
                    host_log_error("[daemon] route_exception path=" + req->path +
                                   " error=" + host_log_truncate(e.what()));
                    response = http_json_response(500, failure("internal_error", "daemon route failed"));
                } catch (...) {
                    host_log_error("[daemon] route_exception path=" + req->path +
                                   " error=unknown");
                    response = http_json_response(500, failure("internal_error", "daemon route failed"));
                }
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
    host_log_info("[daemon] serve stopped port=" + std::to_string(port));
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
        << "  wait --json --condition <condition> [--target <ref|selector>] [--text <text>] [--timeout-ms <ms>]\n"
        << "  assert --json --condition <condition> [--target <ref|selector>] [--text <text>] [--value <text>] [--url <text>] [--method <GET|POST>] [--status-class <2xx|3xx|4xx|5xx>] [--timeout-ms <ms>]\n"
        << "  batch --json [--session <name>] [--steps-file <path>|--stdin-input-json <json>]  # or JSON steps array/object on stdin\n"
        << "  click|fill|type|hover|drag|scroll --json [--session <name>] ...\n"
        << "  evaluate --json --code <javascript> [--session <name>]\n"
        << "  network --json --cmd <start|stop|list|detail> [--filter <text>] [--request-id <id>]\n"
        << "  devtools --json --cmd <console-start|console-list|network-start|network-detail|emulate|performance-start|performance-stop|heap-snapshot> [--session <name>]\n"
        << "  cdp --json --method <CDP.method> [--params <json>] [--session <name>]\n"
        << "  screenshot --json --session <name> --output <path> [--target <ref|selector>|--locator <json>|--attachment-ref <ref>] [--port 52007]\n"
        << "  save-pdf --json [--session <name>] [--file-name <name>]\n"
        << "  list-tabs --json [--session <name>] [--all]\n"
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
    if (command == "assert") return assert_command(argc, argv);
    if (command == "batch") return batch_command(argc, argv);
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

#ifndef ACE_BROWSER_HOST_NO_MAIN
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
#endif
