#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct SlashCommandCandidate {
    std::string name;
    std::string description;
};

using SlashCommandUsageCounts = std::map<std::string, std::uint64_t>;

// Filter and rank slash-command candidates for the TUI picker. Existing
// match relevance remains the primary key; usage replaces alphabetical order
// only among equally relevant matches.
std::vector<SlashCommandCandidate> rank_slash_command_candidates(
    std::string_view query,
    const std::vector<SlashCommandCandidate>& candidates,
    const SlashCommandUsageCounts& usage_counts);

} // namespace acecode
