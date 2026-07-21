#include "slash_command_ranking.hpp"

#include <algorithm>
#include <utility>

namespace acecode {

namespace {

int match_score(std::string_view query,
                std::string_view name,
                std::string_view description) {
    if (query.empty()) return 1;
    if (name.rfind(query, 0) == 0) return 100;
    if (name.find(query) != std::string_view::npos) return 50;
    if (description.find(query) != std::string_view::npos) return 10;
    return 0;
}

struct RankedCandidate {
    int match_score = 0;
    std::uint64_t usage_count = 0;
    SlashCommandCandidate candidate;
};

} // namespace

std::vector<SlashCommandCandidate> rank_slash_command_candidates(
    std::string_view query,
    const std::vector<SlashCommandCandidate>& candidates,
    const SlashCommandUsageCounts& usage_counts) {
    std::vector<RankedCandidate> ranked;
    ranked.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        const int score = match_score(
            query, candidate.name, candidate.description);
        if (score == 0) continue;

        const auto usage = usage_counts.find(candidate.name);
        ranked.push_back({
            score,
            usage == usage_counts.end() ? 0 : usage->second,
            candidate,
        });
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const RankedCandidate& a, const RankedCandidate& b) {
                  if (a.match_score != b.match_score) {
                      return a.match_score > b.match_score;
                  }
                  if (a.usage_count != b.usage_count) {
                      return a.usage_count > b.usage_count;
                  }
                  return a.candidate.name < b.candidate.name;
              });

    std::vector<SlashCommandCandidate> result;
    result.reserve(ranked.size());
    for (auto& item : ranked) {
        result.push_back(std::move(item.candidate));
    }
    return result;
}

} // namespace acecode
