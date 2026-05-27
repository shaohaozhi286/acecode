#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

struct AttachmentRecord {
    std::string id;
    std::string session_id;
    std::string name;
    std::string kind;
    std::string mime_type;
    std::string path;
    std::string blob_url;
    std::uintmax_t size_bytes = 0;
    nlohmann::json metadata = nlohmann::json::object();
};

constexpr std::size_t kMaxAttachmentBytes = 25u * 1024u * 1024u;

std::string attachment_blob_url(const std::string& session_id,
                                const std::string& attachment_id);
std::string attachment_kind_for_mime(const std::string& mime_type,
                                     const std::string& name);
std::string attachment_mime_for_name(const std::string& name,
                                     const std::string& supplied_mime_type);
bool is_valid_attachment_id(const std::string& id);

std::optional<AttachmentRecord> save_attachment(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& name,
    const std::string& supplied_mime_type,
    const std::string& bytes,
    std::string* error = nullptr);

std::optional<AttachmentRecord> load_attachment(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& attachment_id,
    std::string* error = nullptr);

std::optional<std::string> read_attachment_bytes(
    const AttachmentRecord& record,
    std::size_t max_bytes = kMaxAttachmentBytes,
    std::string* error = nullptr);

nlohmann::json attachment_to_json(const AttachmentRecord& record);
std::optional<AttachmentRecord> attachment_from_json(const nlohmann::json& j);

} // namespace acecode
