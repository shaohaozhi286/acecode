#include "memory_registry.hpp"

#include "memory_frontmatter.hpp"
#include "memory_index.hpp"
#include "memory_paths.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string read_file_to_string(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// Atomic write: dump text to `<target>.tmp-<rand>` then rename() over target.
// Returns empty string on success; rejection reason otherwise.
std::string atomic_write(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) return "failed to create memory dir: " + ec.message();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    auto suffix = std::to_string(gen());
    fs::path tmp = target;
    tmp += ".tmp-" + suffix;

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return "failed to open temp file: " + tmp.string();
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) return "failed to write temp file: " + tmp.string();
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(tmp, cleanup_ec);
        return "failed to rename temp to target: " + ec.message();
    }
    return {};
}

} // namespace

void MemoryRegistry::scan() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();

    fs::path dir = get_memory_dir();
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        // Empty directory is not an error — first launch.
        return;
    }
    if (!fs::is_directory(dir, ec)) {
        LOG_WARN("[memory] " + dir.string() + " is not a directory; ignoring");
        return;
    }

    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (!fs::is_regular_file(p)) continue;
        if (p.extension() != ".md") continue;
        // Skip the index itself; entries live in sibling files.
        std::string stem = p.stem().string();
        std::string stem_lower = stem;
        std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (stem_lower == "memory") continue;

        auto parsed = parse_memory_entry_file(p);
        if (!parsed) continue; // parse_memory_entry_file logs the reason.
        entries_.push_back(std::move(*parsed));
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const MemoryEntry& a, const MemoryEntry& b) { return a.name < b.name; });
}

std::vector<MemoryEntry> MemoryRegistry::list(std::optional<MemoryType> type_filter) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!type_filter.has_value()) return entries_;
    std::vector<MemoryEntry> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.type == *type_filter) out.push_back(e);
    }
    return out;
}

std::optional<MemoryEntry> MemoryRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : entries_) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

std::string MemoryRegistry::read_index_raw(std::size_t max_bytes) const {
    std::lock_guard<std::mutex> lock(mu_);
    fs::path idx = get_memory_index_path();
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec) || ec) return {};

    std::string content = read_file_to_string(idx);
    if (max_bytes > 0 && content.size() > max_bytes) {
        std::size_t dropped = content.size() - max_bytes;
        content.resize(max_bytes);
        content += "\n[... truncated " + std::to_string(dropped) + " bytes]\n";
        LOG_WARN("[memory] MEMORY.md exceeded max_index_bytes; truncated in-memory by " +
                 std::to_string(dropped) + " bytes");
    }
    return content;
}

std::vector<MemoryEntry>::iterator MemoryRegistry::find_locked(const std::string& name) {
    return std::find_if(entries_.begin(), entries_.end(),
                        [&](const MemoryEntry& e) { return e.name == name; });
}

void MemoryRegistry::rewrite_index_locked() {
    fs::path idx = get_memory_index_path();
    std::string existing;
    std::error_code ec;
    if (fs::is_regular_file(idx, ec) && !ec) {
        existing = read_file_to_string(idx);
    }
    std::string rendered = render_memory_index(entries_, existing);
    std::string err = atomic_write(idx, rendered);
    if (!err.empty()) {
        LOG_ERROR("[memory] failed to rewrite MEMORY.md: " + err);
    }
}

std::optional<MemoryEntry> MemoryRegistry::upsert(const std::string& name,
                                                  MemoryType type,
                                                  const std::string& description,
                                                  const std::string& body,
                                                  MemoryWriteMode mode,
                                                  std::string& error_out) {
    error_out.clear();
    std::string name_err = validate_memory_name(name);
    if (!name_err.empty()) {
        error_out = name_err;
        return std::nullopt;
    }
    if (description.empty()) {
        error_out = "description must not be empty";
        return std::nullopt;
    }

    fs::path target = resolve_memory_entry_path(name);
    if (target.empty() || !is_within_memory_dir(target)) {
        error_out = "resolved path escapes ~/.acecode/memory/: " + name;
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mu_);

    auto existing = find_locked(name);
    bool file_exists = existing != entries_.end();
    if (mode == MemoryWriteMode::Create && file_exists) {
        error_out = "memory entry already exists: " + name;
        return std::nullopt;
    }
    if (mode == MemoryWriteMode::Update && !file_exists) {
        error_out = "memory entry does not exist: " + name;
        return std::nullopt;
    }

    MemoryEntry entry;
    entry.name = name;
    entry.description = description;
    entry.type = type;
    entry.path = target;
    entry.body = body;

    std::string rendered = render_memory_entry(entry);
    std::string write_err = atomic_write(target, rendered);
    if (!write_err.empty()) {
        error_out = write_err;
        return std::nullopt;
    }

    if (file_exists) {
        *existing = entry;
    } else {
        entries_.push_back(entry);
        std::sort(entries_.begin(), entries_.end(),
                  [](const MemoryEntry& a, const MemoryEntry& b) { return a.name < b.name; });
    }

    rewrite_index_locked();
    return entry;
}

bool MemoryRegistry::remove(const std::string& name, std::string& error_out) {
    error_out.clear();
    std::string name_err = validate_memory_name(name);
    if (!name_err.empty()) {
        error_out = name_err;
        return false;
    }

    fs::path target = resolve_memory_entry_path(name);
    if (target.empty() || !is_within_memory_dir(target)) {
        error_out = "resolved path escapes ~/.acecode/memory/: " + name;
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);

    auto it = find_locked(name);
    if (it == entries_.end()) {
        error_out = "memory entry does not exist: " + name;
        return false;
    }

    std::error_code ec;
    fs::remove(target, ec);
    if (ec) {
        error_out = "failed to remove entry file: " + ec.message();
        return false;
    }

    entries_.erase(it);

    // Update MEMORY.md: drop the matching line and keep everything else.
    fs::path idx = get_memory_index_path();
    if (fs::is_regular_file(idx, ec) && !ec) {
        std::string existing = read_file_to_string(idx);
        std::string updated = remove_memory_index_line(existing, name);
        if (updated != existing) {
            std::string werr = atomic_write(idx, updated);
            if (!werr.empty()) {
                LOG_ERROR("[memory] failed to update MEMORY.md after remove: " + werr);
            }
        }
    }
    return true;
}

std::size_t MemoryRegistry::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

} // namespace acecode
