#include "skill_activation.hpp"

#include <cctype>
#include <sstream>

namespace acecode {

namespace {

std::string strip(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

} // namespace

std::string build_skill_invocation_hint(const SkillMetadata& meta,
                                         const std::string& args) {
    std::ostringstream oss;

    oss << "[SYSTEM: User invoked /" << meta.name << " skill]";

    std::string trimmed_desc = strip(meta.description);
    if (!trimmed_desc.empty()) {
        oss << "\n\nDescription: " << trimmed_desc;
    }
    oss << "\nUse skill_view(name=\"" << meta.name
        << "\") to load the full SKILL.md if you need details beyond the description.";

    std::string trimmed_args = strip(args);
    if (!trimmed_args.empty()) {
        oss << "\n\nUser's request: " << trimmed_args;
    }

    return oss.str();
}

} // namespace acecode
