#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace acecode::feedback {

constexpr std::size_t kDefaultLogTailBytes = 512 * 1024;

struct FeedbackPackageRequest {
    std::string feedback_text;
    std::string session_id;
    std::filesystem::path session_jsonl_path;
    std::filesystem::path log_path;
    std::filesystem::path output_dir;
    std::string created_at;
    std::string acecode_version;
    std::string platform;
    std::string computer_name;
    std::string login_name;
    std::size_t max_log_bytes = kDefaultLogTailBytes;
};

struct FeedbackPackageResult {
    bool ok = false;
    std::filesystem::path package_path;
    std::string package_filename;
    std::string error;
    bool log_included = false;
    std::size_t log_tail_bytes = 0;
    std::vector<std::string> included_files;
};

struct FeedbackUploadRequest {
    std::string upload_url;
    std::filesystem::path package_path;
    std::string package_filename;
    int timeout_ms = 30000;
};

struct FeedbackUploadResult {
    bool ok = false;
    long status_code = 0;
    std::string response_body;
    std::string error;
};

std::string sanitize_feedback_filename_component(const std::string& value);
std::string make_feedback_package_filename(const std::string& session_id,
                                           const std::string& created_at,
                                           const std::string& platform = {},
                                           const std::string& computer_name = {},
                                           const std::string& login_name = {});

FeedbackPackageResult build_feedback_package(const FeedbackPackageRequest& request);
FeedbackUploadResult upload_feedback_package(const FeedbackUploadRequest& request);

} // namespace acecode::feedback
