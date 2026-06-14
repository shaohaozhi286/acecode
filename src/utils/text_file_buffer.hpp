#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace acecode {

enum class TextEncoding {
    Utf8,
    Utf8Bom,
    Utf16Le,
    Utf16Be,
    Gbk,
    Gb18030,
    Binary,
    Unsupported
};

enum class LineEndingStyle {
    None,
    Lf,
    CrLf,
    Cr,
    Mixed
};

struct TextFileMetadata {
    TextEncoding encoding = TextEncoding::Utf8;
    LineEndingStyle line_ending = LineEndingStyle::Lf;
    bool has_bom = false;
    bool binary = false;
    bool unsupported = false;
    bool lossy = false;
    size_t lossy_replacement_count = 0;
    std::string error;
};

struct TextFileBuffer {
    std::string path;
    std::string raw_bytes;
    std::string text; // UTF-8, LF-normalized
    TextFileMetadata metadata;
};

struct TextBufferResult {
    bool success = false;
    TextFileBuffer buffer;
    std::string error;
};

struct TextEncodeResult {
    bool success = false;
    std::string bytes;
    std::string error;
};

struct TextSafeWriteResult {
    bool success = false;
    std::string error;
    bool rolled_back = false;
    bool rollback_failed = false;
};

std::string text_encoding_label(TextEncoding encoding);
std::string line_ending_label(LineEndingStyle style);

bool text_bytes_are_valid_utf8(const std::string& bytes);
std::string normalize_text_to_lf(std::string text);
LineEndingStyle detect_line_ending_style(const std::string& text);
std::string restore_line_endings(const std::string& lf_text, LineEndingStyle style);

TextBufferResult read_text_file_buffer(const std::string& path,
                                       bool allow_lossy = false);
TextBufferResult decode_text_file_bytes(const std::string& bytes,
                                        const std::string& path = "",
                                        bool allow_lossy = false);
TextBufferResult decode_text_file_bytes_with_metadata(const std::string& bytes,
                                                      const TextFileMetadata& metadata,
                                                      const std::string& path = "");
TextEncodeResult encode_text_for_write(const std::string& lf_text,
                                       const TextFileMetadata& metadata);

TextFileMetadata default_new_file_text_metadata();
TextSafeWriteResult safe_write_text_file(
    const std::string& path,
    const std::string& lf_text,
    const TextFileMetadata& metadata,
    const std::function<void(const std::string& path)>& before_write = {});

std::vector<std::string> split_lf_lines_preserve_empty(const std::string& lf_text);
std::string line_range_content(const std::string& lf_text,
                               int start_line,
                               int end_line,
                               bool* ok = nullptr,
                               int* total_lines = nullptr);
std::string range_hash(const std::string& lf_text, int start_line, int end_line);
std::string read_id_for_text_buffer(const std::string& path, const std::string& raw_bytes);

} // namespace acecode
