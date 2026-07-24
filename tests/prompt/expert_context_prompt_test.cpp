#include <gtest/gtest.h>

#include "experts/expert_registry.hpp"
#include "prompt/system_prompt.hpp"

namespace {

acecode::ExpertDefinition make_team() {
    acecode::ExpertDefinition expert;
    expert.id = "delivery-team";
    expert.version = "1.2.0";
    expert.type = acecode::ExpertType::Team;
    expert.display_name = "Delivery Team";
    expert.description = "Ship a verified change.";
    expert.references_existing_experts = true;
    expert.lead_expert_id = "planner";
    expert.member_expert_ids = {"tester"};
    expert.lead_agent_id = "planner";
    expert.agents = {
        {"planner", "Planner", "Coordinator",
         "Plan the work and verify the result.", {}, {}},
        {"tester", "Tester", "QA", "Test the assigned change thoroughly.", {}},
    };
    expert.member_agent_ids = {"tester"};
    return expert;
}

} // namespace

TEST(ExpertContextPrompt, TeamLeadReceivesDeclaredDelegationContract) {
    auto expert = make_team();
    auto block = acecode::build_expert_context_prompt(&expert);
    EXPECT_NE(block.content.find("Delivery Team"), std::string::npos);
    EXPECT_NE(block.content.find("Team purpose: Ship a verified change."),
              std::string::npos);
    EXPECT_NE(block.content.find("selected experts"), std::string::npos);
    EXPECT_NE(block.content.find("spawn_subagent(expert_member=\"<id>\"") , std::string::npos);
    EXPECT_NE(block.content.find("tester: Tester - QA"), std::string::npos);
    EXPECT_NE(block.content.find("does not grant tools, permissions, or sandbox exceptions"),
              std::string::npos);
    EXPECT_NE(block.content.find("Plan the work and verify the result."), std::string::npos);
    EXPECT_NE(block.cache_key.find("delivery-team:1.2.0"), std::string::npos);
}

TEST(ExpertContextPrompt, MemberReceivesOnlyItsOwnInstructions) {
    auto expert = make_team();
    auto block = acecode::build_expert_context_prompt(&expert, "tester");
    EXPECT_NE(block.content.find("Test the assigned change thoroughly."), std::string::npos);
    EXPECT_EQ(block.content.find("Plan the work and verify the result."), std::string::npos);
    EXPECT_EQ(block.content.find("spawn_subagent(expert_member"), std::string::npos);
}

TEST(ExpertContextPrompt, ExpertPrecedesOtherDynamicSessionContext) {
    auto expert = make_team();
    auto block = acecode::build_session_context_prompt(
        "/tmp", nullptr, nullptr, nullptr, nullptr, 0, nullptr, "git snapshot", &expert);
    const auto expert_pos = block.content.find("# Selected Expert Component");
    const auto git_pos = block.content.find("# Git Status");
    ASSERT_NE(expert_pos, std::string::npos);
    ASSERT_NE(git_pos, std::string::npos);
    EXPECT_LT(expert_pos, git_pos);
}
