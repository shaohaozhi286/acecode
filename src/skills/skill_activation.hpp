#pragma once

#include "skill_metadata.hpp"

#include <string>
#include <vector>

namespace acecode {

// Build the user-facing message text injected into the agent when a user types
// `/<skill-name> [args]`. Output layout mirrors hermes' activation note:
//
//   [SYSTEM: The user has invoked the "<name>" skill ...]
//
//   <SKILL.md body>
//
//   [This skill has supporting files you can load with the skill_view tool:]
//   - references/api.md
//   - ...
//   To view any of these, use: skill_view(name="<name>", file_path="<path>")
//
//   The user has provided the following instruction alongside the skill invocation: <user_arg>
//
// Any section whose source material is empty is omitted. `body` should already
// be the post-frontmatter SKILL.md content.
std::string build_activation_message(const SkillMetadata& meta,
                                     const std::string& body,
                                     const std::vector<std::string>& supporting_files,
                                     const std::string& user_arg);

} // namespace acecode
