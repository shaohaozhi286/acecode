#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace acecode::upgrade {
namespace {

bool parse_non_negative_int(const std::string& s, int* out) {
    if (s.empty()) return false;
    long long value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
        if (value > 1000000) return false;
    }
    *out = static_cast<int>(value);
    return true;
}

std::vector<std::string> split_dot(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

bool is_numeric_identifier(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

int compare_prerelease(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;
    if (b.empty()) return -1;

    auto as = split_dot(a);
    auto bs = split_dot(b);
    const size_t n = std::min(as.size(), bs.size());
    for (size_t i = 0; i < n; ++i) {
        const bool an = is_numeric_identifier(as[i]);
        const bool bn = is_numeric_identifier(bs[i]);
        if (an && bn) {
            long long av = std::strtoll(as[i].c_str(), nullptr, 10);
            long long bv = std::strtoll(bs[i].c_str(), nullptr, 10);
            if (av < bv) return -1;
            if (av > bv) return 1;
        } else if (an != bn) {
            return an ? -1 : 1;
        } else {
            if (as[i] < bs[i]) return -1;
            if (as[i] > bs[i]) return 1;
        }
    }
    if (as.size() < bs.size()) return -1;
    if (as.size() > bs.size()) return 1;
    return 0;
}

} // namespace

std::optional<SemVersion> parse_sem_version(const std::string& raw) {
    std::string s = raw;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
        s.erase(s.begin());
    }
    const size_t plus = s.find('+');
    if (plus != std::string::npos) {
        s = s.substr(0, plus);
    }

    std::string prerelease;
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        prerelease = s.substr(dash + 1);
        s = s.substr(0, dash);
        if (prerelease.empty()) return std::nullopt;
    }

    auto parts = split_dot(s);
    if (parts.size() != 3) return std::nullopt;

    SemVersion v;
    if (!parse_non_negative_int(parts[0], &v.major) ||
        !parse_non_negative_int(parts[1], &v.minor) ||
        !parse_non_negative_int(parts[2], &v.patch)) {
        return std::nullopt;
    }
    v.prerelease = std::move(prerelease);
    return v;
}

int compare_sem_version(const SemVersion& a, const SemVersion& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return compare_prerelease(a.prerelease, b.prerelease);
}

bool sem_version_less(const SemVersion& a, const SemVersion& b) {
    return compare_sem_version(a, b) < 0;
}

} // namespace acecode::upgrade
