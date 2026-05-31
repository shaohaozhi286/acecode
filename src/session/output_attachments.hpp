#pragma once

#include "attachment_store.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct OutputAttachmentMaterializeResult {
    nlohmann::json attachments = nlohmann::json::array();
    std::vector<std::string> warnings;
};

nlohmann::json attachment_content_part(const AttachmentRecord& record);
nlohmann::json output_attachments_to_content_parts(const nlohmann::json& attachments);
nlohmann::json output_attachments_from_content_parts(const nlohmann::json& content_parts);
std::string output_attachments_fallback_text(const nlohmann::json& attachments);

OutputAttachmentMaterializeResult materialize_output_attachments(
    const nlohmann::json& attachments,
    const std::string& project_dir,
    const std::string& session_id,
    const std::function<std::string(const std::string&)>& validate_local_path = {},
    const std::string& local_path_base = {});

} // namespace acecode
