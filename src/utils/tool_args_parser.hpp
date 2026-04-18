#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <optional>

namespace acecode {

// Unified argument parser for tool implementations
class ToolArgsParser {
public:
    explicit ToolArgsParser(const std::string& json_str) {
        try {
            args_ = nlohmann::json::parse(json_str);
        } catch (const nlohmann::json::parse_error& e) {
            error_ = "Failed to parse tool arguments: " + std::string(e.what());
        }
    }

    template<typename T>
    std::optional<T> get(const std::string& key) const {
        if (has_error() || !args_.contains(key)) {
            return std::nullopt;
        }
        try {
            return args_[key].get<T>();
        } catch (...) {
            return std::nullopt;
        }
    }

    template<typename T>
    T get_or(const std::string& key, const T& default_val) const {
        auto result = get<T>(key);
        return result.has_value() ? *result : default_val;
    }

    bool has_error() const { return !error_.empty(); }
    std::string error() const { return error_; }

private:
    nlohmann::json args_;
    std::string error_;
};

} // namespace acecode
