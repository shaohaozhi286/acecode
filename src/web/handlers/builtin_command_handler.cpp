#include "builtin_command_handler.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::web {

namespace {

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

BuiltinCommandHttpParseResult parse_command_text(
    std::string command_text,
    std::string explicit_args,
    std::string display_text) {
    BuiltinCommandHttpParseResult result;
    command_text = trim_ascii(std::move(command_text));
    if (command_text.empty()) {
        result.error = "command required";
        return result;
    }

    if (!command_text.empty() && command_text.front() == '/') {
        command_text.erase(command_text.begin());
    }

    std::string name = command_text;
    std::string tail_args;
    const auto split = command_text.find_first_of(" \t\r\n");
    if (split != std::string::npos) {
        name = command_text.substr(0, split);
        tail_args = trim_ascii(command_text.substr(split + 1));
    }

    name = lower_ascii(trim_ascii(std::move(name)));
    if (name.empty()) {
        result.error = "command required";
        return result;
    }
    if (!is_supported_builtin_command(name)) {
        result.error = "unsupported command";
        result.request.name = name;
        return result;
    }

    result.ok = true;
    result.status = 202;
    result.request.name = std::move(name);
    result.request.args = explicit_args.empty()
        ? std::move(tail_args)
        : trim_ascii(std::move(explicit_args));
    result.request.display_text = display_text.empty()
        ? "/" + result.request.name + (result.request.args.empty() ? "" : " " + result.request.args)
        : std::move(display_text);
    return result;
}

} // namespace

bool is_supported_builtin_command(const std::string& name) {
    return name == "init" || name == "compact" || name == "goal" || name == "plan";
}

BuiltinCommandHttpParseResult parse_builtin_command_request(const std::string& body) {
    BuiltinCommandHttpParseResult result;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        result.error = std::string("bad json: ") + e.what();
        return result;
    }

    std::string command_text;
    if (j.contains("command") && j["command"].is_string()) {
        command_text = j["command"].get<std::string>();
    } else if (j.contains("name") && j["name"].is_string()) {
        command_text = j["name"].get<std::string>();
    }

    std::string args;
    if (j.contains("args") && j["args"].is_string()) {
        args = j["args"].get<std::string>();
    }

    std::string display_text;
    if (j.contains("display_text") && j["display_text"].is_string()) {
        display_text = j["display_text"].get<std::string>();
    }

    return parse_command_text(std::move(command_text), std::move(args), std::move(display_text));
}

nlohmann::json builtin_command_error_json(const BuiltinCommandHttpParseResult& result) {
    nlohmann::json out{{"error", result.error.empty() ? "invalid command" : result.error}};
    if (!result.request.name.empty()) {
        out["command"] = result.request.name;
    }
    return out;
}

} // namespace acecode::web
