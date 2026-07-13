#pragma once

#include "session_storage.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace acecode::session_export {

// Build a portable Markdown snapshot of the visible session transcript.
std::string build_markdown(const SessionMeta& meta,
                           const std::vector<ChatMessage>& messages);

// Return a filesystem-safe filename stem while preserving UTF-8 characters.
std::string sanitize_filename_stem(const std::string& preferred,
                                  const std::string& fallback);

// Select a non-existing UTF-8 Markdown filename in the destination directory.
// The returned string is a filename only, not an absolute path.
std::string choose_markdown_filename(const std::filesystem::path& directory,
                                     const std::string& preferred,
                                     const std::string& fallback);

} // namespace acecode::session_export
