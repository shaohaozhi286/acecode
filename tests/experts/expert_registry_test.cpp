#include <gtest/gtest.h>

#include "experts/expert_registry.hpp"
#include "permissions.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("acecode_experts_test_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

acecode::ExpertDraft make_agent(std::string id = "reviewer",
                                std::string display = "Code Reviewer") {
    acecode::ExpertDraft draft;
    draft.id = std::move(id);
    draft.display_name = std::move(display);
    draft.profession = "Review specialist";
    draft.description = "Finds correctness and security issues.";
    draft.quick_prompts = {"Review this change"};
    draft.lead = {"lead", "Lead Reviewer", "Reviewer", "Review code carefully."};
    return draft;
}

} // namespace

TEST(ExpertRegistry, CreatesDiscoversUpdatesAndDeletesGlobalExpert) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    auto draft = make_agent();
    ASSERT_TRUE(registry.create_global(draft, &error)) << error;
    ASSERT_FALSE(registry.create_global(draft, &error));

    auto found = registry.find(acecode::path_to_utf8(temp.path), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Code Reviewer");
    EXPECT_EQ(found->source, "global");
    EXPECT_TRUE(found->managed_global);
    ASSERT_NE(found->selected_agent(), nullptr);
    EXPECT_EQ(found->selected_agent()->instructions, "Review code carefully.");

    draft.display_name = "Senior Reviewer";
    ASSERT_TRUE(registry.update_global("reviewer", draft, &error)) << error;
    found = registry.find(acecode::path_to_utf8(temp.path), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Senior Reviewer");

    ASSERT_TRUE(registry.delete_global("reviewer", &error)) << error;
    EXPECT_FALSE(registry.find(acecode::path_to_utf8(temp.path), "reviewer").has_value());
}

TEST(ExpertRegistry, ClosestWorkspaceExpertShadowsGlobalExpert) {
    TempDir temp;
    const fs::path workspace = temp.path / "project" / "nested";
    fs::create_directories(workspace);
    acecode::ExpertRegistry global_writer(temp.path / "global");
    std::string error;
    ASSERT_TRUE(global_writer.create_global(make_agent("reviewer", "Global"), &error)) << error;

    acecode::ExpertRegistry workspace_writer(temp.path / "project" / ".acecode" / "experts");
    ASSERT_TRUE(workspace_writer.create_global(make_agent("reviewer", "Workspace"), &error)) << error;

    acecode::ExpertRegistry registry(temp.path / "global");
    auto found = registry.find(acecode::path_to_utf8(workspace), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Workspace");
    EXPECT_EQ(found->source, "workspace");
    EXPECT_FALSE(found->managed_global);
}

TEST(ExpertRegistry, RejectsEscapedAgentAndSkillPaths) {
    TempDir temp;
    const fs::path root = temp.path / "global";
    const fs::path package = root / "unsafe";
    fs::create_directories(package);
    std::ofstream(temp.path / "outside.md") << "Do unsafe work";
    std::ofstream(package / "expert.json")
        << R"({"name":"unsafe","expertType":"agent","displayName":"Unsafe","agentName":"lead","agents":[{"id":"lead","path":"../../outside.md"}]})";

    acecode::ExpertRegistry registry(root);
    std::vector<acecode::ExpertDiagnostic> diagnostics;
    EXPECT_TRUE(registry.list(acecode::path_to_utf8(temp.path), &diagnostics).empty());
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics.front().message.find("escapes"), std::string::npos);
}

TEST(ExpertRegistry, TeamReferencesExistingExpertsWithoutCopyingDefinitions) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;
    ASSERT_TRUE(registry.create_global(make_agent("reviewer", "Reviewer"), &error)) << error;
    auto tester = make_agent("tester", "Tester");
    tester.profession = "QA";
    tester.lead.profession = "QA";
    tester.lead.instructions = "Test the implementation.";
    ASSERT_TRUE(registry.create_global(tester, &error)) << error;

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.description = "Ship reliable changes.";
    team.lead_expert_id = "reviewer";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(registry.create_global(team, &error)) << error;

    auto found = registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->references_existing_experts);
    EXPECT_EQ(found->lead_expert_id, "reviewer");
    EXPECT_EQ(found->member_expert_ids, std::vector<std::string>({"tester"}));
    EXPECT_TRUE(found->is_declared_member("tester"));
    EXPECT_FALSE(found->is_declared_member("intruder"));
    ASSERT_NE(found->selected_agent(), nullptr);
    EXPECT_EQ(found->selected_agent()->display_name, "Reviewer");
    ASSERT_NE(found->selected_agent("tester"), nullptr);
    EXPECT_EQ(found->selected_agent("tester")->profession, "QA");
    EXPECT_EQ(found->selected_agent("tester")->instructions,
              "Test the implementation.");

    std::ifstream manifest_input(
        temp.path / "global" / "delivery-team" / "expert.json");
    const auto manifest = nlohmann::json::parse(manifest_input);
    manifest_input.close();
    EXPECT_FALSE(manifest.contains("agents"));
    EXPECT_EQ(manifest["teamInfo"]["leadExpert"], "reviewer");
    EXPECT_EQ(manifest["teamInfo"]["memberExperts"][0], "tester");

    const fs::path stale_agents =
        temp.path / "global" / "delivery-team" / "agents";
    fs::create_directories(stale_agents);
    std::ofstream(stale_agents / "legacy-copy.md") << "Legacy member copy.";
    team.description = "Updated without copied member documents.";
    ASSERT_TRUE(registry.update_global("delivery-team", team, &error)) << error;
    EXPECT_FALSE(fs::exists(stale_agents));

    tester.display_name = "Senior Tester";
    tester.lead.instructions = "Run the current acceptance plan.";
    ASSERT_TRUE(registry.update_global("tester", tester, &error)) << error;
    found = registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    ASSERT_NE(found->selected_agent("tester"), nullptr);
    EXPECT_EQ(found->selected_agent("tester")->display_name, "Senior Tester");
    EXPECT_EQ(found->selected_agent("tester")->instructions,
              "Run the current acceptance plan.");
}

TEST(ExpertRegistry, ReferencedTeamKeepsEachExpertsOwnSkillRoots) {
    TempDir temp;
    const fs::path global_root = temp.path / "global";
    acecode::ExpertRegistry registry(global_root);
    std::string error;
    ASSERT_TRUE(registry.create_global(make_agent("reviewer", "Reviewer"), &error))
        << error;
    ASSERT_TRUE(registry.create_global(make_agent("tester", "Tester"), &error))
        << error;

    for (const std::string& id : {"reviewer", "tester"}) {
        const fs::path package = global_root / id;
        fs::create_directories(package / "skills");
        std::ifstream input(package / "expert.json");
        auto manifest = nlohmann::json::parse(input);
        input.close();
        manifest["skills"] = nlohmann::json::array({"skills"});
        std::ofstream(package / "expert.json") << manifest.dump(2) << '\n';
    }

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.lead_expert_id = "reviewer";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(registry.create_global(team, &error)) << error;

    const auto found =
        registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    const auto lead_roots = found->selected_skill_roots();
    const auto member_roots = found->selected_skill_roots("tester");
    ASSERT_EQ(lead_roots.size(), 1u);
    ASSERT_EQ(member_roots.size(), 1u);
    EXPECT_EQ(lead_roots.front(),
              fs::weakly_canonical(global_root / "reviewer" / "skills"));
    EXPECT_EQ(member_roots.front(),
              fs::weakly_canonical(global_root / "tester" / "skills"));
}

TEST(ExpertRegistry, RejectsMissingSelfNestedAndOutOfScopeTeamReferences) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    const fs::path other_workspace = temp.path / "other";
    fs::create_directories(workspace);
    fs::create_directories(other_workspace);
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    ASSERT_TRUE(registry.create_global(make_agent("reviewer", "Reviewer"), &error)) << error;
    acecode::ExpertDraft missing;
    missing.id = "missing-team";
    missing.type = acecode::ExpertType::Team;
    missing.display_name = "Missing Team";
    missing.lead_expert_id = "reviewer";
    missing.member_expert_ids = {"not-installed"};
    EXPECT_FALSE(registry.create_global(missing, &error));

    auto self = missing;
    self.id = "self-team";
    self.display_name = "Self Team";
    self.lead_expert_id = "self-team";
    self.member_expert_ids = {"reviewer"};
    EXPECT_FALSE(registry.create_global(self, &error));

    acecode::ExpertRegistry workspace_writer(workspace / ".acecode" / "experts");
    ASSERT_TRUE(workspace_writer.create_global(
        make_agent("workspace-tester", "Workspace Tester"), &error)) << error;
    acecode::ExpertDraft scoped;
    scoped.id = "scoped-team";
    scoped.type = acecode::ExpertType::Team;
    scoped.display_name = "Scoped Team";
    scoped.lead_expert_id = "reviewer";
    scoped.member_expert_ids = {"workspace-tester"};
    ASSERT_TRUE(registry.create_global(scoped, &error,
                                      acecode::path_to_utf8(workspace))) << error;
    EXPECT_TRUE(registry.find(acecode::path_to_utf8(workspace),
                              "scoped-team").has_value());
    EXPECT_FALSE(registry.find(acecode::path_to_utf8(other_workspace),
                               "scoped-team").has_value());

    ASSERT_TRUE(registry.create_global(
        make_agent("global-tester", "Global Tester"), &error)) << error;
    acecode::ExpertDraft base_team;
    base_team.id = "base-team";
    base_team.type = acecode::ExpertType::Team;
    base_team.display_name = "Base Team";
    base_team.lead_expert_id = "reviewer";
    base_team.member_expert_ids = {"global-tester"};
    ASSERT_TRUE(registry.create_global(base_team, &error)) << error;

    acecode::ExpertDraft nested = base_team;
    nested.id = "nested-team";
    nested.display_name = "Nested Team";
    nested.lead_expert_id = "base-team";
    EXPECT_FALSE(registry.create_global(nested, &error));
}

TEST(ExpertRegistry, DraftJsonRejectsInvalidIdentifiersAndToolFieldsDoNotMatter) {
    std::string error;
    auto invalid = acecode::ExpertRegistry::draft_from_json({
        {"id", "../bad"},
        {"display_name", "Bad"},
        {"instructions", "Bad instructions"},
    }, &error);
    EXPECT_FALSE(invalid.has_value());

    auto valid = acecode::ExpertRegistry::draft_from_json({
        {"id", "safe"},
        {"display_name", "Safe"},
        {"instructions", "Act safely."},
        {"tools", nlohmann::json::array({"shell", "admin"})},
    }, &error);
    ASSERT_TRUE(valid.has_value()) << error;
    EXPECT_EQ(valid->lead.instructions, "Act safely.");
}

TEST(ExpertRegistry, RejectsDraftsThatCannotRoundTrip) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    auto blank_instructions = make_agent("blank-instructions", "Blank Instructions");
    blank_instructions.lead.instructions = " \n\t ";
    EXPECT_FALSE(registry.create_global(blank_instructions, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "blank-instructions"));

    auto oversized_metadata = make_agent("oversized-metadata", "Oversized Metadata");
    oversized_metadata.lead.display_name = std::string(513, 'x');
    EXPECT_FALSE(registry.create_global(oversized_metadata, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "oversized-metadata"));

    auto invalid_utf8 = make_agent("invalid-utf8", "Invalid UTF-8");
    invalid_utf8.lead.instructions = std::string(1, static_cast<char>(0xC3));
    EXPECT_FALSE(registry.create_global(invalid_utf8, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "invalid-utf8"));
}

TEST(ExpertRegistry, SessionCreationBindsKnownExpertAndRejectsUnknownExpert) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    fs::create_directories(workspace);
    acecode::ExpertRegistry experts(temp.path / "global");
    std::string error;
    ASSERT_TRUE(experts.create_global(make_agent(), &error)) << error;

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = acecode::path_to_utf8(workspace);
    deps.expert_registry = &experts;
    deps.template_permissions = &permissions;
    acecode::SessionRegistry sessions(std::move(deps));

    acecode::SessionOptions options;
    options.cwd = acecode::path_to_utf8(workspace);
    options.expert_id = "reviewer";
    const std::string id = sessions.create(options);
    auto entry = sessions.acquire(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->expert.has_value());
    EXPECT_EQ(entry->expert_id, "reviewer");
    EXPECT_FALSE(entry->expert_missing);
    ASSERT_NE(entry->loop, nullptr);

    options.expert_id = "missing";
    EXPECT_THROW(sessions.create(options), std::invalid_argument);
    sessions.destroy(id);
}
