#include "memory_types.hpp"

namespace acecode {

std::string memory_type_to_string(MemoryType t) {
    switch (t) {
        case MemoryType::User:      return "user";
        case MemoryType::Feedback:  return "feedback";
        case MemoryType::Project:   return "project";
        case MemoryType::Reference: return "reference";
    }
    return "user";
}

std::optional<MemoryType> parse_memory_type(const std::string& s) {
    if (s == "user")      return MemoryType::User;
    if (s == "feedback")  return MemoryType::Feedback;
    if (s == "project")   return MemoryType::Project;
    if (s == "reference") return MemoryType::Reference;
    return std::nullopt;
}

} // namespace acecode
