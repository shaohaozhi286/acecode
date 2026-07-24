#include "expert_registry.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace acecode {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxManifestBytes = 128 * 1024;
constexpr std::size_t kMaxAgentBytes = 256 * 1024;
constexpr std::size_t kMaxTextBytes = 64 * 1024;
constexpr std::size_t kMaxQuickPrompts = 24;
constexpr std::size_t kMaxAgents = 32;

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::optional<std::string> read_bounded_file(const fs::path& path,
                                             std::size_t limit,
                                             std::string* error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > limit) {
        set_error(error, ec ? "cannot read file size" : "file exceeds size limit");
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "cannot open file");
        return std::nullopt;
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (size > 0) input.read(content.data(), static_cast<std::streamsize>(size));
    if (!input && size > 0) {
        set_error(error, "cannot read file");
        return std::nullopt;
    }
    if (!is_valid_utf8(content)) {
        set_error(error, "file is not valid UTF-8");
        return std::nullopt;
    }
    return content;
}

std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string unquote(std::string value) {
    value = trim_copy(std::move(value));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

struct AgentDocument {
    std::string body;
    std::map<std::string, std::string> frontmatter;
};

AgentDocument parse_agent_document(const std::string& content) {
    AgentDocument result;
    if (content.rfind("---", 0) != 0) {
        result.body = trim_copy(content);
        return result;
    }

    const auto first_line_end = content.find('\n');
    if (first_line_end == std::string::npos) {
        result.body = trim_copy(content);
        return result;
    }
    const auto closing = content.find("\n---", first_line_end);
    if (closing == std::string::npos) {
        result.body = trim_copy(content);
        return result;
    }

    std::istringstream metadata(content.substr(first_line_end + 1,
                                                closing - first_line_end - 1));
    std::string line;
    while (std::getline(metadata, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim_copy(line.substr(0, colon));
        if (key.empty()) continue;
        result.frontmatter[key] = unquote(line.substr(colon + 1));
    }

    std::size_t body_start = closing + 4;
    if (body_start < content.size() && content[body_start] == '\r') ++body_start;
    if (body_start < content.size() && content[body_start] == '\n') ++body_start;
    result.body = trim_copy(content.substr(body_start));
    return result;
}

std::string localized_string(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_object()) return {};
    for (const char* key : {"zh-CN", "zh_CN", "en-US", "en_US", "default"}) {
        const auto it = value.find(key);
        if (it != value.end() && it->is_string()) return it->get<std::string>();
    }
    for (const auto& [key, item] : value.items()) {
        (void)key;
        if (item.is_string()) return item.get<std::string>();
    }
    return {};
}

std::string json_text(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? std::string{} : localized_string(*it);
}

bool is_contained_existing_path(const fs::path& root,
                                const fs::path& relative,
                                bool require_directory,
                                fs::path* resolved,
                                std::string* error) {
    if (relative.empty() || relative.is_absolute()) {
        set_error(error, "package path must be relative");
        return false;
    }

    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec) {
        set_error(error, "cannot resolve package root");
        return false;
    }
    const fs::path canonical_target = fs::weakly_canonical(root / relative, ec);
    if (ec) {
        set_error(error, "cannot resolve package path");
        return false;
    }
    const fs::path relation = fs::relative(canonical_target, canonical_root, ec);
    if (ec || relation.empty() || relation == "." ||
        *relation.begin() == "..") {
        set_error(error, "package path escapes its package root");
        return false;
    }
    if (require_directory ? !fs::is_directory(canonical_target, ec)
                          : !fs::is_regular_file(canonical_target, ec)) {
        set_error(error, require_directory ? "package directory does not exist"
                                           : "package file does not exist");
        return false;
    }
    if (resolved) *resolved = canonical_target;
    return true;
}

std::string path_error_prefix(const fs::path& path, const std::string& error) {
    return path_to_utf8(path) + ": " + error;
}

std::optional<ExpertDefinition> load_package(const fs::path& package_root,
                                             const std::string& source,
                                             bool managed,
                                             std::string* error) {
    const fs::path manifest_path = package_root / "expert.json";
    auto raw = read_bounded_file(manifest_path, kMaxManifestBytes, error);
    if (!raw) return std::nullopt;

    json manifest;
    try {
        manifest = json::parse(*raw);
    } catch (const std::exception& ex) {
        set_error(error, std::string("invalid expert.json: ") + ex.what());
        return std::nullopt;
    }
    if (!manifest.is_object()) {
        set_error(error, "expert.json must contain an object");
        return std::nullopt;
    }

    ExpertDefinition expert;
    expert.id = json_text(manifest, "name");
    if (!ExpertRegistry::valid_id(expert.id)) {
        set_error(error, "manifest name is not a valid expert ID");
        return std::nullopt;
    }
    if (package_root.filename() != path_from_utf8(expert.id)) {
        set_error(error, "package directory name must match manifest name");
        return std::nullopt;
    }

    expert.version = json_text(manifest, "version");
    if (expert.version.empty()) expert.version = "1.0.0";
    const std::string type = json_text(manifest, "expertType");
    if (type == "agent") expert.type = ExpertType::Agent;
    else if (type == "team") expert.type = ExpertType::Team;
    else {
        set_error(error, "expertType must be agent or team");
        return std::nullopt;
    }
    expert.display_name = json_text(manifest, "displayName");
    if (expert.display_name.empty()) expert.display_name = expert.id;
    expert.profession = json_text(manifest, "profession");
    expert.description = json_text(manifest, "displayDescription");
    expert.default_init_prompt = json_text(manifest, "defaultInitPrompt");
    expert.avatar_path = json_text(manifest, "avatar");
    expert.package_root = package_root;
    expert.source = source;
    expert.managed_global = managed;

    if (expert.display_name.size() > 512 || expert.profession.size() > 512 ||
        expert.description.size() > kMaxTextBytes ||
        expert.default_init_prompt.size() > kMaxTextBytes) {
        set_error(error, "expert display text exceeds size limit");
        return std::nullopt;
    }

    const auto quick_it = manifest.find("quickPrompts");
    if (quick_it != manifest.end()) {
        if (!quick_it->is_array() || quick_it->size() > kMaxQuickPrompts) {
            set_error(error, "quickPrompts must be a bounded array");
            return std::nullopt;
        }
        for (const auto& prompt : *quick_it) {
            const std::string text = localized_string(prompt);
            if (text.empty() || text.size() > 4096) {
                set_error(error, "quick prompt is invalid or too large");
                return std::nullopt;
            }
            expert.quick_prompts.push_back(text);
        }
    }

    const json* team_info = nullptr;
    if (expert.type == ExpertType::Team) {
        const auto team_it = manifest.find("teamInfo");
        if (team_it == manifest.end() || !team_it->is_object()) {
            set_error(error, "team expert requires teamInfo");
            return std::nullopt;
        }
        team_info = &*team_it;
        expert.lead_expert_id = json_text(*team_info, "leadExpert");
        expert.references_existing_experts = !expert.lead_expert_id.empty();
        if (expert.references_existing_experts) {
            if (!ExpertRegistry::valid_id(expert.lead_expert_id) ||
                expert.lead_expert_id == expert.id) {
                set_error(error, "team lead expert reference is invalid");
                return std::nullopt;
            }
            const auto member_it = team_info->find("memberExperts");
            if (member_it == team_info->end() || !member_it->is_array() ||
                member_it->empty() || member_it->size() + 1 > kMaxAgents) {
                set_error(error, "teamInfo.memberExperts must be a non-empty bounded array");
                return std::nullopt;
            }
            std::unordered_set<std::string> member_ids;
            for (const auto& item : *member_it) {
                const std::string id = localized_string(item);
                if (!ExpertRegistry::valid_id(id) || id == expert.id ||
                    id == expert.lead_expert_id || !member_ids.insert(id).second) {
                    set_error(error, "team expert reference is invalid or duplicated");
                    return std::nullopt;
                }
                expert.member_expert_ids.push_back(id);
            }
            expert.lead_agent_id = expert.lead_expert_id;
            expert.member_agent_ids = expert.member_expert_ids;
        }
    }

    if (!expert.references_existing_experts) {
        const auto agents_it = manifest.find("agents");
        if (agents_it == manifest.end() || !agents_it->is_array() ||
            agents_it->empty() || agents_it->size() > kMaxAgents) {
            set_error(error, "agents must be a non-empty bounded array");
            return std::nullopt;
        }
        std::unordered_set<std::string> agent_ids;
        for (const auto& entry : *agents_it) {
            std::string id;
            std::string raw_path;
            std::string display_name;
            std::string profession;
            if (entry.is_string()) {
                raw_path = entry.get<std::string>();
                id = path_from_utf8(raw_path).stem().string();
            } else if (entry.is_object()) {
                id = json_text(entry, "id");
                raw_path = json_text(entry, "path");
                display_name = json_text(entry, "displayName");
                profession = json_text(entry, "profession");
            } else {
                set_error(error, "agent entry must be a path or object");
                return std::nullopt;
            }
            if (!ExpertRegistry::valid_id(id) || !agent_ids.insert(id).second) {
                set_error(error, "agent ID is invalid or duplicated");
                return std::nullopt;
            }
            fs::path agent_path;
            std::string path_error;
            if (!is_contained_existing_path(package_root, path_from_utf8(raw_path),
                                            false, &agent_path, &path_error)) {
                set_error(error, "invalid Agent path for " + id + ": " + path_error);
                return std::nullopt;
            }
            auto document_text = read_bounded_file(agent_path, kMaxAgentBytes, &path_error);
            if (!document_text) {
                set_error(error, "invalid Agent document for " + id + ": " + path_error);
                return std::nullopt;
            }
            AgentDocument document = parse_agent_document(*document_text);
            if (document.body.empty()) {
                set_error(error, "Agent document has no instruction body: " + id);
                return std::nullopt;
            }
            if (display_name.empty()) {
                const auto it = document.frontmatter.find("displayName");
                if (it != document.frontmatter.end()) display_name = it->second;
            }
            if (display_name.empty()) display_name = id;
            if (profession.empty()) {
                const auto it = document.frontmatter.find("profession");
                if (it != document.frontmatter.end()) profession = it->second;
            }
            if (display_name.size() > 512 || profession.size() > 512) {
                set_error(error, "Agent display text exceeds size limit: " + id);
                return std::nullopt;
            }
            expert.agents.push_back({id, display_name, profession,
                                     std::move(document.body), std::move(agent_path), {}});
        }

        expert.lead_agent_id = json_text(manifest, "agentName");
        if (expert.type == ExpertType::Team) {
            const std::string declared_lead = json_text(*team_info, "leadAgent");
            if (!declared_lead.empty()) expert.lead_agent_id = declared_lead;
            const auto member_it = team_info->find("memberAgents");
            if (member_it == team_info->end() || !member_it->is_array() ||
                member_it->empty()) {
                set_error(error, "teamInfo.memberAgents must be a non-empty array");
                return std::nullopt;
            }
            std::unordered_set<std::string> member_ids;
            for (const auto& item : *member_it) {
                const std::string id = localized_string(item);
                if (!ExpertRegistry::valid_id(id) || id == expert.lead_agent_id ||
                    !member_ids.insert(id).second) {
                    set_error(error, "team member ID is invalid or duplicated");
                    return std::nullopt;
                }
                expert.member_agent_ids.push_back(id);
            }
        }
        if (expert.lead_agent_id.empty()) expert.lead_agent_id = expert.agents.front().id;
        if (!expert.agent(expert.lead_agent_id)) {
            set_error(error, "lead Agent is not declared in agents");
            return std::nullopt;
        }
        for (const auto& member_id : expert.member_agent_ids) {
            if (!expert.agent(member_id)) {
                set_error(error, "team member is not declared in agents: " + member_id);
                return std::nullopt;
            }
        }
    }

    const auto skills_it = manifest.find("skills");
    if (skills_it != manifest.end()) {
        if (!skills_it->is_array()) {
            set_error(error, "skills must be an array of contained directories");
            return std::nullopt;
        }
        for (const auto& item : *skills_it) {
            if (!item.is_string()) {
                set_error(error, "Skill path must be a string");
                return std::nullopt;
            }
            fs::path root;
            std::string path_error;
            if (!is_contained_existing_path(package_root,
                                            path_from_utf8(item.get<std::string>()),
                                            true, &root, &path_error)) {
                set_error(error, "invalid Skill root: " + path_error);
                return std::nullopt;
            }
            expert.skill_roots.push_back(std::move(root));
        }
    }

    if (!expert.avatar_path.empty()) {
        fs::path avatar;
        std::string path_error;
        if (!is_contained_existing_path(package_root,
                                        path_from_utf8(expert.avatar_path),
                                        false, &avatar, &path_error)) {
            set_error(error, "invalid avatar path: " + path_error);
            return std::nullopt;
        }
        expert.avatar_path = path_to_utf8(avatar);
    }
    return expert;
}

std::vector<fs::path> sorted_package_dirs(const fs::path& root) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return result;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end(), [](const fs::path& lhs, const fs::path& rhs) {
        return path_to_utf8(lhs.filename()) < path_to_utf8(rhs.filename());
    });
    return result;
}

std::string agent_markdown(const ExpertAgentDraft& agent);
json draft_manifest(const ExpertDraft& draft);

bool validate_draft(const ExpertDraft& draft, std::string* error) {
    if (!ExpertRegistry::valid_id(draft.id)) {
        set_error(error, "expert ID must use lowercase letters, digits, and hyphens");
        return false;
    }
    if (draft.version.size() > 128 ||
        draft.display_name.empty() || draft.display_name.size() > 512 ||
        draft.profession.size() > 512 || draft.description.size() > kMaxTextBytes ||
        draft.default_init_prompt.size() > kMaxTextBytes) {
        set_error(error, "expert display fields are missing or too large");
        return false;
    }
    if (!is_valid_utf8(draft.version) || !is_valid_utf8(draft.display_name) ||
        !is_valid_utf8(draft.profession) ||
        !is_valid_utf8(draft.description) || !is_valid_utf8(draft.default_init_prompt)) {
        set_error(error, "expert text must be valid UTF-8");
        return false;
    }
    auto valid_agent = [](const ExpertAgentDraft& agent) {
        return ExpertRegistry::valid_id(agent.id) &&
               !trim_copy(agent.instructions).empty() &&
               agent.display_name.size() <= 512 && agent.profession.size() <= 512 &&
               is_valid_utf8(agent.display_name) && is_valid_utf8(agent.profession) &&
               is_valid_utf8(agent.instructions) &&
               agent_markdown(agent).size() <= kMaxAgentBytes;
    };
    if (draft.type == ExpertType::Agent) {
        if (!valid_agent(draft.lead)) {
            set_error(error, "lead Agent ID or instructions are invalid");
            return false;
        }
        if (!draft.lead_expert_id.empty() || !draft.member_expert_ids.empty()) {
            set_error(error, "single expert cannot declare team references");
            return false;
        }
    } else {
        if (!ExpertRegistry::valid_id(draft.lead_expert_id) ||
            draft.lead_expert_id == draft.id ||
            draft.member_expert_ids.empty() ||
            draft.member_expert_ids.size() + 1 > kMaxAgents) {
            set_error(error, "expert team requires a valid existing lead and collaborators");
            return false;
        }
        std::unordered_set<std::string> ids{draft.lead_expert_id};
        for (const auto& member_id : draft.member_expert_ids) {
            if (!ExpertRegistry::valid_id(member_id) || member_id == draft.id ||
                !ids.insert(member_id).second) {
                set_error(error, "expert team reference is invalid or duplicated");
                return false;
            }
        }
    }
    if (draft.quick_prompts.size() > kMaxQuickPrompts) {
        set_error(error, "expert has too many quick prompts");
        return false;
    }
    for (const auto& prompt : draft.quick_prompts) {
        if (prompt.empty() || prompt.size() > 4096 || !is_valid_utf8(prompt)) {
            set_error(error, "quick prompt is invalid or too large");
            return false;
        }
    }
    if (draft_manifest(draft).dump().size() > kMaxManifestBytes) {
        set_error(error, "generated expert manifest exceeds size limit");
        return false;
    }
    return true;
}

std::string agent_markdown(const ExpertAgentDraft& agent) {
    std::ostringstream out;
    out << "---\nname: " << agent.id << "\n";
    if (!agent.display_name.empty()) out << "displayName: " << agent.display_name << "\n";
    if (!agent.profession.empty()) out << "profession: " << agent.profession << "\n";
    out << "---\n\n" << trim_copy(agent.instructions) << "\n";
    return out.str();
}

json draft_manifest(const ExpertDraft& draft) {
    json result = {
        {"name", draft.id},
        {"version", draft.version.empty() ? "1.0.0" : draft.version},
        {"expertType", to_string(draft.type)},
        {"displayName", draft.display_name},
        {"profession", draft.profession},
        {"displayDescription", draft.description},
        {"quickPrompts", draft.quick_prompts},
        {"defaultInitPrompt", draft.default_init_prompt},
    };
    if (draft.type == ExpertType::Agent) {
        result["agentName"] = draft.lead.id;
        result["agents"] = json::array({
            {
                {"id", draft.lead.id},
                {"path", "agents/" + draft.lead.id + ".md"},
                {"displayName", draft.lead.display_name.empty()
                                    ? draft.lead.id
                                    : draft.lead.display_name},
                {"profession", draft.lead.profession},
            },
        });
    } else {
        result["teamInfo"] = {
            {"leadExpert", draft.lead_expert_id},
            {"memberExperts", draft.member_expert_ids},
        };
    }
    return result;
}

bool materialize_draft(const fs::path& root,
                       const ExpertDraft& draft,
                       std::string* error) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        set_error(error, "cannot create expert staging directory");
        return false;
    }
    if (draft.type == ExpertType::Agent) {
        fs::create_directories(root / "agents", ec);
        if (ec) {
            set_error(error, "cannot create Agent staging directory");
            return false;
        }
        if (!atomic_write_file(
                path_to_utf8(root / "agents" / (draft.lead.id + ".md")),
                agent_markdown(draft.lead))) {
            set_error(error, "cannot write lead Agent document");
            return false;
        }
    } else {
        fs::remove_all(root / "agents", ec);
        if (ec) {
            set_error(error, "cannot remove copied Agent documents from team package");
            return false;
        }
    }
    json manifest = draft_manifest(draft);
    const fs::path existing_manifest_path = root / "expert.json";
    if (fs::is_regular_file(existing_manifest_path, ec)) {
        std::string preserved_error;
        if (auto existing_text =
                read_bounded_file(existing_manifest_path, kMaxManifestBytes,
                                  &preserved_error)) {
            try {
                const json existing = json::parse(*existing_text);
                for (const char* field : {"skills", "avatar"}) {
                    const auto it = existing.find(field);
                    if (it != existing.end()) manifest[field] = *it;
                }
            } catch (...) {
                // The newly validated managed fields still replace malformed metadata.
            }
        }
    }
    const std::string manifest_text = manifest.dump(2) + "\n";
    if (manifest_text.size() > kMaxManifestBytes) {
        set_error(error, "expert manifest exceeds size limit");
        return false;
    }
    if (!atomic_write_file(path_to_utf8(existing_manifest_path), manifest_text)) {
        set_error(error, "cannot write expert manifest");
        return false;
    }
    return true;
}

std::string staging_suffix() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return ".tmp-" + std::to_string(value);
}

bool replace_package(const fs::path& global_root,
                     const std::string& id,
                     const ExpertDraft& draft,
                     bool require_existing,
                     std::string* error) {
    std::error_code ec;
    fs::create_directories(global_root, ec);
    if (ec) {
        set_error(error, "cannot create global expert root");
        return false;
    }
    const fs::path target = global_root / path_from_utf8(id);
    const bool exists = fs::exists(target, ec);
    if (ec || exists != require_existing) {
        set_error(error, require_existing ? "global expert does not exist"
                                          : "global expert already exists");
        return false;
    }

    const std::string suffix = staging_suffix();
    const fs::path staging = global_root / path_from_utf8("." + id + suffix);
    const fs::path backup = global_root / path_from_utf8("." + id + ".backup" + suffix);
    if (require_existing) {
        fs::copy(target, staging, fs::copy_options::recursive, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            set_error(error, "cannot preserve existing expert package assets");
            return false;
        }
    }
    if (!materialize_draft(staging, draft, error)) {
        fs::remove_all(staging, ec);
        return false;
    }

    if (require_existing) {
        fs::rename(target, backup, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            set_error(error, "cannot stage existing expert for update");
            return false;
        }
    }
    fs::rename(staging, target, ec);
    if (ec) {
        if (require_existing) {
            std::error_code rollback;
            fs::rename(backup, target, rollback);
        }
        fs::remove_all(staging, ec);
        set_error(error, "cannot install expert package");
        return false;
    }
    if (require_existing) fs::remove_all(backup, ec);
    return true;
}

std::optional<ExpertAgentDraft> agent_draft_from_json(const json& value,
                                                      std::string* error) {
    if (!value.is_object()) {
        set_error(error, "Agent definition must be an object");
        return std::nullopt;
    }
    ExpertAgentDraft draft;
    draft.id = json_text(value, "id");
    draft.display_name = json_text(value, "display_name");
    if (draft.display_name.empty()) draft.display_name = json_text(value, "displayName");
    draft.profession = json_text(value, "profession");
    draft.instructions = json_text(value, "instructions");
    return draft;
}

bool validate_team_reference_targets(
    const ExpertDraft& draft,
    const std::vector<ExpertDefinition>& effective_experts,
    std::string* error) {
    if (draft.type != ExpertType::Team) return true;
    std::unordered_map<std::string, const ExpertDefinition*> by_id;
    for (const auto& expert : effective_experts) by_id.emplace(expert.id, &expert);

    auto validate_one = [&](const std::string& id) {
        const auto it = by_id.find(id);
        if (it == by_id.end()) {
            set_error(error, "selected expert is unavailable: " + id);
            return false;
        }
        if (it->second->type != ExpertType::Agent) {
            set_error(error, "expert teams can only contain single experts: " + id);
            return false;
        }
        return true;
    };
    if (!validate_one(draft.lead_expert_id)) return false;
    for (const auto& id : draft.member_expert_ids) {
        if (!validate_one(id)) return false;
    }
    return true;
}

} // namespace

const ExpertAgent* ExpertDefinition::agent(const std::string& agent_id) const {
    const auto it = std::find_if(agents.begin(), agents.end(), [&](const ExpertAgent& item) {
        return item.id == agent_id;
    });
    return it == agents.end() ? nullptr : &*it;
}

const ExpertAgent* ExpertDefinition::selected_agent(const std::string& member_id) const {
    return agent(member_id.empty() ? lead_agent_id : member_id);
}

std::vector<fs::path> ExpertDefinition::selected_skill_roots(
    const std::string& member_id) const {
    std::vector<fs::path> roots = skill_roots;
    if (const ExpertAgent* selected = selected_agent(member_id)) {
        for (const auto& root : selected->skill_roots) {
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
                roots.push_back(root);
            }
        }
    }
    return roots;
}

bool ExpertDefinition::is_declared_member(const std::string& member_id) const {
    return type == ExpertType::Team &&
           std::find(member_agent_ids.begin(), member_agent_ids.end(), member_id) !=
               member_agent_ids.end();
}

const char* to_string(ExpertType type) {
    return type == ExpertType::Team ? "team" : "agent";
}

ExpertRegistry::ExpertRegistry(fs::path global_root)
    : global_root_(global_root.empty()
                       ? path_from_utf8(get_acecode_dir()) / "experts"
                       : std::move(global_root)) {}

bool ExpertRegistry::valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    });
}

std::vector<ExpertDefinition> ExpertRegistry::list(
    const std::string& working_dir,
    std::vector<ExpertDiagnostic>* diagnostics) const {
    struct Root {
        fs::path path;
        std::string source;
        bool managed;
    };
    std::vector<Root> roots;
    if (!working_dir.empty()) {
        for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
            roots.push_back({path_from_utf8(dir) / ".acecode" / "experts",
                             "workspace", false});
        }
    }
    roots.push_back({global_root_, "global", true});

    std::vector<ExpertDefinition> result;
    std::unordered_set<std::string> seen;
    for (const auto& root : roots) {
        for (const auto& package : sorted_package_dirs(root.path)) {
            std::string error;
            auto expert = load_package(package, root.source, root.managed, &error);
            if (!expert) {
                if (diagnostics) {
                    diagnostics->push_back({path_to_utf8(package), std::move(error)});
                }
                continue;
            }
            if (seen.insert(expert->id).second) result.push_back(std::move(*expert));
        }
    }

    std::unordered_map<std::string, const ExpertDefinition*> by_id;
    for (const auto& expert : result) by_id.emplace(expert.id, &expert);
    std::unordered_set<std::string> invalid_teams;
    for (auto& expert : result) {
        if (!expert.references_existing_experts) continue;

        std::string reference_error;
        auto append_reference = [&](const std::string& id) {
            const auto it = by_id.find(id);
            if (it == by_id.end()) {
                reference_error = "referenced expert is unavailable: " + id;
                return false;
            }
            const ExpertDefinition& referenced = *it->second;
            if (referenced.type != ExpertType::Agent) {
                reference_error = "expert teams cannot contain another team: " + id;
                return false;
            }
            const ExpertAgent* selected = referenced.selected_agent();
            if (!selected) {
                reference_error = "referenced expert has no active Agent: " + id;
                return false;
            }
            ExpertAgent projected = *selected;
            projected.id = referenced.id;
            projected.display_name = referenced.display_name;
            if (!referenced.profession.empty()) {
                projected.profession = referenced.profession;
            }
            projected.skill_roots = referenced.selected_skill_roots();
            expert.agents.push_back(std::move(projected));
            return true;
        };

        expert.agents.clear();
        if (!append_reference(expert.lead_expert_id)) {
            invalid_teams.insert(expert.id);
        } else {
            for (const auto& member_id : expert.member_expert_ids) {
                if (!append_reference(member_id)) {
                    invalid_teams.insert(expert.id);
                    break;
                }
            }
        }
        if (invalid_teams.count(expert.id) != 0 && diagnostics) {
            diagnostics->push_back({path_to_utf8(expert.package_root),
                                    std::move(reference_error)});
        }
    }
    if (!invalid_teams.empty()) {
        result.erase(std::remove_if(result.begin(), result.end(),
                                    [&](const ExpertDefinition& expert) {
                                        return invalid_teams.count(expert.id) != 0;
                                    }),
                     result.end());
    }
    std::sort(result.begin(), result.end(), [](const ExpertDefinition& lhs,
                                               const ExpertDefinition& rhs) {
        return lhs.display_name < rhs.display_name;
    });
    return result;
}

std::optional<ExpertDefinition> ExpertRegistry::find(
    const std::string& working_dir,
    const std::string& id,
    std::vector<ExpertDiagnostic>* diagnostics) const {
    if (!valid_id(id)) return std::nullopt;
    auto experts = list(working_dir, diagnostics);
    auto it = std::find_if(experts.begin(), experts.end(), [&](const ExpertDefinition& item) {
        return item.id == id;
    });
    if (it == experts.end()) return std::nullopt;
    return std::move(*it);
}

bool ExpertRegistry::create_global(const ExpertDraft& draft,
                                   std::string* error,
                                   const std::string& working_dir) const {
    if (!validate_draft(draft, error)) return false;
    if (!validate_team_reference_targets(draft, list(working_dir), error)) return false;
    return replace_package(global_root_, draft.id, draft, false, error);
}

bool ExpertRegistry::update_global(const std::string& id,
                                   const ExpertDraft& draft,
                                   std::string* error,
                                   const std::string& working_dir) const {
    if (!valid_id(id) || draft.id != id) {
        set_error(error, "expert ID cannot be changed during update");
        return false;
    }
    if (!validate_draft(draft, error)) return false;
    if (!validate_team_reference_targets(draft, list(working_dir), error)) return false;
    return replace_package(global_root_, id, draft, true, error);
}

bool ExpertRegistry::delete_global(const std::string& id, std::string* error) const {
    if (!valid_id(id)) {
        set_error(error, "invalid expert ID");
        return false;
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(global_root_, ec);
    if (ec) {
        set_error(error, "global expert root does not exist");
        return false;
    }
    const fs::path target = fs::weakly_canonical(global_root_ / path_from_utf8(id), ec);
    if (ec || target.parent_path() != root || !fs::is_directory(target, ec)) {
        set_error(error, "global expert does not exist or is outside the managed root");
        return false;
    }
    const auto loaded = load_package(target, "global", true, error);
    if (!loaded || loaded->id != id) return false;
    const auto removed = fs::remove_all(target, ec);
    if (ec || removed == 0) {
        set_error(error, "cannot delete global expert");
        return false;
    }
    return true;
}

std::optional<ExpertDraft> ExpertRegistry::draft_from_json(const json& value,
                                                           std::string* error) {
    if (!value.is_object()) {
        set_error(error, "request body must be an object");
        return std::nullopt;
    }
    ExpertDraft draft;
    draft.id = json_text(value, "id");
    if (draft.id.empty()) draft.id = json_text(value, "name");
    draft.version = json_text(value, "version");
    if (draft.version.empty()) draft.version = "1.0.0";
    const std::string type = json_text(value, "type").empty()
                                 ? json_text(value, "expertType")
                                 : json_text(value, "type");
    if (type.empty() || type == "agent") draft.type = ExpertType::Agent;
    else if (type == "team") draft.type = ExpertType::Team;
    else {
        set_error(error, "type must be agent or team");
        return std::nullopt;
    }
    draft.display_name = json_text(value, "display_name");
    if (draft.display_name.empty()) draft.display_name = json_text(value, "displayName");
    draft.profession = json_text(value, "profession");
    draft.description = json_text(value, "description");
    if (draft.description.empty()) draft.description = json_text(value, "displayDescription");
    draft.default_init_prompt = json_text(value, "default_init_prompt");
    if (draft.default_init_prompt.empty()) {
        draft.default_init_prompt = json_text(value, "defaultInitPrompt");
    }
    const auto prompts = value.find("quick_prompts") != value.end()
                             ? value.find("quick_prompts")
                             : value.find("quickPrompts");
    if (prompts != value.end()) {
        if (!prompts->is_array()) {
            set_error(error, "quick_prompts must be an array");
            return std::nullopt;
        }
        for (const auto& item : *prompts) {
            if (!item.is_string()) {
                set_error(error, "quick prompt must be a string");
                return std::nullopt;
            }
            draft.quick_prompts.push_back(item.get<std::string>());
        }
    }

    if (draft.type == ExpertType::Agent) {
        const auto lead_it = value.find("lead");
        if (lead_it != value.end()) {
            auto lead = agent_draft_from_json(*lead_it, error);
            if (!lead) return std::nullopt;
            draft.lead = std::move(*lead);
        } else {
            draft.lead.id = json_text(value, "agent_id");
            if (draft.lead.id.empty()) draft.lead.id = "lead";
            draft.lead.display_name = draft.display_name;
            draft.lead.profession = draft.profession;
            draft.lead.instructions = json_text(value, "instructions");
        }
    } else {
        draft.lead_expert_id = json_text(value, "lead_expert_id");
        if (draft.lead_expert_id.empty()) {
            draft.lead_expert_id = json_text(value, "leadExpert");
        }
        auto members_it = value.find("member_expert_ids");
        if (members_it == value.end()) members_it = value.find("memberExpertIds");
        if (members_it == value.end()) members_it = value.find("memberExperts");
        if (members_it == value.end()) members_it = value.find("members");
        if (members_it == value.end() || !members_it->is_array()) {
            set_error(error, "member_expert_ids must be an array");
            return std::nullopt;
        }
        for (const auto& item : *members_it) {
            if (!item.is_string()) {
                set_error(error, "team members must reference existing expert IDs");
                return std::nullopt;
            }
            draft.member_expert_ids.push_back(item.get<std::string>());
        }
    }
    if (!validate_draft(draft, error)) return std::nullopt;
    return draft;
}

json expert_definition_to_json(const ExpertDefinition& expert,
                               bool include_instructions) {
    json agents = json::array();
    for (const auto& agent : expert.agents) {
        json item = {
            {"id", agent.id},
            {"display_name", agent.display_name},
            {"profession", agent.profession},
        };
        if (include_instructions) item["instructions"] = agent.instructions;
        agents.push_back(std::move(item));
    }
    json skills = json::array();
    for (const auto& root : expert.skill_roots) skills.push_back(path_to_utf8(root));
    return {
        {"id", expert.id},
        {"version", expert.version},
        {"type", to_string(expert.type)},
        {"display_name", expert.display_name},
        {"profession", expert.profession},
        {"description", expert.description},
        {"avatar_path", expert.avatar_path},
        {"default_init_prompt", expert.default_init_prompt},
        {"quick_prompts", expert.quick_prompts},
        {"lead_agent_id", expert.lead_agent_id},
        {"member_agent_ids", expert.member_agent_ids},
        {"references_existing_experts", expert.references_existing_experts},
        {"lead_expert_id", expert.lead_expert_id},
        {"member_expert_ids", expert.member_expert_ids},
        {"agents", std::move(agents)},
        {"skill_roots", std::move(skills)},
        {"source", expert.source},
        {"managed_global", expert.managed_global},
        {"package_root", path_to_utf8(expert.package_root)},
    };
}

} // namespace acecode
