#include "output_attachments.hpp"

#include "../utils/base64.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void push_warning(OutputAttachmentMaterializeResult& result, const std::string& warning) {
    if (warning.empty()) return;
    result.warnings.push_back(warning);
    LOG_WARN("[output-attachment] " + warning);
}

std::string value_string(const nlohmann::json& j,
                         std::initializer_list<const char*> keys) {
    if (!j.is_object()) return {};
    for (const char* key : keys) {
        auto it = j.find(key);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    }
    return {};
}

std::string filename_from_path(const std::string& path) {
    if (path.empty()) return {};
    try {
        auto name = path_from_utf8(path).filename();
        return path_to_utf8(name);
    } catch (...) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
}

std::optional<std::pair<std::string, std::string>> parse_image_data_url(
    const std::string& url,
    std::string* error) {
    constexpr const char* prefix = "data:";
    if (url.rfind(prefix, 0) != 0) return std::nullopt;
    const auto comma = url.find(',');
    if (comma == std::string::npos) {
        if (error) *error = "data URL missing comma separator";
        return std::nullopt;
    }

    std::string header = url.substr(5, comma - 5);
    std::string header_lower = ascii_lower(header);
    if (header_lower.rfind("image/", 0) != 0) {
        if (error) *error = "data URL is not an image";
        return std::nullopt;
    }
    if (header_lower.find(";base64") == std::string::npos) {
        if (error) *error = "image data URL must be base64";
        return std::nullopt;
    }
    const auto semi = header.find(';');
    const std::string mime = semi == std::string::npos ? header : header.substr(0, semi);
    auto decoded = base64_decode(url.substr(comma + 1));
    if (!decoded.has_value()) {
        if (error) *error = "invalid base64 image data URL";
        return std::nullopt;
    }
    return std::make_pair(mime, std::move(*decoded));
}

std::optional<std::string> read_file_bytes(const std::string& path,
                                           const std::string& local_path_base,
                                           std::string* error) {
    std::error_code ec;
    fs::path resolved = path_from_utf8(path);
    if (resolved.is_relative() && !local_path_base.empty()) {
        resolved = path_from_utf8(local_path_base) / resolved;
    }
    if (resolved.is_relative()) resolved = fs::absolute(resolved, ec);
    if (ec) {
        if (error) *error = "failed to resolve local output image path: " + ec.message();
        return std::nullopt;
    }
    if (!fs::exists(resolved, ec) || ec) {
        if (error) *error = "local output image path does not exist: " + path;
        return std::nullopt;
    }
    if (!fs::is_regular_file(resolved, ec) || ec) {
        if (error) *error = "local output image path is not a regular file: " + path;
        return std::nullopt;
    }
    const auto size = fs::file_size(resolved, ec);
    if (ec) {
        if (error) *error = "failed to stat local output image path: " + path;
        return std::nullopt;
    }
    if (size > kMaxAttachmentBytes) {
        if (error) *error = "local output image exceeds attachment size limit: " + path;
        return std::nullopt;
    }

    std::ifstream ifs(resolved, std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open local output image path: " + path;
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    if (!ifs.good() && !ifs.eof()) {
        if (error) *error = "failed to read local output image path: " + path;
        return std::nullopt;
    }
    return bytes;
}

std::optional<nlohmann::json> stored_attachment_json_from(const nlohmann::json& item) {
    if (!item.is_object()) return std::nullopt;
    if (item.contains("attachment") && item["attachment"].is_object()) {
        auto record = attachment_from_json(item["attachment"]);
        if (record.has_value()) return attachment_to_json(*record);
    }
    auto record = attachment_from_json(item);
    if (record.has_value()) return attachment_to_json(*record);
    return std::nullopt;
}

void materialize_one(const nlohmann::json& item,
                     const std::string& project_dir,
                     const std::string& session_id,
                     const std::function<std::string(const std::string&)>& validate_local_path,
                     const std::string& local_path_base,
                     OutputAttachmentMaterializeResult& result) {
    if (!item.is_object()) {
        push_warning(result, "output attachment descriptor must be an object");
        return;
    }

    if (auto stored = stored_attachment_json_from(item)) {
        result.attachments.push_back(std::move(*stored));
        return;
    }

    const std::string data_url = value_string(item, {"data_url", "dataUrl", "url", "source_url"});
    const std::string local_path = value_string(item, {"path", "file_path", "filePath"});
    std::string name = value_string(item, {"name", "filename", "file_name"});
    std::string mime = value_string(item, {"mime_type", "mimeType", "mime"});
    std::string bytes;

    if (!data_url.empty() && data_url.rfind("data:", 0) == 0) {
        std::string error;
        auto parsed = parse_image_data_url(data_url, &error);
        if (!parsed.has_value()) {
            push_warning(result, error.empty() ? "failed to decode output image data URL" : error);
            return;
        }
        if (mime.empty()) mime = parsed->first;
        bytes = std::move(parsed->second);
        if (name.empty()) name = "tool-output" + std::string(
            mime == "image/jpeg" || mime == "image/jpg" ? ".jpg" : ".png");
    } else if (!local_path.empty()) {
        if (validate_local_path) {
            std::string validation_error = validate_local_path(local_path);
            if (!validation_error.empty()) {
                push_warning(result, validation_error);
                return;
            }
        }
        if (name.empty()) name = filename_from_path(local_path);
        if (mime.empty()) mime = attachment_mime_for_name(name, std::string{});
        if (attachment_kind_for_mime(mime, name) != "image") {
            push_warning(result, "local output attachment is not an image: " + local_path);
            return;
        }
        std::string error;
        auto read = read_file_bytes(local_path, local_path_base, &error);
        if (!read.has_value()) {
            push_warning(result, error);
            return;
        }
        bytes = std::move(*read);
    } else {
        push_warning(result, "output attachment missing data_url or path");
        return;
    }

    if (attachment_kind_for_mime(mime, name) != "image") {
        push_warning(result, "output attachment MIME is not renderable as image: " + mime);
        return;
    }

    std::string save_error;
    auto record = save_attachment(project_dir, session_id, name, mime, bytes, &save_error);
    if (!record.has_value()) {
        push_warning(result, save_error.empty() ? "failed to save output image attachment" : save_error);
        return;
    }
    result.attachments.push_back(attachment_to_json(*record));
}

std::string attachment_display_name(const nlohmann::json& attachment) {
    if (!attachment.is_object()) return "attachment";
    std::string name = attachment.value("name", std::string{});
    if (!name.empty()) return name;
    return attachment.value("id", std::string{"attachment"});
}

} // namespace

nlohmann::json attachment_content_part(const AttachmentRecord& record) {
    const std::string kind = attachment_kind_for_mime(record.mime_type, record.name);
    return nlohmann::json{
        {"type", kind == "image" ? "image" : "file"},
        {"attachment", attachment_to_json(record)},
    };
}

nlohmann::json output_attachments_to_content_parts(const nlohmann::json& attachments) {
    nlohmann::json parts = nlohmann::json::array();
    if (!attachments.is_array()) return parts;
    for (const auto& item : attachments) {
        auto record = stored_attachment_json_from(item);
        if (!record.has_value()) continue;
        const std::string kind = attachment_kind_for_mime(
            record->value("mime_type", std::string{}),
            record->value("name", std::string{}));
        parts.push_back(nlohmann::json{
            {"type", kind == "image" ? "image" : "file"},
            {"attachment", std::move(*record)},
        });
    }
    return parts;
}

nlohmann::json output_attachments_from_content_parts(const nlohmann::json& content_parts) {
    nlohmann::json attachments = nlohmann::json::array();
    if (!content_parts.is_array()) return attachments;
    for (const auto& part : content_parts) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type != "image" && type != "file") continue;
        if (!part.contains("attachment") || !part["attachment"].is_object()) continue;
        auto record = attachment_from_json(part["attachment"]);
        if (!record.has_value()) continue;
        attachments.push_back(attachment_to_json(*record));
    }
    return attachments;
}

std::string output_attachments_fallback_text(const nlohmann::json& attachments) {
    if (!attachments.is_array() || attachments.empty()) return {};
    std::ostringstream oss;
    bool first = true;
    for (const auto& item : attachments) {
        nlohmann::json attachment;
        if (item.is_object() && item.contains("attachment") && item["attachment"].is_object()) {
            attachment = item["attachment"];
        } else {
            attachment = item;
        }
        if (!attachment.is_object()) continue;
        if (!first) oss << '\n';
        first = false;
        const std::string mime = attachment.value("mime_type", std::string{});
        const bool image = attachment_kind_for_mime(mime, attachment.value("name", std::string{})) == "image";
        oss << '[' << (image ? "image" : "attachment") << ": "
            << attachment_display_name(attachment) << ']';
    }
    return oss.str();
}

OutputAttachmentMaterializeResult materialize_output_attachments(
    const nlohmann::json& attachments,
    const std::string& project_dir,
    const std::string& session_id,
    const std::function<std::string(const std::string&)>& validate_local_path,
    const std::string& local_path_base) {
    OutputAttachmentMaterializeResult result;
    if (!attachments.is_array() || attachments.empty()) return result;
    if (project_dir.empty() || session_id.empty()) {
        push_warning(result, "active session storage is required for output attachments");
        return result;
    }

    for (const auto& item : attachments) {
        materialize_one(item, project_dir, session_id, validate_local_path,
                        local_path_base, result);
    }
    return result;
}

} // namespace acecode
