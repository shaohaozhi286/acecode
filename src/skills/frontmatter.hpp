#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace acecode {

// Minimal YAML frontmatter value. Supports three shapes observed in skill
// SKILL.md files: scalar string, list of strings, and nested mapping.
struct FrontmatterValue;
using FrontmatterMap = std::map<std::string, FrontmatterValue>;

struct FrontmatterValue {
    enum class Type { String, List, Map };
    Type type = Type::String;
    std::string string_value;
    std::vector<std::string> list_value;
    FrontmatterMap map_value;

    FrontmatterValue() = default;
    explicit FrontmatterValue(std::string v) : type(Type::String), string_value(std::move(v)) {}
    explicit FrontmatterValue(std::vector<std::string> v) : type(Type::List), list_value(std::move(v)) {}
    explicit FrontmatterValue(FrontmatterMap v) : type(Type::Map), map_value(std::move(v)) {}

    bool is_string() const { return type == Type::String; }
    bool is_list() const   { return type == Type::List; }
    bool is_map() const    { return type == Type::Map; }
};

using Frontmatter = FrontmatterMap;

// Parse YAML frontmatter from a markdown document. Returns (frontmatter, body).
// If the content does not start with "---\n", returns ({}, content) unchanged.
// Handles:
//   - scalar values: `key: value` (quotes optional)
//   - bracketed lists: `key: [a, b, c]`
//   - multi-line dash lists under a key
//   - one level of indented nested mapping (`metadata:` → indented `hermes:` → ...)
// Invalid lines are skipped rather than aborting the whole parse.
std::pair<Frontmatter, std::string> parse_frontmatter(const std::string& content);

// Helpers for common lookups.
std::string get_string(const Frontmatter& fm, const std::string& key,
                       const std::string& fallback = "");

std::vector<std::string> get_list(const Frontmatter& fm, const std::string& key);

// Walk a nested mapping, e.g. get_nested(fm, {"metadata", "hermes", "tags"}).
// Returns a pointer to the deepest FrontmatterValue, or nullptr if any key is
// missing or not a map at an intermediate level.
const FrontmatterValue* get_nested(const Frontmatter& fm,
                                   const std::vector<std::string>& path);

} // namespace acecode
