#pragma once

#include "memory_types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Write semantics for upsert().
enum class MemoryWriteMode {
    Create, // fail if an entry with `name` already exists
    Update, // fail if no entry with `name` exists
    Upsert, // create-or-replace (default)
};

// Thread-safe in-memory cache over ~/.acecode/memory/. Scans the directory
// once on construction (plus whenever reload() is called); all mutating
// operations (upsert / remove) atomically update both the entry file and the
// MEMORY.md index.
class MemoryRegistry {
public:
    MemoryRegistry() = default;

    // Scan disk now. Entries with invalid frontmatter / unreadable files are
    // skipped with LOG_WARN so one bad entry doesn't kill the whole cache.
    // Callers should call this once at startup; after that operate via
    // upsert/remove/reload.
    void scan();

    // Equivalent to scan(); named for /memory reload clarity and command help.
    void reload() { scan(); }

    // Return a copy of the current entry list, optionally filtered by type.
    std::vector<MemoryEntry> list(std::optional<MemoryType> type_filter = std::nullopt) const;

    // Look up by filesystem stem (the part before ".md"). Returns nullopt
    // when no entry by that name exists.
    std::optional<MemoryEntry> find(const std::string& name) const;

    // Read MEMORY.md raw (for system-prompt injection). Returns empty string
    // when the file doesn't exist or is zero-length. `max_bytes` enforces the
    // config.memory.max_index_bytes cap — content beyond that is truncated
    // and a marker appended.
    std::string read_index_raw(std::size_t max_bytes) const;

    // Upsert an entry. Returns the written entry on success (with path set);
    // returns nullopt and fills `error_out` on validation/write failure.
    // The in-memory cache is updated only when the on-disk write succeeds.
    std::optional<MemoryEntry> upsert(const std::string& name,
                                      MemoryType type,
                                      const std::string& description,
                                      const std::string& body,
                                      MemoryWriteMode mode,
                                      std::string& error_out);

    // Remove an entry. Returns true when the file existed and was deleted.
    // Removes the matching MEMORY.md line as a side effect.
    bool remove(const std::string& name, std::string& error_out);

    // Entry count, safe for logging / /memory list.
    std::size_t size() const;

private:
    // Non-locking helpers — callers must already hold mu_.
    std::vector<MemoryEntry>::iterator find_locked(const std::string& name);
    void rewrite_index_locked();

    mutable std::mutex mu_;
    std::vector<MemoryEntry> entries_;
};

} // namespace acecode
