#include "file_read_tool.hpp"

#include "mtime_tracker.hpp"
#include "tool_icons.hpp"
#include "utils/encoding.hpp"
#include "utils/file_operations.hpp"
#include "utils/logger.hpp"
#include "utils/text_file_buffer.hpp"
#include "utils/tool_errors.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace acecode {

namespace {

constexpr size_t kFileReadContentLimit = 48 * 1024;
constexpr size_t kFileReadByteWindowLimit = 32 * 1024;
constexpr size_t kFileReadIoChunkSize = 1024 * 1024;
constexpr uintmax_t kLegacyReadMaterializationLimit =
    static_cast<uintmax_t>(512) * 1024 * 1024;
constexpr size_t kLargeFileHintThreshold = 200 * 1024;
constexpr const char* kFileUnchangedStub =
    "File unchanged since last read.";

struct ReadRequest {
    std::string file_path;
    int start_line = 0;
    int end_line = 0;
    bool has_line_range = false;
    bool byte_mode = false;
    uint64_t byte_offset = 0;
    size_t requested_max_bytes = 0;
    size_t effective_max_bytes = kFileReadByteWindowLimit;
};

struct LineReadResult {
    bool success = true;
    bool needs_materialized_fallback = false;
    std::string error;
    std::string content;
    int actual_start = 0;
    int actual_end = 0;
    int total_lines = 0;
    int displayed_line_count = 0;
    bool automatic_truncation = false;
    std::optional<int> next_line;
    std::optional<uint64_t> next_byte_offset;
};

struct ReadPresentation {
    bool partial = false;
    bool automatic_truncation = false;
    bool byte_mode = false;
    uint64_t byte_start = 0;
    uint64_t byte_end = 0; // exclusive
    std::optional<int> next_line;
    std::optional<uint64_t> next_byte_offset;
};

bool json_nonnegative_u64(const nlohmann::json& value, uint64_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<uint64_t>();
        return true;
    }
    if (!value.is_number_integer()) return false;
    const auto signed_value = value.get<int64_t>();
    if (signed_value < 0) return false;
    out = static_cast<uint64_t>(signed_value);
    return true;
}

bool json_line_number(const nlohmann::json& value, int& out) {
    if (!value.is_number_integer()) return false;
    const auto number = value.get<int64_t>();
    if (number < 0 || number > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(number);
    return true;
}

std::optional<ReadRequest> parse_read_request(
    const std::string& arguments_json,
    std::string& error
) {
    const auto args = nlohmann::json::parse(arguments_json, nullptr, false);
    if (!args.is_object()) {
        error = ToolErrors::parse_failed();
        return std::nullopt;
    }
    if (!args.contains("file_path")) {
        error = ToolErrors::missing_parameter("file_path");
        return std::nullopt;
    }
    if (!args["file_path"].is_string()) {
        error = ToolErrors::invalid_parameter(
            "file_path", "must be a string");
        return std::nullopt;
    }

    ReadRequest request;
    request.file_path = args["file_path"].get<std::string>();
    if (request.file_path.empty()) {
        error = ToolErrors::missing_parameter("file_path");
        return std::nullopt;
    }

    const bool has_start = args.contains("start_line");
    const bool has_end = args.contains("end_line");
    const bool has_byte_offset = args.contains("byte_offset");
    const bool has_max_bytes = args.contains("max_bytes");
    request.has_line_range = has_start || has_end;
    request.byte_mode = has_byte_offset || has_max_bytes;

    if (request.has_line_range && request.byte_mode) {
        error = ToolErrors::incompatible_parameters(
            "byte_offset/max_bytes", "start_line/end_line");
        return std::nullopt;
    }
    if (has_max_bytes && !has_byte_offset) {
        error = ToolErrors::invalid_parameter(
            "max_bytes", "requires byte_offset");
        return std::nullopt;
    }

    if (has_start && !json_line_number(args["start_line"], request.start_line)) {
        error = ToolErrors::invalid_parameter(
            "start_line", "must be a non-negative integer");
        return std::nullopt;
    }
    if (has_end && !json_line_number(args["end_line"], request.end_line)) {
        error = ToolErrors::invalid_parameter(
            "end_line", "must be a non-negative integer");
        return std::nullopt;
    }

    if (has_byte_offset &&
        !json_nonnegative_u64(args["byte_offset"], request.byte_offset)) {
        error = ToolErrors::invalid_parameter(
            "byte_offset", "must be a non-negative integer");
        return std::nullopt;
    }

    if (has_max_bytes) {
        uint64_t max_bytes = 0;
        if (!json_nonnegative_u64(args["max_bytes"], max_bytes) ||
            max_bytes == 0 || max_bytes > kFileReadByteWindowLimit) {
            error = ToolErrors::invalid_parameter(
                "max_bytes",
                "must be between 1 and " +
                std::to_string(kFileReadByteWindowLimit));
            return std::nullopt;
        }
        request.requested_max_bytes = static_cast<size_t>(max_bytes);
        request.effective_max_bytes = request.requested_max_bytes;
    }

    return request;
}

std::string utf8_prefix_without_marker(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    return truncate_utf8_prefix(value, max_bytes, "");
}

std::string format_read_metadata_footer(
    const FileReadEditMetadata& metadata,
    const ReadPresentation& presentation
) {
    std::ostringstream oss;
    oss << "\n<acecode-read-metadata"
        << " encoding=\"" << metadata.encoding << "\""
        << " line_endings=\"" << metadata.line_ending << "\""
        << " partial=\"" << (presentation.partial ? "true" : "false") << "\"";
    if (metadata.start_line > 0 && metadata.end_line > 0) {
        oss << " range=\"" << metadata.start_line << "-" << metadata.end_line << "\"";
    }
    if (presentation.automatic_truncation) {
        oss << " truncated=\"true\"";
    }
    if (presentation.byte_mode) {
        oss << " byte_range=\"" << presentation.byte_start
            << "-" << presentation.byte_end << "\"";
    }
    if (presentation.next_line.has_value()) {
        oss << " next_line=\"" << *presentation.next_line << "\"";
    }
    if (presentation.next_byte_offset.has_value()) {
        oss << " next_byte_offset=\"" << *presentation.next_byte_offset << "\"";
    }
    if (metadata.lossy) {
        oss << " lossy=\"true\""
            << " replacements=\"" << metadata.lossy_replacement_count << "\""
            << " editable=\"false\"";
    }
    oss << " />\n";
    return oss.str();
}

std::string format_file_unchanged_stub(
    const MtimeTracker::ReadObservation& observation
) {
    std::ostringstream oss;
    oss << kFileUnchangedStub
        << " The previous file_read result for this same file/window is still current.";
    if (!observation.tool_call_id.empty()) {
        oss << "\nPrevious file_read tool_call_id: " << observation.tool_call_id;
    }
    if (!observation.persisted_output_path.empty()) {
        oss << "\nFull previous output path: " << observation.persisted_output_path
            << "\nIf full content is needed, call file_read on that saved output path.";
    }
    oss << "\nDo not call file_read on the original file/window again unless the "
           "file changed or a different window is needed.";
    return oss.str();
}

uint64_t source_line_start_offset(
    const TextFileBuffer& buffer,
    int target_line
) {
    if (target_line <= 1) {
        return buffer.metadata.has_bom
            ? (buffer.metadata.encoding == TextEncoding::Utf8Bom ? 3 : 2)
            : 0;
    }

    const std::string& raw = buffer.raw_bytes;
    size_t pos = buffer.metadata.has_bom
        ? (buffer.metadata.encoding == TextEncoding::Utf8Bom ? 3 : 2)
        : 0;
    int line = 1;

    if (buffer.metadata.encoding == TextEncoding::Utf16Le ||
        buffer.metadata.encoding == TextEncoding::Utf16Be) {
        const bool little_endian = buffer.metadata.encoding == TextEncoding::Utf16Le;
        auto code_unit_at = [&](size_t at) -> uint16_t {
            const auto a = static_cast<unsigned char>(raw[at]);
            const auto b = static_cast<unsigned char>(raw[at + 1]);
            return little_endian
                ? static_cast<uint16_t>(a | (static_cast<uint16_t>(b) << 8))
                : static_cast<uint16_t>(b | (static_cast<uint16_t>(a) << 8));
        };
        while (pos + 1 < raw.size() && line < target_line) {
            const uint16_t current = code_unit_at(pos);
            pos += 2;
            if (current == '\r') {
                if (pos + 1 < raw.size() && code_unit_at(pos) == '\n') pos += 2;
                ++line;
            } else if (current == '\n') {
                ++line;
            }
        }
        return static_cast<uint64_t>(pos);
    }

    while (pos < raw.size() && line < target_line) {
        const unsigned char current = static_cast<unsigned char>(raw[pos++]);
        if (current == '\r') {
            if (pos < raw.size() && raw[pos] == '\n') ++pos;
            ++line;
        } else if (current == '\n') {
            ++line;
        }
    }
    return static_cast<uint64_t>(pos);
}

bool append_presented_line(
    LineReadResult& result,
    int line_number,
    const std::string& line,
    bool include_newline,
    bool numbered,
    uint64_t source_start,
    bool displayed_bytes_map_to_source
) {
    const std::string prefix = numbered
        ? std::to_string(line_number) + ": "
        : std::string{};

    if (result.content.size() + prefix.size() > kFileReadContentLimit) {
        result.automatic_truncation = true;
        result.next_line = line_number;
        return false;
    }
    result.content += prefix;

    const size_t remaining = kFileReadContentLimit - result.content.size();
    if (line.size() > remaining) {
        const std::string visible = utf8_prefix_without_marker(line, remaining);
        result.content += visible;
        result.automatic_truncation = true;
        result.next_byte_offset = displayed_bytes_map_to_source
            ? source_start + static_cast<uint64_t>(visible.size())
            : source_start;
        result.actual_end = line_number;
        ++result.displayed_line_count;
        return false;
    }

    result.content += line;
    if (include_newline) {
        if (result.content.size() == kFileReadContentLimit) {
            result.automatic_truncation = true;
            result.next_byte_offset =
                source_start + static_cast<uint64_t>(line.size());
            result.actual_end = line_number;
            ++result.displayed_line_count;
            return false;
        }
        result.content.push_back('\n');
    }

    result.actual_end = line_number;
    ++result.displayed_line_count;
    return true;
}

LineReadResult present_materialized_lines(
    const TextFileBuffer& buffer,
    const ReadRequest& request
) {
    LineReadResult result;
    const std::vector<std::string> lines =
        split_lf_lines_preserve_empty(buffer.text);
    result.total_lines = static_cast<int>(lines.size());

    const int start = request.start_line > 0 ? request.start_line : 1;
    const int requested_end = request.end_line > 0
        ? request.end_line
        : std::numeric_limits<int>::max();
    const int end = std::min(requested_end, result.total_lines);
    if (result.total_lines == 0 && !request.has_line_range) {
        return result;
    }
    if (result.total_lines == 0 || start > end || start > result.total_lines) {
        result.success = false;
        result.error = ToolErrors::no_lines_in_range(
            start,
            request.end_line > 0 ? request.end_line : result.total_lines,
            result.total_lines);
        return result;
    }

    result.actual_start = start;
    const bool exact_source_mapping =
        buffer.metadata.encoding == TextEncoding::Utf8 ||
        buffer.metadata.encoding == TextEncoding::Utf8Bom;

    for (int line_number = start; line_number <= end; ++line_number) {
        const size_t index = static_cast<size_t>(line_number - 1);
        const bool original_has_newline =
            index + 1 < lines.size() ||
            (!buffer.text.empty() && buffer.text.back() == '\n');
        const bool include_newline =
            request.has_line_range || original_has_newline;
        const uint64_t source_start =
            source_line_start_offset(buffer, line_number);
        if (!append_presented_line(
                result,
                line_number,
                lines[index],
                include_newline,
                request.has_line_range,
                source_start,
                exact_source_mapping)) {
            if (!result.next_byte_offset.has_value()) {
                result.next_line = line_number;
            }
            break;
        }
    }

    if (result.automatic_truncation &&
        !result.next_line.has_value() &&
        !result.next_byte_offset.has_value()) {
        result.next_line = result.actual_end + 1;
    }
    return result;
}

LineReadResult stream_utf8_lines(
    const std::string& path,
    bool has_utf8_bom,
    const ReadRequest& request
) {
    LineReadResult result;
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) {
        result.success = false;
        result.error = ToolErrors::cannot_open_file(path);
        return result;
    }

    uint64_t source_offset = has_utf8_bom ? 3 : 0;
    if (has_utf8_bom) ifs.seekg(3, std::ios::beg);

    const int start = request.start_line > 0 ? request.start_line : 1;
    const int end = request.end_line > 0
        ? request.end_line
        : std::numeric_limits<int>::max();
    int line_number = 1;
    int completed_lines = 0;
    bool line_has_bytes = false;
    bool line_is_presented = false;
    bool stopped = false;
    bool pending_cr = false;
    uint64_t pending_cr_offset = 0;
    std::string pending_utf8;
    size_t expected_utf8_bytes = 0;
    uint64_t pending_utf8_offset = 0;

    auto line_is_selected = [&]() {
        return line_number >= start && line_number <= end;
    };

    auto begin_presented_line = [&]() -> bool {
        if (!line_is_selected() || line_is_presented) return true;
        const std::string prefix = request.has_line_range
            ? std::to_string(line_number) + ": "
            : std::string{};
        if (result.content.size() + prefix.size() > kFileReadContentLimit) {
            result.automatic_truncation = true;
            result.next_line = line_number;
            return false;
        }
        if (result.actual_start == 0) result.actual_start = line_number;
        result.content += prefix;
        line_is_presented = true;
        return true;
    };

    auto mark_truncated_inside_line = [&](uint64_t byte_offset) {
        result.automatic_truncation = true;
        result.next_byte_offset = byte_offset;
        result.actual_end = line_number;
        if (line_is_presented) ++result.displayed_line_count;
    };

    auto append_source_token = [&](const std::string& token,
                                   uint64_t token_offset) -> bool {
        line_has_bytes = true;
        if (!line_is_selected()) return true;
        if (!begin_presented_line()) return false;
        if (result.content.size() + token.size() > kFileReadContentLimit) {
            mark_truncated_inside_line(token_offset);
            return false;
        }
        result.content += token;
        return true;
    };

    auto finish_line = [&](bool has_source_newline,
                           uint64_t terminator_offset) -> bool {
        if (line_is_selected()) {
            if (!begin_presented_line()) return false;
            if (has_source_newline) {
                if (result.content.size() == kFileReadContentLimit) {
                    mark_truncated_inside_line(terminator_offset);
                    return false;
                }
                result.content.push_back('\n');
            } else if (request.has_line_range &&
                       result.content.size() < kFileReadContentLimit) {
                // Preserve the established numbered-range presentation. This
                // newline is formatting, not unread source content.
                result.content.push_back('\n');
            }
            result.actual_end = line_number;
            ++result.displayed_line_count;
        }

        ++completed_lines;
        if (line_number >= end) return false;
        if (line_number == std::numeric_limits<int>::max()) {
            result.success = false;
            result.error = ToolErrors::too_many_lines_for_line_read();
            return false;
        }
        ++line_number;
        line_has_bytes = false;
        line_is_presented = false;
        return true;
    };

    auto fail_to_materialized_fallback = [&]() {
        result.needs_materialized_fallback = true;
        stopped = true;
    };

    std::vector<char> block(kFileReadIoChunkSize);
    while (!stopped && ifs.good()) {
        ifs.read(block.data(), static_cast<std::streamsize>(block.size()));
        const size_t bytes_read = static_cast<size_t>(ifs.gcount());
        if (bytes_read == 0) break;

        for (size_t i = 0; i < bytes_read; ++i) {
            const uint64_t byte_offset = source_offset + i;
            const unsigned char byte =
                static_cast<unsigned char>(block[i]);

            if (pending_cr) {
                if (byte != '\n') {
                    // CR-only and mixed endings need normalization by the
                    // existing full decoder.
                    fail_to_materialized_fallback();
                    break;
                }
                pending_cr = false;
                if (!finish_line(true, pending_cr_offset)) {
                    stopped = true;
                    break;
                }
                continue;
            }

            if (pending_utf8.empty()) {
                if (byte == '\0') {
                    fail_to_materialized_fallback();
                    break;
                }
                if (byte == '\r') {
                    pending_cr = true;
                    pending_cr_offset = byte_offset;
                    continue;
                }
                if (byte == '\n') {
                    if (!finish_line(true, byte_offset)) {
                        stopped = true;
                        break;
                    }
                    continue;
                }
                if (byte <= 0x7F) {
                    const std::string token(1, static_cast<char>(byte));
                    if (!append_source_token(token, byte_offset)) {
                        stopped = true;
                        break;
                    }
                    continue;
                }

                if ((byte & 0xE0) == 0xC0) expected_utf8_bytes = 2;
                else if ((byte & 0xF0) == 0xE0) expected_utf8_bytes = 3;
                else if ((byte & 0xF8) == 0xF0) expected_utf8_bytes = 4;
                else {
                    fail_to_materialized_fallback();
                    break;
                }
                pending_utf8.assign(1, static_cast<char>(byte));
                pending_utf8_offset = byte_offset;
                continue;
            }

            if ((byte & 0xC0) != 0x80) {
                fail_to_materialized_fallback();
                break;
            }
            pending_utf8.push_back(static_cast<char>(byte));
            if (pending_utf8.size() == expected_utf8_bytes) {
                if (!text_bytes_are_valid_utf8(pending_utf8)) {
                    fail_to_materialized_fallback();
                    break;
                }
                if (!append_source_token(
                        pending_utf8,
                        pending_utf8_offset)) {
                    stopped = true;
                    break;
                }
                pending_utf8.clear();
                expected_utf8_bytes = 0;
            }
        }
        source_offset += bytes_read;
    }

    if (result.needs_materialized_fallback || !result.success) return result;
    if (!stopped) {
        if (pending_cr || !pending_utf8.empty()) {
            result.needs_materialized_fallback = true;
            return result;
        }
        if (line_has_bytes) {
            finish_line(false, source_offset);
        }
    }

    result.total_lines = completed_lines;
    if (result.actual_start == 0) {
        result.success = false;
        result.error = ToolErrors::no_lines_in_range(
            start,
            request.end_line > 0 ? request.end_line : completed_lines,
            completed_lines);
        return result;
    }
    if (result.automatic_truncation &&
        !result.next_line.has_value() &&
        !result.next_byte_offset.has_value()) {
        result.next_line = result.actual_end + 1;
    }
    return result;
}

TextBufferResult read_file_probe(
    const std::string& path,
    uintmax_t file_size
) {
    const size_t probe_size = static_cast<size_t>(
        std::min<uintmax_t>(file_size, kFileReadIoChunkSize));
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) {
        return TextBufferResult{
            false,
            {},
            ToolErrors::cannot_open_file(path)
        };
    }

    std::string probe(probe_size, '\0');
    ifs.read(probe.data(), static_cast<std::streamsize>(probe.size()));
    probe.resize(static_cast<size_t>(ifs.gcount()));
    if (static_cast<uintmax_t>(probe.size()) < file_size) {
        trim_trailing_partial_utf8(probe);
    }
    return decode_text_file_bytes(probe, path, true);
}

std::string large_legacy_materialization_error(
    uintmax_t file_size,
    const TextFileMetadata& metadata
) {
    return ToolErrors::large_text_requires_streaming_encoding(
        text_encoding_label(metadata.encoding),
        static_cast<size_t>(file_size / (1024 * 1024)),
        static_cast<size_t>(
            kLegacyReadMaterializationLimit / (1024 * 1024)));
}

ToolResult execute_file_read(
    const std::string& arguments_json,
    const ToolContext& /*ctx*/
) {
    std::string parse_error;
    auto parsed_request = parse_read_request(arguments_json, parse_error);
    if (!parsed_request.has_value()) {
        return ToolResult{parse_error, false};
    }
    const ReadRequest request = *parsed_request;

    LOG_DEBUG(
        "file_read: path=" + request.file_path +
        " start=" + std::to_string(request.start_line) +
        " end=" + std::to_string(request.end_line) +
        " byte_offset=" + std::to_string(request.byte_offset));

    auto exists_check = FileOperations::check_file_exists(request.file_path);
    if (!exists_check.success) return exists_check;

    std::error_code file_ec;
    const auto fs_path = path_from_utf8(request.file_path);
    if (!std::filesystem::is_regular_file(fs_path, file_ec) || file_ec) {
        return ToolResult{
            ToolErrors::path_not_regular_file(request.file_path),
            false
        };
    }
    const uintmax_t file_size = std::filesystem::file_size(fs_path, file_ec);
    if (file_ec) {
        return ToolResult{ToolErrors::cannot_open_file(request.file_path), false};
    }

    auto unchanged_observation =
        MtimeTracker::instance().unchanged_read_observation(
            request.file_path,
            request.start_line,
            request.end_line,
            request.byte_mode,
            request.byte_offset,
            request.requested_max_bytes);
    if (unchanged_observation.has_value()) {
        ToolSummary summary;
        summary.verb = "Read";
        summary.object = request.file_path;
        summary.metrics.emplace_back("cache", "unchanged");
        summary.icon = tool_icon("file_read");

        ToolResult cached{
            format_file_unchanged_stub(*unchanged_observation),
            true
        };
        cached.summary = std::move(summary);
        return cached;
    }

    TextBufferResult probe_result =
        file_size <= FileOperations::MAX_EDIT_FILE_SIZE
        ? read_text_file_buffer(request.file_path, true)
        : read_file_probe(request.file_path, file_size);
    if (!probe_result.success) {
        return ToolResult{probe_result.error, false};
    }

    FileReadEditMetadata metadata;
    metadata.encoding =
        text_encoding_label(probe_result.buffer.metadata.encoding);
    metadata.line_ending =
        line_ending_label(probe_result.buffer.metadata.line_ending);
    if (probe_result.buffer.metadata.lossy) {
        metadata.encoding += " (lossy)";
        metadata.lossy = true;
        metadata.lossy_replacement_count =
            probe_result.buffer.metadata.lossy_replacement_count;
    }

    std::string content;
    int displayed_line_count = 0;
    ReadPresentation presentation;

    if (request.byte_mode) {
        if (request.byte_offset >= file_size) {
            return ToolResult{
                ToolErrors::byte_offset_outside_file(
                    static_cast<size_t>(request.byte_offset),
                    static_cast<size_t>(file_size)),
                false
            };
        }

        std::ifstream ifs(fs_path, std::ios::binary);
        if (!ifs.is_open()) {
            return ToolResult{
                ToolErrors::cannot_open_file(request.file_path),
                false
            };
        }
        std::vector<char> io_buffer(kFileReadIoChunkSize);
        ifs.rdbuf()->pubsetbuf(
            io_buffer.data(),
            static_cast<std::streamsize>(io_buffer.size()));
        ifs.seekg(static_cast<std::streamoff>(request.byte_offset),
                  std::ios::beg);

        const uint64_t remaining =
            static_cast<uint64_t>(file_size) - request.byte_offset;
        const size_t to_read = static_cast<size_t>(
            std::min<uint64_t>(remaining, request.effective_max_bytes));
        std::string raw(to_read, '\0');
        ifs.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        raw.resize(static_cast<size_t>(ifs.gcount()));
        const size_t source_bytes_read = raw.size();
        size_t trailing_partial_bytes = 0;

        if (request.byte_offset == 0 &&
            raw.size() >= 3 &&
            static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw.erase(0, 3);
        }
        const bool clean_utf8_window =
            !probe_result.buffer.metadata.lossy &&
            (probe_result.buffer.metadata.encoding == TextEncoding::Utf8 ||
             probe_result.buffer.metadata.encoding == TextEncoding::Utf8Bom);
        const bool has_more_source =
            request.byte_offset + source_bytes_read < file_size;
        if (clean_utf8_window && has_more_source) {
            const size_t before_trim = raw.size();
            trim_trailing_partial_utf8(raw);
            trailing_partial_bytes = before_trim - raw.size();
        }
        content = normalize_text_to_lf(ensure_utf8(raw));
        if (content.size() > kFileReadContentLimit) {
            content = utf8_prefix_without_marker(
                content,
                kFileReadContentLimit);
        }
        displayed_line_count = static_cast<int>(
            std::count(content.begin(), content.end(), '\n'));
        if (!content.empty() && content.back() != '\n') {
            ++displayed_line_count;
        }

        presentation.partial = true;
        presentation.byte_mode = true;
        presentation.byte_start = request.byte_offset;
        presentation.byte_end =
            request.byte_offset +
            static_cast<uint64_t>(source_bytes_read - trailing_partial_bytes);
        if (presentation.byte_end < file_size) {
            presentation.next_byte_offset = presentation.byte_end;
        }
        MtimeTracker::instance().record_read(
            request.file_path,
            std::string{},
            true,
            metadata);
    } else {
        const bool clean_streamable_utf8 =
            !probe_result.buffer.metadata.lossy &&
            (probe_result.buffer.metadata.encoding == TextEncoding::Utf8 ||
             probe_result.buffer.metadata.encoding == TextEncoding::Utf8Bom) &&
            probe_result.buffer.metadata.line_ending != LineEndingStyle::Cr &&
            probe_result.buffer.metadata.line_ending != LineEndingStyle::Mixed;

        TextBufferResult full_result;
        bool has_full_buffer =
            file_size <= FileOperations::MAX_EDIT_FILE_SIZE;
        if (has_full_buffer) {
            full_result = std::move(probe_result);
        }

        LineReadResult line_result;
        if (!has_full_buffer && clean_streamable_utf8) {
            line_result = stream_utf8_lines(
                request.file_path,
                probe_result.buffer.metadata.encoding == TextEncoding::Utf8Bom,
                request);
        } else {
            if (!has_full_buffer) {
                if (file_size > kLegacyReadMaterializationLimit) {
                    return ToolResult{
                        large_legacy_materialization_error(
                            file_size,
                            probe_result.buffer.metadata),
                        false
                    };
                }
                full_result =
                    read_text_file_buffer(request.file_path, true);
                if (!full_result.success) {
                    return ToolResult{full_result.error, false};
                }
                has_full_buffer = true;
            }
            line_result =
                present_materialized_lines(full_result.buffer, request);
        }

        if (line_result.needs_materialized_fallback) {
            if (file_size > kLegacyReadMaterializationLimit) {
                return ToolResult{
                    large_legacy_materialization_error(
                        file_size,
                        probe_result.buffer.metadata),
                    false
                };
            }
            full_result = read_text_file_buffer(request.file_path, true);
            if (!full_result.success) {
                return ToolResult{full_result.error, false};
            }
            has_full_buffer = true;
            line_result =
                present_materialized_lines(full_result.buffer, request);
        }
        if (!line_result.success) {
            return ToolResult{line_result.error, false};
        }

        if (has_full_buffer) {
            metadata.encoding =
                text_encoding_label(full_result.buffer.metadata.encoding);
            metadata.line_ending =
                line_ending_label(full_result.buffer.metadata.line_ending);
            metadata.lossy = full_result.buffer.metadata.lossy;
            metadata.lossy_replacement_count =
                full_result.buffer.metadata.lossy_replacement_count;
            if (metadata.lossy) metadata.encoding += " (lossy)";
        }
        if (!metadata.lossy) {
            metadata.start_line = line_result.actual_start;
            metadata.end_line = line_result.actual_end;
        }

        content = std::move(line_result.content);
        displayed_line_count = line_result.displayed_line_count;
        presentation.partial =
            request.has_line_range ||
            line_result.automatic_truncation ||
            metadata.lossy;
        presentation.automatic_truncation =
            line_result.automatic_truncation;
        presentation.next_line = line_result.next_line;
        presentation.next_byte_offset =
            line_result.next_byte_offset;

        MtimeTracker::instance().record_read(
            request.file_path,
            has_full_buffer ? full_result.buffer.text : std::string{},
            presentation.partial,
            metadata);
    }

    MtimeTracker::instance().record_read_observation(
        request.file_path,
        request.start_line,
        request.end_line,
        request.byte_mode,
        request.byte_offset,
        request.requested_max_bytes);

    bool hint_added = false;
    if (!request.byte_mode &&
        !request.has_line_range &&
        file_size > kLargeFileHintThreshold) {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "[hint: file is large (" +
                   std::to_string(file_size / 1024) +
                   "KB). This read is bounded; use next_line or "
                   "next_byte_offset from the metadata to continue.]";
        hint_added = true;
    }

    if (presentation.automatic_truncation) {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "[truncated: file_read returned at most " +
                   std::to_string(kFileReadContentLimit) +
                   " content bytes.";
        if (presentation.next_line.has_value()) {
            content += " Continue with start_line=" +
                       std::to_string(*presentation.next_line) + ".";
        } else if (presentation.next_byte_offset.has_value()) {
            content += " Continue with byte_offset=" +
                       std::to_string(*presentation.next_byte_offset) + ".";
        }
        content += "]";
    }

    if (metadata.lossy) {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "[note: decoded with " +
                   std::to_string(metadata.lossy_replacement_count) +
                   " replacement(s) (U+FFFD); original encoding could not be "
                   "fully determined; editing is disabled for this lossy read.]";
    }

    content += format_read_metadata_footer(metadata, presentation);

    ToolSummary summary;
    summary.verb = "Read";
    summary.object = request.file_path;
    summary.metrics.emplace_back(
        "lines",
        std::to_string(displayed_line_count));
    summary.metrics.emplace_back(
        "size",
        format_bytes_compact(content.size()));
    summary.metrics.emplace_back("enc", metadata.encoding);
    summary.metrics.emplace_back("eol", metadata.line_ending);
    if (request.byte_mode) summary.metrics.emplace_back("mode", "bytes");
    if (presentation.automatic_truncation) {
        summary.metrics.emplace_back("partial", "truncated");
    }
    if (metadata.lossy) {
        summary.metrics.emplace_back(
            "lossy",
            std::to_string(metadata.lossy_replacement_count));
    }
    if (hint_added) summary.metrics.emplace_back("hint", "large_file");
    summary.icon = tool_icon("file_read");

    ToolResult result{content, true};
    result.summary = std::move(summary);
    return result;
}

} // namespace

ToolImpl create_file_read_tool() {
    ToolDef def;
    def.name = "file_read";
    def.description =
        "Read a bounded portion of a text file regardless of total file size. "
        "Use start_line/end_line for line ranges, or byte_offset/max_bytes to "
        "continue through an exceptionally long line. Follow next_line or "
        "next_byte_offset in truncated result metadata. Do not re-read the "
        "same file/range or byte window if its contents are already current in the "
        "conversation; repeated unchanged reads return a compact stub. "
        "Always use absolute paths.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Absolute path to the file to read"}
            }},
            {"start_line", {
                {"type", "integer"},
                {"description", "Start line number (1-indexed, inclusive). Optional."}
            }},
            {"end_line", {
                {"type", "integer"},
                {"description", "End line number (1-indexed, inclusive). Optional."}
            }},
            {"byte_offset", {
                {"type", "integer"},
                {"description", "Zero-based source byte offset for byte-window mode. Cannot be combined with line ranges."}
            }},
            {"max_bytes", {
                {"type", "integer"},
                {"description", "Maximum source bytes for byte-window mode (1-32768, default 32768). Requires byte_offset."}
            }}
        }},
        {"required", nlohmann::json::array({"file_path"})}
    });

    return ToolImpl{
        def,
        execute_file_read,
        /*is_read_only=*/true
    };
}

} // namespace acecode
