#pragma once

#include <optional>
#include <string>

namespace acecode::upgrade {

struct SemVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;
};

std::optional<SemVersion> parse_sem_version(const std::string& raw);
int compare_sem_version(const SemVersion& a, const SemVersion& b);
bool sem_version_less(const SemVersion& a, const SemVersion& b);

} // namespace acecode::upgrade
