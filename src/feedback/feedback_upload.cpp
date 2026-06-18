#include "feedback_upload.hpp"

#include "../config/config.hpp"
#include "../network/proxy_resolver.hpp"
#include "../session/session_storage.hpp"
#include "../upgrade/manifest.hpp"
#include "../utils/encoding.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <vector>

#include <cpr/cpr.h>
#include <zip.h>

namespace fs = std::filesystem;

namespace acecode::feedback {

namespace {

std::string zip_error_from_code(int code) {
    zip_error_t ze;
    zip_error_init_with_code(&ze, code);
    std::string message = zip_error_strerror(&ze);
    zip_error_fini(&ze);
    return message;
}

bool add_buffer_entry(zip_t* archive,
                      const std::string& entry_name,
                      const std::string& content,
                      std::string* error) {
    zip_source_t* source = zip_source_buffer(
        archive, content.data(), static_cast<zip_uint64_t>(content.size()), 0);
    if (!source) {
        if (error) *error = "failed to create zip source for " + entry_name;
        return false;
    }
    if (zip_file_add(archive, entry_name.c_str(), source,
                     ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        if (error) *error = "failed to add zip entry " + entry_name + ": " +
                            zip_strerror(archive);
        zip_source_free(source);
        return false;
    }
    return true;
}

zip_source_t* source_from_file(zip_t* archive, const fs::path& path) {
#ifdef _WIN32
    return zip_source_win32w(archive, path.native().c_str(), 0, -1);
#else
    const std::string encoded = path_to_utf8(path);
    return zip_source_file(archive, encoded.c_str(), 0, -1);
#endif
}

bool add_file_entry(zip_t* archive,
                    const std::string& entry_name,
                    const fs::path& path,
                    std::string* error) {
    zip_source_t* source = source_from_file(archive, path);
    if (!source) {
        if (error) *error = "failed to create zip source for " + path_to_utf8(path);
        return false;
    }
    if (zip_file_add(archive, entry_name.c_str(), source,
                     ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
        if (error) *error = "failed to add zip entry " + entry_name + ": " +
                            zip_strerror(archive);
        zip_source_free(source);
        return false;
    }
    return true;
}

std::string filename_timestamp_from_created_at(const std::string& created_at) {
    std::string digits;
    digits.reserve(14);
    for (char ch : created_at) {
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            digits.push_back(ch);
            if (digits.size() == 14) break;
        }
    }
    if (digits.size() >= 14) {
        return digits.substr(0, 8) + "-" + digits.substr(8, 6);
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return oss.str();
}

bool read_tail(const fs::path& path,
               std::size_t max_bytes,
               std::string* out,
               std::size_t* bytes_read) {
    if (out) out->clear();
    if (bytes_read) *bytes_read = 0;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;

    const auto size = fs::file_size(path, ec);
    if (ec) return false;

    const std::uintmax_t wanted =
        max_bytes == 0 ? 0 : std::min<std::uintmax_t>(size, max_bytes);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    if (wanted < size) {
        ifs.seekg(static_cast<std::streamoff>(size - wanted), std::ios::beg);
    }
    std::string data(static_cast<std::size_t>(wanted), '\0');
    if (wanted > 0) {
        ifs.read(data.data(), static_cast<std::streamsize>(wanted));
        data.resize(static_cast<std::size_t>(ifs.gcount()));
    }
    if (out) *out = std::move(data);
    if (bytes_read && out) *bytes_read = out->size();
    return true;
}

std::string default_output_dir() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = fs::current_path(ec);
    if (dir.empty()) dir = ".";
    return path_to_utf8(dir / "acecode-feedback");
}

std::string trim_ascii_whitespace(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string first_non_empty_env(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        std::string value = trim_ascii_whitespace(getenv_utf8(name));
        if (!value.empty()) return value;
    }
    return {};
}

std::string current_computer_name() {
#ifdef _WIN32
    return first_non_empty_env({"COMPUTERNAME", "HOSTNAME"});
#else
    return first_non_empty_env({"HOSTNAME", "COMPUTERNAME"});
#endif
}

std::string current_login_name() {
#ifdef _WIN32
    return first_non_empty_env({"USERNAME", "USER", "LOGNAME"});
#else
    return first_non_empty_env({"USER", "LOGNAME", "USERNAME"});
#endif
}

void append_filename_component(std::string& out, const std::string& value) {
    const std::string trimmed = trim_ascii_whitespace(value);
    if (trimmed.empty()) return;
    out.push_back('-');
    out += sanitize_feedback_filename_component(trimmed);
}

} // namespace

std::string sanitize_feedback_filename_component(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    const char* hex = "0123456789abcdef";
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else if (c >= 0x80) {
            out.push_back('x');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        } else {
            out.push_back('-');
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '-')) {
        out.erase(out.begin());
    }
    if (out.empty()) out = "session";
    if (out.size() > 80) out.resize(80);
    return out;
}

std::string make_feedback_package_filename(const std::string& session_id,
                                           const std::string& created_at,
                                           const std::string& platform,
                                           const std::string& computer_name,
                                           const std::string& login_name) {
    std::string filename = "acecode-feedback-" +
        sanitize_feedback_filename_component(session_id) + "-" +
        filename_timestamp_from_created_at(created_at);
    append_filename_component(filename, platform);
    append_filename_component(filename, computer_name);
    append_filename_component(filename, login_name);
    filename += ".zip";
    return filename;
}

FeedbackPackageResult build_feedback_package(const FeedbackPackageRequest& request) {
    FeedbackPackageResult result;
    if (request.session_id.empty()) {
        result.error = "missing session id";
        return result;
    }
    if (request.session_jsonl_path.empty()) {
        result.error = "missing session JSONL path";
        return result;
    }
    std::error_code ec;
    if (!fs::is_regular_file(request.session_jsonl_path, ec)) {
        result.error = "session JSONL file not found: " +
                       path_to_utf8(request.session_jsonl_path);
        return result;
    }

    const std::string created_at = request.created_at.empty()
        ? SessionStorage::now_iso8601()
        : request.created_at;
    const std::string platform = request.platform.empty()
        ? upgrade::current_target()
        : request.platform;
    const std::string computer_name = request.computer_name.empty()
        ? current_computer_name()
        : request.computer_name;
    const std::string login_name = request.login_name.empty()
        ? current_login_name()
        : request.login_name;
    const std::string package_filename =
        make_feedback_package_filename(
            request.session_id, created_at, platform, computer_name, login_name);
    const fs::path output_dir = request.output_dir.empty()
        ? path_from_utf8(default_output_dir())
        : request.output_dir;

    fs::create_directories(output_dir, ec);
    if (ec) {
        result.error = "failed to create feedback directory: " + ec.message();
        return result;
    }

    const fs::path package_path = output_dir / path_from_utf8(package_filename);
    fs::remove(package_path, ec);

    std::string log_tail;
    std::size_t log_tail_bytes = 0;
    const bool log_included =
        !request.log_path.empty() &&
        read_tail(request.log_path, request.max_log_bytes, &log_tail, &log_tail_bytes);

    std::vector<std::string> included_files;
    const std::string session_entry =
        "session/" + sanitize_feedback_filename_component(request.session_id) + ".jsonl";
    included_files.push_back(session_entry);
    if (log_included) included_files.push_back("logs/acecode.log.tail.txt");
    included_files.push_back("feedback.json");

    nlohmann::json metadata = {
        {"feedback_text", request.feedback_text},
        {"session_id", request.session_id},
        {"created_at", created_at},
        {"acecode_version", request.acecode_version},
        {"platform", platform},
        {"computer_name", computer_name},
        {"login_name", login_name},
        {"log_available", log_included},
        {"log_tail_bytes", static_cast<std::uint64_t>(log_tail_bytes)},
        {"included_files", included_files},
    };
    if (!request.log_path.empty()) {
        metadata["log_path"] = path_to_utf8(request.log_path);
    }

    const std::string metadata_text = metadata.dump(2);

    int zip_error = 0;
    zip_t* archive = zip_open(path_to_utf8(package_path).c_str(),
                              ZIP_CREATE | ZIP_TRUNCATE, &zip_error);
    if (!archive) {
        result.error = "failed to create feedback zip: " + zip_error_from_code(zip_error);
        return result;
    }

    std::string entry_error;
    bool ok = add_file_entry(archive, session_entry, request.session_jsonl_path, &entry_error);
    if (ok && log_included) {
        ok = add_buffer_entry(archive, "logs/acecode.log.tail.txt", log_tail, &entry_error);
    }
    if (ok) {
        ok = add_buffer_entry(archive, "feedback.json", metadata_text, &entry_error);
    }

    if (!ok) {
        zip_discard(archive);
        fs::remove(package_path, ec);
        result.error = entry_error;
        return result;
    }

    if (zip_close(archive) != 0) {
        result.error = "failed to finish feedback zip";
        fs::remove(package_path, ec);
        return result;
    }

    result.ok = true;
    result.package_path = package_path;
    result.package_filename = package_filename;
    result.log_included = log_included;
    result.log_tail_bytes = log_tail_bytes;
    result.included_files = std::move(included_files);
    return result;
}

FeedbackUploadResult upload_feedback_package(const FeedbackUploadRequest& request) {
    FeedbackUploadResult result;
    if (!is_valid_upgrade_base_url(request.upload_url)) {
        result.error = "upgrade.base_url must be a non-empty http or https URL";
        return result;
    }
    std::error_code ec;
    if (!fs::is_regular_file(request.package_path, ec)) {
        result.error = "feedback package not found: " + path_to_utf8(request.package_path);
        return result;
    }

    const std::string url = normalize_upgrade_base_url(request.upload_url);
    const std::string filename = request.package_filename.empty()
        ? request.package_path.filename().string()
        : request.package_filename;
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/json, text/plain, */*"},
                    {"User-Agent", "acecode-feedback"}},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{request.timeout_ms},
        cpr::Multipart{
            {"file",
             cpr::Files{cpr::File{path_to_utf8(request.package_path), filename}},
             "application/zip"},
            {"filename", filename},
        });

    result.status_code = response.status_code;
    result.response_body = std::move(response.text);
    if (response.error.code != cpr::ErrorCode::OK) {
        result.error = response.error.message;
        return result;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        result.error = "HTTP " + std::to_string(response.status_code);
        return result;
    }
    try {
        if (!result.response_body.empty()) {
            auto body = nlohmann::json::parse(result.response_body);
            if (body.contains("success")) {
                if (!body["success"].is_boolean() || !body["success"].get<bool>()) {
                    result.error = "server returned success:false";
                    if (body.contains("error") && body["error"].is_string()) {
                        result.error += ": " + body["error"].get<std::string>();
                    }
                    return result;
                }
            }
        }
    } catch (...) {
        // Non-JSON 2xx responses are accepted for compatibility with simple file servers.
    }
    result.ok = true;
    return result;
}

} // namespace acecode::feedback
