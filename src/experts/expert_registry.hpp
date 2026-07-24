#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

enum class ExpertType {
    Agent,
    Team,
};

struct ExpertAgent {
    std::string id;
    std::string display_name;
    std::string profession;
    std::string instructions;
    std::filesystem::path document_path;
    std::vector<std::filesystem::path> skill_roots;
};

struct ExpertDefinition {
    std::string id;
    std::string version;
    ExpertType type = ExpertType::Agent;
    std::string display_name;
    std::string profession;
    std::string description;
    std::string avatar_path;
    std::string default_init_prompt;
    std::vector<std::string> quick_prompts;

    std::string lead_agent_id;
    std::vector<ExpertAgent> agents;
    std::vector<std::string> member_agent_ids;
    std::vector<std::filesystem::path> skill_roots;
    bool references_existing_experts = false;
    std::string lead_expert_id;
    std::vector<std::string> member_expert_ids;

    std::filesystem::path package_root;
    std::string source; // "workspace" or "global"
    bool managed_global = false;

    const ExpertAgent* agent(const std::string& agent_id) const;
    const ExpertAgent* selected_agent(const std::string& member_id = {}) const;
    std::vector<std::filesystem::path> selected_skill_roots(
        const std::string& member_id = {}) const;
    bool is_declared_member(const std::string& member_id) const;
};

struct ExpertDiagnostic {
    std::string path;
    std::string message;
};

struct ExpertAgentDraft {
    std::string id;
    std::string display_name;
    std::string profession;
    std::string instructions;
};

struct ExpertDraft {
    std::string id;
    std::string version = "1.0.0";
    ExpertType type = ExpertType::Agent;
    std::string display_name;
    std::string profession;
    std::string description;
    std::string default_init_prompt;
    std::vector<std::string> quick_prompts;
    ExpertAgentDraft lead;
    std::string lead_expert_id;
    std::vector<std::string> member_expert_ids;
};

class ExpertRegistry {
public:
    // Empty uses ~/.acecode/experts. Tests can inject a contained temporary root.
    explicit ExpertRegistry(std::filesystem::path global_root = {});

    const std::filesystem::path& global_root() const { return global_root_; }

    std::vector<ExpertDefinition> list(
        const std::string& working_dir,
        std::vector<ExpertDiagnostic>* diagnostics = nullptr) const;

    std::optional<ExpertDefinition> find(
        const std::string& working_dir,
        const std::string& id,
        std::vector<ExpertDiagnostic>* diagnostics = nullptr) const;

    bool create_global(const ExpertDraft& draft,
                       std::string* error = nullptr,
                       const std::string& working_dir = {}) const;
    bool update_global(const std::string& id,
                       const ExpertDraft& draft,
                       std::string* error = nullptr,
                       const std::string& working_dir = {}) const;
    bool delete_global(const std::string& id, std::string* error = nullptr) const;

    static bool valid_id(const std::string& id);
    static std::optional<ExpertDraft> draft_from_json(
        const nlohmann::json& value,
        std::string* error = nullptr);

private:
    std::filesystem::path global_root_;
};

nlohmann::json expert_definition_to_json(const ExpertDefinition& expert,
                                         bool include_instructions = false);

const char* to_string(ExpertType type);

} // namespace acecode
