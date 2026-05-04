#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

struct HttpTextResult {
    long status_code = 0;
    std::string body;
    std::string error;
};

struct DownloadResult {
    long status_code = 0;
    std::uintmax_t bytes_written = 0;
    std::string error;
};

HttpTextResult fetch_text(const std::string& url, int timeout_ms);
DownloadResult download_to_file(const std::string& url,
                                const std::filesystem::path& output_path,
                                int timeout_ms);

} // namespace acecode::upgrade
