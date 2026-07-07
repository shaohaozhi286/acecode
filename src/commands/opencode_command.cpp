#include "opencode_command.hpp"

#include "../utils/encoding.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace acecode {

namespace {

namespace fs = std::filesystem;

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string strip_utf8_bom(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

std::string normalize_newlines(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string unquote_scalar(std::string value) {
    value = trim_ascii(std::move(value));
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

bool parse_bool_scalar(const std::string& value, bool* out) {
    if (!out) return false;
    const std::string v = lower_ascii(trim_ascii(value));
    if (v == "true" || v == "yes" || v == "1") {
        *out = true;
        return true;
    }
    if (v == "false" || v == "no" || v == "0") {
        *out = false;
        return true;
    }
    return false;
}

std::optional<std::pair<std::map<std::string, std::string>, std::string>>
parse_frontmatter(std::string content) {
    content = normalize_newlines(strip_utf8_bom(std::move(content)));
    std::size_t line_start = 0;
    std::size_t line_end = content.find('\n', line_start);
    std::string first = line_end == std::string::npos
        ? content.substr(line_start)
        : content.substr(line_start, line_end - line_start);
    if (trim_ascii(first) != "---") {
        return std::make_pair(std::map<std::string, std::string>{}, trim_ascii(std::move(content)));
    }

    line_start = line_end == std::string::npos ? content.size() : line_end + 1;
    std::map<std::string, std::string> data;
    while (line_start <= content.size()) {
        line_end = content.find('\n', line_start);
        const std::size_t current_end = line_end == std::string::npos ? content.size() : line_end;
        std::string line = content.substr(line_start, current_end - line_start);
        if (trim_ascii(line) == "---") {
            std::string body;
            if (line_end != std::string::npos) body = content.substr(line_end + 1);
            return std::make_pair(std::move(data), trim_ascii(std::move(body)));
        }

        std::string trimmed = trim_ascii(line);
        if (!trimmed.empty() && trimmed.front() != '#') {
            const auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                std::string key = lower_ascii(trim_ascii(trimmed.substr(0, colon)));
                std::string value = unquote_scalar(trimmed.substr(colon + 1));
                if (!key.empty()) data[std::move(key)] = std::move(value);
            }
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }

    return std::make_pair(std::map<std::string, std::string>{}, trim_ascii(std::move(content)));
}

std::optional<OpencodeCommandInfo> decode_command_file(
    const fs::path& config_root,
    const fs::path& filepath) {
    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto parsed = parse_frontmatter(buffer.str());
    if (!parsed.has_value()) return std::nullopt;

    std::error_code ec;
    fs::path rel = fs::relative(filepath, config_root, ec);
    if (ec || rel.empty()) return std::nullopt;
    std::string name = path_to_utf8_generic(rel);
    if (name.rfind("command/", 0) == 0) {
        name.erase(0, std::string("command/").size());
    } else if (name.rfind("commands/", 0) == 0) {
        name.erase(0, std::string("commands/").size());
    } else {
        return std::nullopt;
    }
    if (name.size() > 3 && name.substr(name.size() - 3) == ".md") {
        name.resize(name.size() - 3);
    }
    if (name.empty()) return std::nullopt;

    OpencodeCommandInfo info;
    info.name = std::move(name);
    info.template_text = parsed->second;
    info.source_path = filepath;

    const auto& data = parsed->first;
    auto get = [&data](const char* key) -> std::string {
        auto it = data.find(key);
        return it == data.end() ? std::string{} : it->second;
    };
    info.description = get("description");
    info.agent = get("agent");
    info.model = get("model");
    info.variant = get("variant");
    auto st = data.find("subtask");
    if (st != data.end()) {
        bool value = false;
        if (parse_bool_scalar(st->second, &value)) {
            info.subtask = value;
            info.has_subtask = true;
        }
    }
    return info;
}

std::string strip_json_comments(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (in_string) {
            out.push_back(ch);
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            out.push_back(ch);
            continue;
        }
        if (ch == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            while (i < input.size() && input[i] != '\n') ++i;
            if (i < input.size()) out.push_back(input[i]);
            continue;
        }
        if (ch == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
            if (i + 1 < input.size()) ++i;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::optional<nlohmann::json> read_json_config_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string content = strip_utf8_bom(buffer.str());
    if (lower_ascii(path_to_utf8(path.extension())) == ".jsonc") {
        content = strip_json_comments(content);
    }
    try {
        auto json = nlohmann::json::parse(content, nullptr, true, true);
        if (json.is_object()) return json;
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<OpencodeCommandInfo> decode_config_command(
    const std::string& name,
    const nlohmann::json& value,
    const fs::path& source_path) {
    if (name.empty() || !value.is_object()) return std::nullopt;
    if (!value.contains("template") || !value["template"].is_string()) return std::nullopt;

    OpencodeCommandInfo info;
    info.name = name;
    info.template_text = trim_ascii(value["template"].get<std::string>());
    info.source_path = source_path;
    auto string_field = [&value](const char* key) -> std::string {
        return value.contains(key) && value[key].is_string()
            ? value[key].get<std::string>()
            : std::string{};
    };
    info.description = string_field("description");
    info.agent = string_field("agent");
    info.model = string_field("model");
    info.variant = string_field("variant");
    if (value.contains("subtask") && value["subtask"].is_boolean()) {
        info.subtask = value["subtask"].get<bool>();
        info.has_subtask = true;
    }
    return info;
}

std::vector<OpencodeCommandInfo> load_json_config_commands(const fs::path& config_root) {
    std::vector<OpencodeCommandInfo> out;
    std::vector<fs::path> candidates;
    if (path_to_utf8(config_root.filename()) == ".opencode") {
        candidates.push_back(config_root.parent_path() / "opencode.json");
        candidates.push_back(config_root.parent_path() / "opencode.jsonc");
    }
    for (const char* filename : {"opencode.json", "opencode.jsonc", "config.json", "config.jsonc"}) {
        candidates.push_back(config_root / filename);
    }

    for (const auto& path : candidates) {
        auto json = read_json_config_file(path);
        if (!json.has_value()) continue;
        for (const char* key : {"command", "commands"}) {
            if (!json->contains(key) || !(*json)[key].is_object()) continue;
            for (const auto& item : (*json)[key].items()) {
                auto info = decode_config_command(item.key(), item.value(), path);
                if (info.has_value()) out.push_back(std::move(*info));
            }
        }
    }
    return out;
}

void append_config_root(std::vector<fs::path>& roots,
                        std::unordered_set<std::string>& seen,
                        const fs::path& root) {
    if (root.empty()) return;
    fs::path normalized = root.lexically_normal();
    const std::string key = path_to_utf8_generic(normalized);
    if (key.empty() || seen.count(key)) return;
    seen.insert(key);
    roots.emplace_back(std::move(normalized));
}

std::string env_or_expanded_path(const char* name, const std::string& fallback) {
    std::string value = getenv_utf8(name);
    if (!value.empty()) return value;
    return expand_path(fallback);
}

std::vector<fs::path> sorted_markdown_files_under(const fs::path& config_root) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const char* folder : {"command", "commands"}) {
        const fs::path root = config_root / folder;
        if (!fs::is_directory(root, ec) || ec) {
            ec.clear();
            continue;
        }
        fs::recursive_directory_iterator it(
            root,
            fs::directory_options::follow_directory_symlink,
            ec);
        fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            std::error_code entry_ec;
            if (it->is_regular_file(entry_ec) && !entry_ec &&
                lower_ascii(path_to_utf8(it->path().extension())) == ".md") {
                files.push_back(it->path());
            }
            it.increment(ec);
        }
        ec.clear();
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return path_to_utf8_generic(a) < path_to_utf8_generic(b);
    });
    return files;
}

std::vector<std::string> split_args(const std::string& raw_args) {
    std::vector<std::string> out;
    std::string cur;
    char quote = '\0';
    bool escape = false;
    for (char ch : raw_args) {
        if (escape) {
            cur.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\' && quote != '\0') {
            escape = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                cur.push_back(ch);
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(ch);
    }
    if (escape) cur.push_back('\\');
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string join_args_from(const std::vector<std::string>& args, std::size_t index) {
    std::string out;
    for (std::size_t i = index; i < args.size(); ++i) {
        if (!out.empty()) out.push_back(' ');
        out += args[i];
    }
    return out;
}

} // namespace

std::vector<std::filesystem::path> opencode_command_config_roots(
    const AppConfig& config,
    const std::string& working_dir) {
    std::vector<fs::path> roots;
    std::unordered_set<std::string> seen;
    if (!config.skills.reuse_opencode) return roots;

    const fs::path xdg_config =
        path_from_utf8(env_or_expanded_path("XDG_CONFIG_HOME", "~/.config"));
    append_config_root(roots, seen, xdg_config / "opencode");
    append_config_root(roots, seen, path_from_utf8(expand_path("~/.opencode")));

    const std::string custom_config = getenv_utf8("OPENCODE_CONFIG_DIR");
    if (!custom_config.empty()) {
        append_config_root(roots, seen, path_from_utf8(custom_config));
    }

    auto project_dirs = get_project_dirs_up_to_home(working_dir);
    std::reverse(project_dirs.begin(), project_dirs.end());
    for (const auto& dir : project_dirs) {
        append_config_root(roots, seen, path_from_utf8(dir) / ".opencode");
    }

    return roots;
}

std::vector<OpencodeCommandInfo> load_opencode_commands_from_config_roots(
    const std::vector<std::filesystem::path>& config_roots) {
    std::map<std::string, OpencodeCommandInfo> by_name;
    for (const auto& root : config_roots) {
        for (auto& info : load_json_config_commands(root)) {
            by_name[info.name] = std::move(info);
        }
        for (const auto& file : sorted_markdown_files_under(root)) {
            auto info = decode_command_file(root, file);
            if (!info.has_value()) continue;
            by_name[info->name] = std::move(*info);
        }
    }

    std::vector<OpencodeCommandInfo> out;
    out.reserve(by_name.size());
    for (auto& [name, info] : by_name) {
        (void)name;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<OpencodeCommandInfo> load_opencode_commands(
    const AppConfig& config,
    const std::string& working_dir) {
    return load_opencode_commands_from_config_roots(
        opencode_command_config_roots(config, working_dir));
}

std::optional<OpencodeCommandInfo> find_opencode_command(
    const AppConfig& config,
    const std::string& working_dir,
    const std::string& name) {
    if (name.empty() || is_web_reserved_builtin_command(name)) return std::nullopt;
    for (auto& command : load_opencode_commands(config, working_dir)) {
        if (command.name == name) return command;
    }
    return std::nullopt;
}

std::optional<ParsedSlashCommand> parse_opencode_slash_command(
    const std::string& input) {
    if (input.empty() || input.front() != '/') return std::nullopt;
    std::size_t i = 1;
    while (i < input.size() && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
    std::string name = input.substr(1, i - 1);
    while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) ++i;
    if (name.empty()) return std::nullopt;
    ParsedSlashCommand parsed;
    parsed.name = std::move(name);
    parsed.args = i < input.size() ? input.substr(i) : std::string{};
    return parsed;
}

std::string expand_opencode_command_template(
    const OpencodeCommandInfo& command,
    const std::string& raw_args) {
    std::string templ = command.template_text;
    const std::vector<std::string> args = split_args(raw_args);

    static const std::regex placeholder_re(R"(\$(\d+))");
    int highest = 0;
    for (std::sregex_iterator it(templ.begin(), templ.end(), placeholder_re), end; it != end; ++it) {
        highest = std::max(highest, std::stoi((*it)[1].str()));
    }

    std::string replaced;
    std::size_t last_pos = 0;
    for (std::sregex_iterator it(templ.begin(), templ.end(), placeholder_re), end; it != end; ++it) {
        const auto& match = *it;
        replaced.append(templ, last_pos, static_cast<std::size_t>(match.position()) - last_pos);
        const int position = std::stoi(match[1].str());
        const std::size_t arg_index = static_cast<std::size_t>(position - 1);
        if (position > 0 && arg_index < args.size()) {
            replaced += position == highest ? join_args_from(args, arg_index) : args[arg_index];
        }
        last_pos = static_cast<std::size_t>(match.position() + match.length());
    }
    replaced.append(templ, last_pos, std::string::npos);

    const bool uses_arguments = replaced.find("$ARGUMENTS") != std::string::npos;
    std::string with_arguments;
    with_arguments.reserve(replaced.size() + raw_args.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = replaced.find("$ARGUMENTS", pos);
        if (found == std::string::npos) {
            with_arguments.append(replaced, pos, std::string::npos);
            break;
        }
        with_arguments.append(replaced, pos, found - pos);
        with_arguments += raw_args;
        pos = found + std::string("$ARGUMENTS").size();
    }

    if (highest == 0 && !uses_arguments && !trim_ascii(raw_args).empty()) {
        if (!with_arguments.empty()) with_arguments += "\n\n";
        with_arguments += raw_args;
    }

    return trim_ascii(std::move(with_arguments));
}

std::string build_opencode_subtask_prompt(
    const OpencodeCommandInfo& command,
    const std::string& expanded_prompt) {
    std::string prompt;
    prompt += "Run the following opencode command as an isolated subagent using the ";
    prompt += "existing `spawn_subagent` tool with `wait` set to true. Do not solve ";
    prompt += "the command in the current session before the subagent returns.\n\n";
    prompt += "Command: /" + command.name + "\n";
    if (!command.description.empty()) {
        prompt += "Description: " + command.description + "\n";
    }
    if (!command.agent.empty()) {
        prompt += "Requested agent: " + command.agent + "\n";
    }
    if (!command.model.empty()) {
        prompt += "Requested model: " + command.model + "\n";
    }
    prompt += "\nSubagent prompt:\n";
    prompt += expanded_prompt;
    return prompt;
}

std::string expand_opencode_command(
    const OpencodeCommandInfo& command,
    const std::string& raw_args,
    const OpencodeCommandExpansionOptions& options) {
    std::string expanded = expand_opencode_command_template(command, raw_args);
    if (command.subtask && options.wrap_subtask_prompt) {
        expanded = build_opencode_subtask_prompt(command, expanded);
    }
    return expanded;
}

bool is_web_reserved_builtin_command(const std::string& name) {
    return name == "init" || name == "compact" ||
           name == "goal" || name == "plan";
}

} // namespace acecode
