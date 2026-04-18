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

std::string build_activation_message(const SkillMetadata& meta,
                                     const std::string& body,
                                     const std::vector<std::string>& supporting_files,
                                     const std::string& user_arg) {
    std::ostringstream oss;

    oss << "[SYSTEM: The user has invoked the \"" << meta.name
        << "\" skill, indicating they want you to follow its instructions. "
        << "The full skill content is loaded below.]";

    std::string trimmed_body = strip(body);
    if (!trimmed_body.empty()) {
        oss << "\n\n" << trimmed_body;
    }

    if (!supporting_files.empty()) {
        oss << "\n\n[This skill has supporting files you can load with the skill_view tool:]";
        for (const auto& f : supporting_files) {
            oss << "\n- " << f;
        }
        oss << "\n\nTo view any of these, use: skill_view(name=\""
            << meta.name << "\", file_path=\"<path>\")";
    }

    std::string trimmed_arg = strip(user_arg);
    if (!trimmed_arg.empty()) {
        oss << "\n\nThe user has provided the following instruction alongside the skill invocation: "
            << trimmed_arg;
    }

    return oss.str();
}

} // namespace acecode
