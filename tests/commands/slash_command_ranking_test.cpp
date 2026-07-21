#include <gtest/gtest.h>

#include "commands/slash_command_ranking.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::string> names_of(
    const std::vector<acecode::SlashCommandCandidate>& candidates) {
    std::vector<std::string> names;
    names.reserve(candidates.size());
    for (const auto& candidate : candidates) names.push_back(candidate.name);
    return names;
}

} // namespace

TEST(SlashCommandRanking, UsageReordersEquallyRelevantMatches) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"alpha", "Alpha command"},
        {"help", "Show help"},
        {"zoom", "Zoom view"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"help", 3},
        {"zoom", 8},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"zoom", "help", "alpha"}));
}

TEST(SlashCommandRanking, MatchRelevancePrecedesUsage) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"config", "Change model options"},
        {"model", "Choose a model"},
        {"remote", "Remote control"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"config", 1000},
        {"remote", 500},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("mo", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"model", "remote", "config"}));
}

TEST(SlashCommandRanking, AlphabeticalOrderBreaksExactTie) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"zeta", "Zeta"},
        {"beta", "Beta"},
        {"alpha", "Alpha"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"alpha", 4},
        {"beta", 4},
        {"zeta", 4},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"alpha", "beta", "zeta"}));
}

TEST(SlashCommandRanking, UnmatchedCandidatesAreFilteredOut) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"model", "Choose a model"},
        {"help", "Show all commands"},
        {"config", "Runtime settings"},
    };
    const acecode::SlashCommandUsageCounts usage = {{"help", 999}};

    const auto ranked =
        acecode::rank_slash_command_candidates("model", candidates, usage);

    ASSERT_EQ(ranked.size(), 1u);
    EXPECT_EQ(ranked.front().name, "model");
}
