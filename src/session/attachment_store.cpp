#include "attachment_store.hpp"

#include "../image/image_processor.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string extension_from_name(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};

    std::string ext = name.substr(dot);
    if (ext.size() > 16) return {};
    for (char c : ext) {
        if (c == '.') continue;
        if (!std::isalnum(static_cast<unsigned char>(c))) return {};
    }
    return ascii_lower(ext);
}

std::string extension_for_mime(const std::string& mime_type) {
    const std::string mime = ascii_lower(mime_type);
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg") return ".jpg";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/webp") return ".webp";
    if (mime == "image/bmp") return ".bmp";
    if (mime == "image/svg+xml") return ".svg";
    if (mime == "text/plain") return ".txt";
    if (mime == "text/markdown") return ".md";
    if (mime == "application/json") return ".json";
    return {};
}

std::string replace_extension_for_mime(const std::string& name,
                                       const std::string& mime_type) {
    const std::string ext = extension_for_mime(mime_type);
    if (ext.empty()) return name;
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
        return name.substr(0, dot) + ext;
    }
    return name + ext;
}

fs::path attachments_dir(const std::string& project_dir,
                         const std::string& session_id) {
    return path_from_utf8(project_dir) / "attachments" / session_id;
}

fs::path metadata_path_for(const std::string& project_dir,
                           const std::string& session_id,
                           const std::string& attachment_id) {
    return attachments_dir(project_dir, session_id) / (attachment_id + ".json");
}

std::string generate_attachment_id() {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "att_" << now << "_" << std::hex << counter.fetch_add(1);
    return oss.str();
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

} // namespace

std::string attachment_blob_url(const std::string& session_id,
                                const std::string& attachment_id) {
    return "/api/sessions/" + session_id + "/attachments/" + attachment_id + "/blob";
}

std::string attachment_mime_for_name(const std::string& name,
                                     const std::string& supplied_mime_type) {
    if (!supplied_mime_type.empty()) return supplied_mime_type;

    const std::string ext = extension_from_name(name);
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".bmp") return "image/bmp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".txt" || ext == ".log") return "text/plain";
    if (ext == ".md" || ext == ".markdown") return "text/markdown";
    if (ext == ".json") return "application/json";
    if (ext == ".csv") return "text/csv";
    if (ext == ".js" || ext == ".jsx") return "text/javascript";
    if (ext == ".ts" || ext == ".tsx") return "text/typescript";
    if (ext == ".css") return "text/css";
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".xml") return "application/xml";
    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".c" ||
        ext == ".py" || ext == ".rs" || ext == ".go" || ext == ".java" ||
        ext == ".yml" || ext == ".yaml" || ext == ".toml" || ext == ".ini") {
        return "text/plain";
    }
    return "application/octet-stream";
}

std::string attachment_kind_for_mime(const std::string& mime_type,
                                     const std::string& name) {
    const std::string mime = ascii_lower(attachment_mime_for_name(name, mime_type));
    if (mime.rfind("image/", 0) == 0) return "image";
    if (mime.rfind("text/", 0) == 0 ||
        mime == "application/json" ||
        mime == "application/xml") {
        return "file";
    }
    return "file";
}

bool is_valid_attachment_id(const std::string& id) {
    if (id.empty() || id.size() > 96) return false;
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

std::optional<AttachmentRecord> save_attachment(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& name,
    const std::string& supplied_mime_type,
    const std::string& bytes,
    std::string* error) {
    if (project_dir.empty() || session_id.empty()) {
        set_error(error, "session storage unavailable");
        return std::nullopt;
    }
    if (bytes.empty()) {
        set_error(error, "attachment data required");
        return std::nullopt;
    }

    std::string stored_name = name.empty() ? "attachment" : name;
    std::string stored_mime = attachment_mime_for_name(stored_name, supplied_mime_type);
    std::string stored_bytes = bytes;
    nlohmann::json normalization = nlohmann::json::object();
    if (attachment_kind_for_mime(stored_mime, stored_name) == "image") {
        auto normalized = image::normalize_image_bytes(stored_bytes, stored_mime);
        if (normalized.attempted) {
            normalization = {
                {"backend", normalized.backend},
                {"changed", normalized.changed},
                {"ok", normalized.ok},
                {"reason", normalized.reason},
                {"original_size_bytes", bytes.size()},
                {"threshold_bytes", image::kImageCompressionThresholdBytes},
                {"max_edge", image::kImageNormalizeMaxEdge},
                {"input_width", normalized.input.width},
                {"input_height", normalized.input.height},
                {"input_channels", normalized.input.channels},
            };
            if (normalized.output.width > 0 && normalized.output.height > 0) {
                normalization["output_width"] = normalized.output.width;
                normalization["output_height"] = normalized.output.height;
                normalization["output_channels"] = normalized.output.channels;
            }
            if (!normalized.error.empty()) normalization["error"] = normalized.error;
            LOG_INFO("[attachment-store] image normalization"
                     " session=" + session_id +
                     " name=" + stored_name +
                     " ok=" + std::string(normalized.ok ? "1" : "0") +
                     " changed=" + std::string(normalized.changed ? "1" : "0") +
                     " original_size=" + std::to_string(bytes.size()) +
                     " reason=" + normalized.reason +
                     " error=" + normalized.error);
            if (!normalized.ok) {
                set_error(error, "image normalization failed: " +
                    (normalized.error.empty() ? normalized.reason : normalized.error));
                return std::nullopt;
            }
            if (normalized.ok && normalized.changed) {
                stored_bytes = std::move(normalized.bytes);
                if (!normalized.mime_type.empty()) {
                    const std::string original_name = stored_name;
                    stored_mime = normalized.mime_type;
                    stored_name = replace_extension_for_mime(stored_name, stored_mime);
                    normalization["output_size_bytes"] = stored_bytes.size();
                    normalization["output_mime_type"] = stored_mime;
                    normalization["original_name"] = original_name;
                }
            }
        }
    }

    if (stored_bytes.size() > kMaxAttachmentBytes) {
        set_error(error, "attachment too large");
        return std::nullopt;
    }

    AttachmentRecord record;
    record.id = generate_attachment_id();
    record.session_id = session_id;
    record.name = stored_name;
    record.mime_type = stored_mime;
    record.kind = attachment_kind_for_mime(record.mime_type, record.name);
    record.size_bytes = static_cast<std::uintmax_t>(stored_bytes.size());
    record.blob_url = attachment_blob_url(session_id, record.id);
    if (!normalization.empty()) {
        record.metadata["image_normalization"] = normalization;
    }

    std::string ext = extension_from_name(record.name);
    if (ext.empty()) ext = extension_for_mime(record.mime_type);

    const fs::path dir = attachments_dir(project_dir, session_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        set_error(error, "failed to create attachment directory: " + ec.message());
        return std::nullopt;
    }

    const fs::path blob_path = dir / (record.id + ext);
    {
        std::ofstream ofs(blob_path, std::ios::binary);
        if (!ofs.is_open()) {
            set_error(error, "failed to write attachment");
            return std::nullopt;
        }
        ofs.write(stored_bytes.data(), static_cast<std::streamsize>(stored_bytes.size()));
        if (!ofs.good()) {
            set_error(error, "failed to write attachment bytes");
            return std::nullopt;
        }
    }

    record.path = path_to_utf8(blob_path);
    atomic_write_file(path_to_utf8(metadata_path_for(project_dir, session_id, record.id)),
                      attachment_to_json(record).dump(2));
    return record;
}

std::optional<AttachmentRecord> load_attachment(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& attachment_id,
    std::string* error) {
    if (!is_valid_attachment_id(attachment_id)) {
        set_error(error, "invalid attachment id");
        return std::nullopt;
    }

    const fs::path meta_path = metadata_path_for(project_dir, session_id, attachment_id);
    std::ifstream ifs(meta_path, std::ios::binary);
    if (!ifs.is_open()) {
        set_error(error, "attachment not found");
        return std::nullopt;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    try {
        auto parsed = nlohmann::json::parse(content);
        auto record = attachment_from_json(parsed);
        if (!record.has_value() || record->session_id != session_id || record->id != attachment_id) {
            set_error(error, "invalid attachment metadata");
            return std::nullopt;
        }
        return record;
    } catch (const std::exception& e) {
        set_error(error, std::string("bad attachment metadata: ") + e.what());
        return std::nullopt;
    }
}

std::optional<std::string> read_attachment_bytes(
    const AttachmentRecord& record,
    std::size_t max_bytes,
    std::string* error) {
    if (record.path.empty()) {
        set_error(error, "attachment path missing");
        return std::nullopt;
    }

    std::ifstream ifs(path_from_utf8(record.path), std::ios::binary);
    if (!ifs.is_open()) {
        set_error(error, "failed to open attachment");
        return std::nullopt;
    }

    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    if (bytes.size() > max_bytes) {
        set_error(error, "attachment too large");
        return std::nullopt;
    }
    return bytes;
}

nlohmann::json attachment_to_json(const AttachmentRecord& record) {
    nlohmann::json j = {
        {"id", record.id},
        {"session_id", record.session_id},
        {"name", record.name},
        {"kind", record.kind},
        {"mime_type", record.mime_type},
        {"path", record.path},
        {"blob_url", record.blob_url},
        {"size_bytes", record.size_bytes},
    };
    if (record.metadata.is_object() && !record.metadata.empty()) {
        j["metadata"] = record.metadata;
    }
    return j;
}

std::optional<AttachmentRecord> attachment_from_json(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    AttachmentRecord record;
    if (!j.contains("id") || !j["id"].is_string()) return std::nullopt;
    record.id = j["id"].get<std::string>();
    if (!is_valid_attachment_id(record.id)) return std::nullopt;
    record.session_id = j.value("session_id", std::string{});
    record.name = j.value("name", std::string{"attachment"});
    record.kind = j.value("kind", std::string{});
    record.mime_type = j.value("mime_type", std::string{});
    record.path = j.value("path", std::string{});
    record.blob_url = j.value("blob_url", std::string{});
    record.size_bytes = j.value("size_bytes", static_cast<std::uintmax_t>(0));
    if (record.kind.empty()) {
        record.kind = attachment_kind_for_mime(record.mime_type, record.name);
    }
    if (record.mime_type.empty()) {
        record.mime_type = attachment_mime_for_name(record.name, std::string{});
    }
    if (record.blob_url.empty() && !record.session_id.empty()) {
        record.blob_url = attachment_blob_url(record.session_id, record.id);
    }
    if (j.contains("metadata") && j["metadata"].is_object()) {
        record.metadata = j["metadata"];
    }
    return record;
}

} // namespace acecode
