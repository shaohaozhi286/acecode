#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

struct BuiltinCommandHttpRequest {
    std::string name;
    std::string args;
    std::string display_text;
};

struct BuiltinCommandHttpParseResult {
    bool ok = false;
    int status = 400;
    std::string error;
    BuiltinCommandHttpRequest request;
};

bool is_supported_builtin_command(const std::string& name);
BuiltinCommandHttpParseResult parse_builtin_command_request(const std::string& body);
nlohmann::json builtin_command_error_json(const BuiltinCommandHttpParseResult& result);

} // namespace acecode::web
