#include "syntax_highlight.hpp"
#include "tui/theme_palette.hpp"
#include <regex>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace ftxui;

namespace acecode::markdown {

// ---------------------------------------------------------------------------
// Language alias mapping
// ---------------------------------------------------------------------------

static const std::unordered_map<std::string, std::string> kAliasMap = {
    {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"h", "cpp"}, {"hpp", "cpp"},
    {"py", "python"}, {"python3", "python"},
    {"js", "javascript"}, {"jsx", "javascript"}, {"mjs", "javascript"},
    {"ts", "typescript"}, {"tsx", "typescript"}, {"mts", "typescript"},
    {"sh", "shell"}, {"bash", "shell"}, {"zsh", "shell"}, {"fish", "shell"},
    {"yml", "yaml"},
    {"rs", "rust"},
    {"rb", "ruby"},
    {"md", "markdown"},
    {"dockerfile", "docker"},
    {"makefile", "make"},
};

std::string normalize_language(const std::string& lang) {
    std::string lower = lang;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = kAliasMap.find(lower);
    if (it != kAliasMap.end()) return it->second;
    return lower;
}

// ---------------------------------------------------------------------------
// Keyword sets per language
// ---------------------------------------------------------------------------

struct LanguageDef {
    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> types;
    std::string line_comment;          // e.g., "//"
    std::string block_comment_start;   // e.g., "/*"
    std::string block_comment_end;     // e.g., "*/"
    bool has_single_quote_strings = true;
    bool has_double_quote_strings = true;
    bool has_backtick_strings = false;
    bool has_hash_comment = false;
    std::string preproc_prefix;        // e.g., "#" for C/C++
};

static const std::unordered_map<std::string, LanguageDef>& get_language_defs() {
    static const std::unordered_map<std::string, LanguageDef> defs = {
        {"cpp", {
            {"auto", "break", "case", "catch", "class", "const", "constexpr",
             "continue", "default", "delete", "do", "else", "enum", "explicit",
             "extern", "false", "for", "friend", "goto", "if", "inline",
             "namespace", "new", "noexcept", "nullptr", "operator", "override",
             "private", "protected", "public", "return", "sizeof", "static",
             "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
             "struct", "switch", "template", "this", "throw", "true", "try",
             "typedef", "typeid", "typename", "union", "using", "virtual",
             "void", "volatile", "while", "co_await", "co_yield", "co_return",
             "concept", "requires", "consteval", "constinit", "module", "import", "export"},
            {"int", "long", "short", "char", "float", "double", "bool",
             "unsigned", "signed", "size_t", "int8_t", "int16_t", "int32_t",
             "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
             "string", "vector", "map", "set", "unordered_map", "unordered_set",
             "shared_ptr", "unique_ptr", "weak_ptr", "optional", "variant",
             "string_view", "span", "array", "pair", "tuple"},
            "//", "/*", "*/", true, true, false, false, "#"
        }},
        {"c", {
            {"auto", "break", "case", "const", "continue", "default", "do",
             "else", "enum", "extern", "for", "goto", "if", "inline",
             "register", "restrict", "return", "sizeof", "static", "struct",
             "switch", "typedef", "union", "volatile", "while", "NULL"},
            {"int", "long", "short", "char", "float", "double", "void",
             "unsigned", "signed", "size_t", "FILE", "bool"},
            "//", "/*", "*/", true, true, false, false, "#"
        }},
        {"python", {
            {"and", "as", "assert", "async", "await", "break", "class",
             "continue", "def", "del", "elif", "else", "except", "finally",
             "for", "from", "global", "if", "import", "in", "is", "lambda",
             "nonlocal", "not", "or", "pass", "raise", "return", "try",
             "while", "with", "yield", "True", "False", "None"},
            {"int", "float", "str", "bool", "list", "dict", "set", "tuple",
             "bytes", "bytearray", "type", "object", "Exception"},
            "#", "", "", true, true, false, true, ""
        }},
        {"javascript", {
            {"async", "await", "break", "case", "catch", "class", "const",
             "continue", "debugger", "default", "delete", "do", "else",
             "export", "extends", "finally", "for", "function", "if", "import",
             "in", "instanceof", "let", "new", "of", "return", "super",
             "switch", "this", "throw", "try", "typeof", "var", "void",
             "while", "with", "yield", "true", "false", "null", "undefined",
             "NaN", "Infinity"},
            {"Array", "Boolean", "Date", "Error", "Function", "Map", "Number",
             "Object", "Promise", "RegExp", "Set", "String", "Symbol",
             "WeakMap", "WeakSet", "BigInt"},
            "//", "/*", "*/", true, true, true, false, ""
        }},
        {"typescript", {
            {"abstract", "any", "as", "asserts", "async", "await", "bigint",
             "boolean", "break", "case", "catch", "class", "const", "continue",
             "debugger", "declare", "default", "delete", "do", "else", "enum",
             "export", "extends", "finally", "for", "from", "function", "get",
             "if", "implements", "import", "in", "infer", "instanceof",
             "interface", "is", "keyof", "let", "module", "namespace", "never",
             "new", "null", "number", "object", "of", "override", "package",
             "private", "protected", "public", "readonly", "return", "require",
             "set", "static", "string", "super", "switch", "symbol", "this",
             "throw", "true", "false", "try", "type", "typeof", "undefined",
             "unique", "unknown", "var", "void", "while", "with", "yield"},
            {"Array", "Boolean", "Date", "Error", "Function", "Map", "Number",
             "Object", "Promise", "RegExp", "Set", "String", "Symbol",
             "Record", "Partial", "Required", "Readonly", "Pick", "Omit",
             "Exclude", "Extract", "NonNullable", "ReturnType"},
            "//", "/*", "*/", true, true, true, false, ""
        }},
        {"shell", {
            {"if", "then", "else", "elif", "fi", "for", "while", "do", "done",
             "case", "esac", "in", "function", "return", "exit", "break",
             "continue", "local", "export", "source", "alias", "unalias",
             "readonly", "declare", "typeset", "select", "until", "shift",
             "trap", "eval", "exec", "set", "unset"},
            {"echo", "cd", "ls", "rm", "cp", "mv", "mkdir", "rmdir", "cat",
             "grep", "sed", "awk", "find", "xargs", "sort", "uniq", "wc",
             "head", "tail", "tee", "cut", "tr", "chmod", "chown", "curl",
             "wget", "git", "docker", "npm", "pip"},
            "#", "", "", true, true, false, true, ""
        }},
        {"json", {
            {"true", "false", "null"},
            {},
            "", "", "", false, true, false, false, ""
        }},
        {"yaml", {
            {"true", "false", "null", "yes", "no", "on", "off"},
            {},
            "#", "", "", true, true, false, true, ""
        }},
        {"go", {
            {"break", "case", "chan", "const", "continue", "default", "defer",
             "else", "fallthrough", "for", "func", "go", "goto", "if",
             "import", "interface", "map", "package", "range", "return",
             "select", "struct", "switch", "type", "var", "true", "false",
             "nil", "iota", "append", "cap", "close", "complex", "copy",
             "delete", "imag", "len", "make", "new", "panic", "print",
             "println", "real", "recover"},
            {"bool", "byte", "complex64", "complex128", "error", "float32",
             "float64", "int", "int8", "int16", "int32", "int64", "rune",
             "string", "uint", "uint8", "uint16", "uint32", "uint64",
             "uintptr", "any"},
            "//", "/*", "*/", true, true, true, false, ""
        }},
        {"rust", {
            {"as", "async", "await", "break", "const", "continue", "crate",
             "dyn", "else", "enum", "extern", "false", "fn", "for", "if",
             "impl", "in", "let", "loop", "match", "mod", "move", "mut",
             "pub", "ref", "return", "self", "Self", "static", "struct",
             "super", "trait", "true", "type", "unsafe", "use", "where",
             "while", "yield", "macro_rules"},
            {"bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128",
             "isize", "str", "u8", "u16", "u32", "u64", "u128", "usize",
             "String", "Vec", "Box", "Rc", "Arc", "Cell", "RefCell",
             "Option", "Result", "HashMap", "HashSet", "BTreeMap"},
            "//", "/*", "*/", true, true, false, false, ""
        }},
    };
    return defs;
}

bool supports_language(const std::string& language) {
    auto norm = normalize_language(language);
    return get_language_defs().count(norm) > 0;
}

// ---------------------------------------------------------------------------
// Token-based highlighter
// ---------------------------------------------------------------------------

enum class HLTokenType { Keyword, Type, String, Number, Comment, Preproc, Operator, Plain };

struct HLToken {
    HLTokenType type;
    std::string text;
};

static Color hl_color(HLTokenType type) {
    const auto& s = acecode::tui::theme().syntax;
    switch (type) {
        case HLTokenType::Keyword:  return s.keyword;
        case HLTokenType::Type:     return s.type;
        case HLTokenType::String:   return s.string;
        case HLTokenType::Number:   return s.number;
        case HLTokenType::Comment:  return s.comment;
        case HLTokenType::Preproc:  return s.preproc;
        case HLTokenType::Operator: return s.op;
        default:                    return s.default_text;
    }
}

static bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Tokenize a single line given a language definition
// `in_block_comment` persists across lines for multi-line /* */ comments
static std::vector<HLToken> tokenize_line(const std::string& line,
                                           const LanguageDef& def,
                                           bool& in_block_comment) {
    std::vector<HLToken> tokens;
    size_t pos = 0;

    auto push = [&](HLTokenType type, const std::string& text) {
        if (!tokens.empty() && tokens.back().type == type) {
            tokens.back().text += text;
        } else {
            tokens.push_back({type, text});
        }
    };

    while (pos < line.size()) {
        // Block comment continuation
        if (in_block_comment) {
            if (!def.block_comment_end.empty()) {
                auto end_pos = line.find(def.block_comment_end, pos);
                if (end_pos != std::string::npos) {
                    push(HLTokenType::Comment,
                         line.substr(pos, end_pos + def.block_comment_end.size() - pos));
                    pos = end_pos + def.block_comment_end.size();
                    in_block_comment = false;
                    continue;
                }
            }
            push(HLTokenType::Comment, line.substr(pos));
            return tokens;
        }

        // Line comment
        if (!def.line_comment.empty() &&
            line.compare(pos, def.line_comment.size(), def.line_comment) == 0) {
            push(HLTokenType::Comment, line.substr(pos));
            return tokens;
        }

        // Hash comment
        if (def.has_hash_comment && line[pos] == '#') {
            push(HLTokenType::Comment, line.substr(pos));
            return tokens;
        }

        // Block comment start
        if (!def.block_comment_start.empty() &&
            line.compare(pos, def.block_comment_start.size(), def.block_comment_start) == 0) {
            in_block_comment = true;
            auto end_pos = line.find(def.block_comment_end, pos + def.block_comment_start.size());
            if (end_pos != std::string::npos) {
                push(HLTokenType::Comment,
                     line.substr(pos, end_pos + def.block_comment_end.size() - pos));
                pos = end_pos + def.block_comment_end.size();
                in_block_comment = false;
                continue;
            }
            push(HLTokenType::Comment, line.substr(pos));
            return tokens;
        }

        // Preprocessor (must be first non-space on line)
        if (!def.preproc_prefix.empty() &&
            line.compare(pos, def.preproc_prefix.size(), def.preproc_prefix) == 0) {
            // Check if all chars before pos are spaces
            bool first_nonspace = true;
            for (size_t j = 0; j < pos; j++) {
                if (line[j] != ' ' && line[j] != '\t') { first_nonspace = false; break; }
            }
            if (first_nonspace) {
                push(HLTokenType::Preproc, line.substr(pos));
                return tokens;
            }
        }

        // Strings
        if ((def.has_double_quote_strings && line[pos] == '"') ||
            (def.has_single_quote_strings && line[pos] == '\'') ||
            (def.has_backtick_strings && line[pos] == '`')) {
            char quote = line[pos];
            std::string str_text;
            str_text += quote;
            pos++;
            while (pos < line.size()) {
                if (line[pos] == '\\' && pos + 1 < line.size()) {
                    str_text += line[pos];
                    str_text += line[pos + 1];
                    pos += 2;
                    continue;
                }
                str_text += line[pos];
                if (line[pos] == quote) {
                    pos++;
                    break;
                }
                pos++;
            }
            push(HLTokenType::String, str_text);
            continue;
        }

        // Numbers (simple: digits, optionally with . and hex prefix)
        if (std::isdigit(static_cast<unsigned char>(line[pos])) ||
            (line[pos] == '.' && pos + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[pos + 1])))) {
            std::string num;
            // Hex prefix
            if (line[pos] == '0' && pos + 1 < line.size() && (line[pos + 1] == 'x' || line[pos + 1] == 'X')) {
                num += line[pos]; num += line[pos + 1]; pos += 2;
                while (pos < line.size() && std::isxdigit(static_cast<unsigned char>(line[pos]))) {
                    num += line[pos]; pos++;
                }
            } else {
                while (pos < line.size() && (std::isdigit(static_cast<unsigned char>(line[pos])) || line[pos] == '.' || line[pos] == 'e' || line[pos] == 'E')) {
                    num += line[pos]; pos++;
                }
            }
            // Optional suffix (f, l, u, etc.)
            while (pos < line.size() && (line[pos] == 'f' || line[pos] == 'F' ||
                   line[pos] == 'l' || line[pos] == 'L' || line[pos] == 'u' || line[pos] == 'U')) {
                num += line[pos]; pos++;
            }
            push(HLTokenType::Number, num);
            continue;
        }

        // Identifiers (keywords, types, or plain)
        if (is_word_char(line[pos])) {
            std::string word;
            while (pos < line.size() && is_word_char(line[pos])) {
                word += line[pos]; pos++;
            }
            if (def.keywords.count(word)) {
                push(HLTokenType::Keyword, word);
            } else if (def.types.count(word)) {
                push(HLTokenType::Type, word);
            } else {
                push(HLTokenType::Plain, word);
            }
            continue;
        }

        // Operators and punctuation
        push(HLTokenType::Plain, std::string(1, line[pos]));
        pos++;
    }

    return tokens;
}

// Highlight for JSON specifically (key/value distinction)
static std::vector<HLToken> tokenize_json_line(const std::string& line) {
    std::vector<HLToken> tokens;
    size_t pos = 0;

    auto push = [&](HLTokenType type, const std::string& text) {
        if (!tokens.empty() && tokens.back().type == type) {
            tokens.back().text += text;
        } else {
            tokens.push_back({type, text});
        }
    };

    while (pos < line.size()) {
        // Skip whitespace
        if (line[pos] == ' ' || line[pos] == '\t') {
            push(HLTokenType::Plain, std::string(1, line[pos]));
            pos++;
            continue;
        }

        // Strings
        if (line[pos] == '"') {
            std::string str;
            str += '"';
            pos++;
            while (pos < line.size()) {
                if (line[pos] == '\\' && pos + 1 < line.size()) {
                    str += line[pos]; str += line[pos + 1]; pos += 2;
                    continue;
                }
                str += line[pos];
                if (line[pos] == '"') { pos++; break; }
                pos++;
            }
            // Check if followed by : (it's a key)
            size_t look = pos;
            while (look < line.size() && (line[look] == ' ' || line[look] == '\t')) look++;
            if (look < line.size() && line[look] == ':') {
                push(HLTokenType::Type, str); // Keys as cyan
            } else {
                push(HLTokenType::String, str);
            }
            continue;
        }

        // Numbers
        if (std::isdigit(static_cast<unsigned char>(line[pos])) || line[pos] == '-') {
            std::string num;
            if (line[pos] == '-') { num += '-'; pos++; }
            while (pos < line.size() && (std::isdigit(static_cast<unsigned char>(line[pos])) ||
                   line[pos] == '.' || line[pos] == 'e' || line[pos] == 'E' ||
                   line[pos] == '+' || line[pos] == '-')) {
                num += line[pos]; pos++;
            }
            push(HLTokenType::Number, num);
            continue;
        }

        // Keywords: true, false, null
        if (line.compare(pos, 4, "true") == 0 && (pos + 4 >= line.size() || !is_word_char(line[pos + 4]))) {
            push(HLTokenType::Keyword, "true"); pos += 4; continue;
        }
        if (line.compare(pos, 5, "false") == 0 && (pos + 5 >= line.size() || !is_word_char(line[pos + 5]))) {
            push(HLTokenType::Keyword, "false"); pos += 5; continue;
        }
        if (line.compare(pos, 4, "null") == 0 && (pos + 4 >= line.size() || !is_word_char(line[pos + 4]))) {
            push(HLTokenType::Keyword, "null"); pos += 4; continue;
        }

        push(HLTokenType::Plain, std::string(1, line[pos]));
        pos++;
    }
    return tokens;
}

// Convert HLToken list to a single FTXUI Element (hbox of colored text)
static Element tokens_to_element(const std::vector<HLToken>& tokens) {
    if (tokens.empty()) return text("");
    Elements elems;
    for (const auto& tok : tokens) {
        elems.push_back(text(tok.text) | color(hl_color(tok.type)));
    }
    return hbox(std::move(elems));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<Element> highlight_code(const std::string& code,
                                    const std::string& language) {
    auto norm = normalize_language(language);
    std::vector<Element> output;

    // Split code into lines
    std::istringstream stream(code);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    // JSON has special tokenizer
    if (norm == "json") {
        for (const auto& l : lines) {
            output.push_back(tokens_to_element(tokenize_json_line(l)));
        }
        return output;
    }

    // Generic language tokenizer
    auto& defs = get_language_defs();
    auto it = defs.find(norm);
    if (it == defs.end()) {
        // No highlighting available — plain text
        for (const auto& l : lines) {
            output.push_back(text(l) | color(acecode::tui::theme().syntax.default_text));
        }
        return output;
    }

    const auto& def = it->second;
    bool in_block_comment = false;
    for (const auto& l : lines) {
        output.push_back(tokens_to_element(tokenize_line(l, def, in_block_comment)));
    }
    return output;
}

} // namespace acecode::markdown
