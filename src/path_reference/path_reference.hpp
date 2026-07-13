#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode::path_reference {

constexpr std::size_t kMaxSuggestions = 50;

struct Token {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string path;
    bool quoted = false;
};

struct Query {
    std::string directory;
    std::string filter;
};

struct Candidate {
    std::string name;
    std::string path;
    bool is_directory = false;
};

struct SuggestionResult {
    std::vector<Candidate> items;
    std::string error;
};

struct Replacement {
    std::string text;
    std::size_t cursor = 0;
};

// Find the @ token that contains cursor_bytes. The token must begin at the
// start of input or after ASCII whitespace. Quoted tokens may contain spaces.
std::optional<Token> token_at_cursor(const std::string& input,
                                     std::size_t cursor_bytes);

// Split a relative reference into the directory to enumerate and the basename
// fragment to filter. Both slash styles are accepted and normalized to '/'.
Query split_query(const std::string& path);

// Format a cwd-relative file or directory as visible prompt text. Directory
// references retain a trailing slash; paths containing whitespace are quoted.
std::string format_reference(const std::string& relative_path,
                             bool is_directory,
                             bool trailing_space = true);

// Replace the complete token, including any part after the caret. When
// enter_directory is true the directory is left open as @path/ with no space.
Replacement replace_token(const std::string& input,
                          const Token& token,
                          const std::string& relative_path,
                          bool is_directory,
                          bool enter_directory);

// Enumerate one directory level under cwd, include hidden/noise entries, filter
// by the basename fragment, and cap the stable directory-first result at 50.
SuggestionResult suggest(const std::string& cwd, const std::string& typed_path);

} // namespace acecode::path_reference
